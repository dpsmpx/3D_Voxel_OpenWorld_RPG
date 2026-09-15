/**
 * @file orientation.h
 * @brief Ориентация сущностей: единственный источник истины.
 */
#pragma once
#include "types.h"
#include <glm/glm.hpp>
#include <cmath>

namespace orient {

// ============================================================
// Соглашение
// ============================================================
//
// Мир правосторонний, +Y вверх.
// У модели +Z — «вперёд», +X — «вправо», +Y — «вверх».
// Поворот сущности — один угол yaw вокруг +Y.
// yaw = 0 смотрит в +Z, угол растёт К +X.
//
// Отсюда ровно одна допустимая матрица поворота, и она обязана быть
// одной и той же на процессоре и в шейдере.
//
// Так было НЕ всегда. Соглашение задавала строка
// `yaw = atan2(vel.x, vel.z)`, шейдер mob.vert ему следовал, а
// mob_renderer.cpp и npc_renderer.cpp применяли поворот
// ПРОТИВОПОЛОЖНОЙ ручности. Совпадение получалось только при движении
// вдоль Z; при любой составляющей по X положения частей зеркалились,
// а геометрия частей — нет, и существо собиралось наизнанку. Это и
// был «идёт боком».

/// Направление, в которое смотрит yaw.
inline glm::vec3 forward(f32 yaw) {
    return { std::sin(yaw), 0.f, std::cos(yaw) };
}

/// Направление «вправо» от yaw.
inline glm::vec3 right(f32 yaw) {
    return { std::cos(yaw), 0.f, -std::sin(yaw) };
}

/// Угол, смотрящий вдоль горизонтального направления.
/// Нулевой вектор даёт 0 — вызывающий обязан проверять значимость.
inline f32 yawFromDirection(f32 dx, f32 dz) {
    return std::atan2(dx, dz);
}

/// ЕДИНСТВЕННЫЙ поворот вокруг +Y, применяемый на процессоре.
///
/// Обязан совпадать с mob.vert дословно:
///   vec3(local.x*c + local.z*s, local.y, -local.x*s + local.z*c)
/// За этим следит проверка: она читает шейдер и сверяет выражение.
inline glm::vec3 rotateY(const glm::vec3& v, f32 yaw) {
    const f32 c = std::cos(yaw), s = std::sin(yaw);
    return { v.x * c + v.z * s, v.y, -v.x * s + v.z * c };
}

/// Разница углов, приведённая к (-pi, pi].
///
/// Нужна для доворота по КРАТЧАЙШЕЙ дуге: без приведения существо,
/// поворачиваясь с 170° на -170°, поедет через весь круг вместо
/// двадцати градусов.
inline f32 angleDelta(f32 from, f32 to) {
    constexpr f32 TAU = 6.28318530718f;
    f32 d = std::fmod(to - from + 3.14159265359f, TAU);
    if (d < 0.f) d += TAU;
    return d - 3.14159265359f;
}

/// Довернуть `current` к `target` не быстрее `maxRate` рад/с.
inline f32 turnToward(f32 current, f32 target, f32 maxRate, f32 dt) {
    const f32 d = angleDelta(current, target);
    const f32 step = maxRate * dt;
    if (d >  step) return current + step;
    if (d < -step) return current - step;
    return target;
}

/// Ниже этой скорости направление движения считается незначимым.
///
/// Прежний код при скорости ниже порога ОБНУЛЯЛ yaw — существо
/// мгновенно разворачивалось на север, стоило ему остановиться.
/// Правильное поведение: сохранить прежнее направление.
constexpr f32 MOVE_EPSILON = 0.05f;

/// Продвинуть ориентацию сущности за кадр.
///
/// Правила (docs/ENTITY_VISUAL_ARCHITECTURE.md, 6.2):
///   * направление движения берётся из скорости, но только когда она
///     значима; иначе сохраняется прежнее;
///   * yaw ДОГОНЯЕТ направление движения с ограниченной угловой
///     скоростью, по кратчайшей дуге;
///   * при остановке yaw не меняется: существо остаётся смотреть
///     туда, куда шло.
template <class FacingT>
inline void advanceFacing(FacingT& f, const glm::vec3& velocity, f32 dt) {
    const f32 speedSq = velocity.x * velocity.x + velocity.z * velocity.z;
    if (speedSq > MOVE_EPSILON * MOVE_EPSILON)
        f.moveYaw = yawFromDirection(velocity.x, velocity.z);

    f.yaw = turnToward(f.yaw, f.moveYaw, f.turnRate, dt);
}

} // namespace orient
