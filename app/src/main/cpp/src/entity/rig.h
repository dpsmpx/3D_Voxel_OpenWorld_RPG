/**
 * @file rig.h
 * @brief Оснастка сущности: иерархия частей и поза.
 */
#pragma once
#include "../core/types.h"
#include "../core/orientation.h"
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <algorithm>
#include <cmath>

namespace entity {

// ============================================================
// Зачем это есть
// ============================================================
//
// Прежняя оснастка была плоским списком коробок со смещением от
// начала сущности. Ни родителя, ни собственной системы координат у
// части. Собрать «торс → бедро → голень» было не на чем, и анимация
// СДВИГАЛА коробку вперёд-назад вместо поворота в суставе: нога ехала
// параллельно себе, отсюда «плывущая» походка.
//
// Причина была глубже лени. Формат инстанса нёс ровно один угол на
// всю коробку — повёрнутую конечность он физически не мог выразить.
// Поэтому вместе с иерархией меняется и формат: часть получает
// кватернион.

constexpr u8 MAX_PARTS = 20;

/// Что это за часть. Нужно анимации: она вращает суставы по роли, а
/// не по номеру, и одна и та же походка ложится на разные скелеты.
enum class PartRole : u8 {
    Root = 0,     ///< таз/корпус целиком
    Torso,
    Head,
    Ear,
    Snout,
    UpperArmL, UpperArmR,
    LowerArmL, LowerArmR,
    HandL,     HandR,
    UpperLegFL, UpperLegFR, UpperLegBL, UpperLegBR,
    LowerLegFL, LowerLegFR, LowerLegBL, LowerLegBR,
    Tail,
    Wing,
    Horn,
    Prop,         ///< оружие, поклажа — крепится, но не анимируется
    Count
};

/// Часть оснастки.
///
/// `pivot` — положение СУСТАВА в системе родителя. Вокруг него часть
/// и вращается. `boxOffset` — смещение центра коробки от сустава:
/// бедро вращается у таза, а коробка бедра висит ниже.
struct Part {
    i8        parent    = -1;      ///< -1 — крепится к корню сущности
    PartRole  role      = PartRole::Root;
    glm::vec3 pivot{0};
    glm::vec3 boxOffset{0};
    glm::vec3 size{0};
    u32       color     = 0xFFFFFFFFu;
    /// Часть без коробки (чистый сустав) не рисуется.
    bool visible = true;
};

/// Оснастка — описание, общее для всех особей вида.
struct Rig {
    Part parts[MAX_PARTS]{};
    u8   count = 0;

    /// На сколько поднять оснастку, чтобы она встала на опору.
    ///
    /// Начало сущности совпадает с точкой опоры: контроллер ставит
    /// position.y ровно на пол. Значит низ модели в покое обязан быть
    /// у нуля — и сборщик оснастки выводит это значение из геометрии,
    /// одним правилом для всех видов. Плоские определения выдерживали
    /// его не везде: Каменный страж стоял по щиколотку в полу.
    ///
    /// Летающие и плавающие переопределяют поле явно, после сборки.
    /// Это единственное законное место для такой поправки: подгонять
    /// высоту в рендере нельзя.
    f32 groundOffset = 0.f;

    /// Путь, который модель проходит за полный цикл шага, метры.
    ///
    /// Свойство модели, а не состояния: у курицы и у коровы он разный
    /// просто потому, что ноги разной длины. Выводится из геометрии
    /// при сборке оснастки, чтобы фазу шага можно было считать путём,
    /// а не подбирать множитель к времени.
    f32 strideLength = 1.6f;

    /// Поправка ориентации модели, если её «вперёд» не совпадает с +Z.
    ///
    /// Единственное законное место для такой поправки. Прятать её в
    /// рендере или в ИИ нельзя: именно так и появляются «этому мобу
    /// +90 градусов».
    f32 modelYawOffset = 0.f;

    u8 add(const Part& p) {
        if (count >= MAX_PARTS) return (u8)(count - 1);
        parts[count] = p;
        return count++;
    }
};

/// Поза — поворот каждого сустава в системе родителя.
///
/// Углы, а не кватернионы: анимация задаёт качание в одной плоскости,
/// и три угла читаются человеком, а четыре компоненты кватерниона —
/// нет.
struct Pose {
    glm::vec3 euler[MAX_PARTS]{};   ///< pitch (X), yaw (Y), roll (Z)

    void clear() {
        for (u8 i = 0; i < MAX_PARTS; ++i) euler[i] = glm::vec3(0.f);
    }
};

/// Разрешённая часть: где коробка в мире и как повёрнута.
struct ResolvedPart {
    glm::vec3 center{0};
    glm::vec3 size{0};
    glm::quat rot{1, 0, 0, 0};
    u32       color = 0xFFFFFFFFu;
};

/// Кватернион сустава из углов позы.
inline glm::quat jointRotation(const glm::vec3& e) {
    // Порядок Y→X→Z: сначала разворот, потом качание, потом наклон.
    // Для конечностей значим только X, поэтому порядок почти не
    // виден; для головы важно, чтобы разворот шёл первым.
    return glm::angleAxis(e.y, glm::vec3(0, 1, 0)) *
           glm::angleAxis(e.x, glm::vec3(1, 0, 0)) *
           glm::angleAxis(e.z, glm::vec3(0, 0, 1));
}

/// Собрать позу в мировые коробки.
///
/// Части обязаны идти так, чтобы родитель стоял РАНЬШЕ ребёнка —
/// тогда обход в один проход, без рекурсии и без сортировки. За этим
/// следит проверка.
///
/// Возвращает число заполненных элементов `out`.
inline u8 resolve(const Rig& rig, const Pose& pose,
                  const glm::vec3& rootPos, f32 rootYaw,
                  ResolvedPart* out, u8 outMax)
{
    glm::quat worldRot[MAX_PARTS];
    glm::vec3 worldPivot[MAX_PARTS];

    const glm::quat base =
        glm::angleAxis(rootYaw + rig.modelYawOffset, glm::vec3(0, 1, 0));
    const glm::vec3 origin = rootPos + glm::vec3(0.f, rig.groundOffset, 0.f);

    u8 written = 0;
    for (u8 i = 0; i < rig.count && i < MAX_PARTS; ++i) {
        const Part& p = rig.parts[i];

        const glm::quat parentRot = (p.parent >= 0 && p.parent < (i8)i)
                                  ? worldRot[p.parent] : base;
        const glm::vec3 parentPos = (p.parent >= 0 && p.parent < (i8)i)
                                  ? worldPivot[p.parent] : origin;

        worldRot[i]   = parentRot * jointRotation(pose.euler[i]);
        worldPivot[i] = parentPos + parentRot * p.pivot;

        if (!p.visible || written >= outMax) continue;
        out[written].center = worldPivot[i] + worldRot[i] * p.boxOffset;
        out[written].size   = p.size;
        out[written].rot    = worldRot[i];
        out[written].color  = p.color;
        ++written;
    }
    return written;
}

/// Нижняя точка оснастки в позе — для проверки опоры.
inline f32 lowestPoint(const Rig& rig, const Pose& pose, f32 rootYaw) {
    ResolvedPart tmp[MAX_PARTS];
    const u8 n = resolve(rig, pose, glm::vec3(0.f), rootYaw, tmp, MAX_PARTS);
    f32 lo = 0.f;
    bool first = true;
    for (u8 i = 0; i < n; ++i) {
        // Коробка повёрнута, поэтому берём её габарит по Y честно.
        const glm::vec3 h = tmp[i].size * 0.5f;
        f32 ext = 0.f;
        for (int sx = -1; sx <= 1; sx += 2)
        for (int sy = -1; sy <= 1; sy += 2)
        for (int sz = -1; sz <= 1; sz += 2) {
            const glm::vec3 c = tmp[i].rot *
                glm::vec3(h.x * (f32)sx, h.y * (f32)sy, h.z * (f32)sz);
            if (c.y < ext) ext = c.y;
        }
        const f32 bottom = tmp[i].center.y + ext;
        if (first || bottom < lo) { lo = bottom; first = false; }
    }
    return lo;
}

/// Длина шага по геометрии оснастки.
///
/// Шаг соразмерен ноге: у курицы и у коровы он разный просто потому,
/// что ноги разной длины. За полный цикл нога делает два шага, каждый
/// примерно в 0.8 своей длины — отсюда множитель.
inline f32 strideFromLegs(const Rig& rig) {
    f32 hip = 0.f;
    Pose rest;
    ResolvedPart p[MAX_PARTS];
    const u8 n = resolve(rig, rest, glm::vec3(0.f), 0.f, p, MAX_PARTS);
    u8 w = 0;
    for (u8 i = 0; i < rig.count && w < n; ++i) {
        if (!rig.parts[i].visible) continue;
        switch (rig.parts[i].role) {
            case PartRole::UpperLegFL: case PartRole::UpperLegFR:
            case PartRole::UpperLegBL: case PartRole::UpperLegBR:
                hip = std::max(hip, p[w].center.y + p[w].size.y * 0.5f);
                break;
            default: break;
        }
        ++w;
    }
    // Оснастка без ног (рыба, призрак) — шага у неё нет, но делить на
    // ноль нельзя: берём заметную величину.
    if (hip < 0.05f) return 1.6f;
    return hip * 1.6f;
}

} // namespace entity
