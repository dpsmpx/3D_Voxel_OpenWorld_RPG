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

// ============================================================
// Кватернионы
// ============================================================
//
// Инстанс несёт поворот кватернионом: одним углом повёрнутую в
// суставе конечность выразить нельзя. Строятся кватернионы ЗДЕСЬ,
// по тому же соглашению, что и rotateY, — чтобы «вперёд» значило
// одно и то же и в матрице, и в кватернионе.

/// Кватернион поворота вокруг +Y на угол yaw — (x, y, z, w).
///
/// Совпадает с rotateY: qrot(yawQuat(a), v) == rotateY(v, a).
inline glm::vec4 yawQuat(f32 yaw) {
    return { 0.f, std::sin(yaw * 0.5f), 0.f, std::cos(yaw * 0.5f) };
}

/// Кватернион поворота вокруг оси `axis` (любой длины) на угол.
inline glm::vec4 axisQuat(const glm::vec3& axis, f32 angle) {
    const f32 len = std::sqrt(axis.x * axis.x + axis.y * axis.y + axis.z * axis.z);
    if (len < 1e-6f) return { 0.f, 0.f, 0.f, 1.f };
    const f32 s = std::sin(angle * 0.5f) / len;
    return { axis.x * s, axis.y * s, axis.z * s, std::cos(angle * 0.5f) };
}

/// Произведение кватернионов: сперва поворот b, потом a.
inline glm::vec4 qmul(const glm::vec4& a, const glm::vec4& b) {
    return { a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
             a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
             a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w,
             a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z };
}

/// Поворот вектора кватернионом — дословно как qrot в шейдерах.
inline glm::vec3 qrot(const glm::vec4& q, const glm::vec3& v) {
    const glm::vec3 u { q.x, q.y, q.z };
    return v + 2.f * glm::cross(u, glm::cross(u, v) + q.w * v);
}

/// Кратчайший поворот, переводящий модельное «вперёд» (+Z) в `dir`.
///
/// Нужен там, где движение не горизонтально: стрела с гравитацией
/// летит вниз, и одного yaw ей мало — она смотрела бы в горизонт,
/// падая отвесно. Нулевой или почти нулевой вектор даёт единичный
/// кватернион: направления нет, значит и поворачивать не на что.
inline glm::vec4 dirQuat(const glm::vec3& dir) {
    const f32 len = std::sqrt(dir.x * dir.x + dir.y * dir.y + dir.z * dir.z);
    if (len < 1e-6f) return { 0.f, 0.f, 0.f, 1.f };
    const glm::vec3 d = dir / len;

    // w = 1 + dot(+Z, d) = 1 + d.z. Обращается в ноль ровно когда
    // направление строго противоположно +Z: тогда ось кратчайшей
    // дуги не определена, и берём любую перпендикулярную — +Y.
    const f32 w = 1.f + d.z;
    if (w < 1e-6f) return { 0.f, 1.f, 0.f, 0.f };

    // cross(+Z, d) = (0,0,1) x (dx,dy,dz) = (-dy, dx, 0).
    glm::vec4 q { -d.y, d.x, 0.f, w };
    const f32 n = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
    return q / n;
}

} // namespace orient
