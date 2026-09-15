/**
 * @file mob_rigs.cpp
 * @brief Оснастка мобов: иерархия частей по видам.
 */
#include "mob_rigs.h"
#include "../mobs/mob_def.h"
#include <array>

namespace mobs {

namespace {

using entity::Part;
using entity::PartRole;
using entity::Rig;

/// Роль части по её слоту в старом плоском определении.
PartRole roleOfSlot(u8 slot) {
    switch (slot) {
        case Part_Body:  return PartRole::Torso;
        case Part_Head:  return PartRole::Head;
        case Part_LegFR: return PartRole::UpperLegFR;
        case Part_LegFL: return PartRole::UpperLegFL;
        case Part_LegBR: return PartRole::UpperLegBR;
        case Part_LegBL: return PartRole::UpperLegBL;
        case Part_Tail:  return PartRole::Tail;
        case Part_ArmR:  return PartRole::UpperArmR;
        case Part_ArmL:  return PartRole::UpperArmL;
        default:         return PartRole::Prop;
    }
}

bool isLegSlot(u8 slot) {
    return slot == Part_LegFR || slot == Part_LegFL ||
           slot == Part_LegBR || slot == Part_LegBL;
}

PartRole lowerOf(PartRole upper) {
    switch (upper) {
        case PartRole::UpperLegFR: return PartRole::LowerLegFR;
        case PartRole::UpperLegFL: return PartRole::LowerLegFL;
        case PartRole::UpperLegBR: return PartRole::LowerLegBR;
        case PartRole::UpperLegBL: return PartRole::LowerLegBL;
        default:                   return PartRole::Prop;
    }
}

/// Построить оснастку из плоского определения вида.
///
/// Правила сборки:
///   * корень — невидимый сустав в точке опоры; вокруг него
///     поворачивается сущность целиком;
///   * торс — ребёнок корня;
///   * голова, хвост, руки и БЁДРА — дети торса;
///   * каждая нога делится надвое: бедро вращается у торса, голень —
///     у колена. Раньше нога была одной коробкой и гнуться не могла.
Rig buildRig(const MobDef& def) {
    Rig rig;

    // Корень: сустав в точке опоры, без коробки.
    Part root;
    root.parent  = -1;
    root.role    = PartRole::Root;
    root.visible = false;
    const u8 iRoot = rig.add(root);

    // Торс. Ищем его среди частей; если вида без тела не бывает,
    // но проверка дешевле падения.
    i8 iTorso = (i8)iRoot;
    for (u8 p = 0; p < def.partCount; ++p) {
        if (roleOfSlot(p) != PartRole::Torso) continue;
        const MobPart& src = def.parts[p];
        Part t;
        t.parent    = (i8)iRoot;
        t.role      = PartRole::Torso;
        t.pivot     = src.offset;      // сустав там, где был центр
        t.boxOffset = glm::vec3(0.f);
        t.size      = src.size;
        t.color     = src.color;
        iTorso = (i8)rig.add(t);
        break;
    }

    for (u8 p = 0; p < def.partCount; ++p) {
        const MobPart& src = def.parts[p];
        const PartRole role = roleOfSlot(p);
        if (role == PartRole::Torso) continue;
        if (src.size.x <= 0.f || src.size.y <= 0.f || src.size.z <= 0.f) continue;

        // Смещение части задавалось от начала сущности, а торс сам
        // смещён — переводим в систему торса.
        const glm::vec3 fromTorso = src.offset - rig.parts[(u8)iTorso].pivot;

        if (!isLegSlot(p)) {
            Part q;
            q.parent    = iTorso;
            q.role      = role;
            q.pivot     = fromTorso;
            q.boxOffset = glm::vec3(0.f);
            q.size      = src.size;
            q.color     = src.color;
            rig.add(q);
            continue;
        }

        // ---- Нога: бедро + голень ----
        //
        // Половинки обязаны в сумме давать исходную длину ноги. Было
        // по 0.55 на каждую — нога выходила на десятую часть длиннее
        // задуманной, и существо ровно на столько же проваливалось
        // под землю.
        const f32 full = src.size.y;
        const f32 upH  = full * 0.5f;
        const f32 loH  = full * 0.5f;

        // Бедро вращается у ВЕРХА ноги, а не у её центра: иначе
        // качание уводит ступню из-под тела.
        Part up;
        up.parent    = iTorso;
        up.role      = role;
        up.pivot     = fromTorso + glm::vec3(0.f, full * 0.5f, 0.f);
        up.boxOffset = glm::vec3(0.f, -upH * 0.5f, 0.f);
        up.size      = glm::vec3(src.size.x, upH, src.size.z);
        up.color     = src.color;
        const u8 iUp = rig.add(up);

        Part lo;
        lo.parent    = (i8)iUp;
        lo.role      = lowerOf(role);
        lo.pivot     = glm::vec3(0.f, -upH, 0.f);       // колено
        lo.boxOffset = glm::vec3(0.f, -loH * 0.5f, 0.f);
        lo.size      = glm::vec3(src.size.x * 0.92f, loH, src.size.z * 0.92f);
        lo.color     = src.color;
        rig.add(lo);
    }

    // ---- Опора ----
    //
    // Начало сущности — точка опоры: контроллер ставит position.y
    // ровно на пол. Значит низ модели в покое обязан быть у нуля.
    //
    // В плоских определениях это выдержано не везде: у Каменного
    // стража ноги заданы как {0, 0.4} размером 1.6 — низ на -0.4, то
    // есть страж по щиколотку в полу. Правило здесь ОДНО и общее для
    // всех видов: оснастка приподнимается так, чтобы её нижняя точка
    // села на опору. Никаких «этому виду плюс 0.4»; величина
    // выводится из самой геометрии и хранится в метаданных модели.
    //
    // Летающие и плавающие переопределяют groundOffset после сборки —
    // это и есть та самая явная поправка, ради которой поле заведено.
    {
        entity::Pose rest;                          // нулевая поза
        rig.groundOffset = -entity::lowestPoint(rig, rest, 0.f);
    }

    return rig;
}

} // namespace

const entity::Rig& rigFor(u16 mobId) {
    static std::array<entity::Rig, MOB_COUNT> cache{};
    static std::array<bool, MOB_COUNT> built{};

    const u16 id = (mobId < MOB_COUNT) ? mobId : (u16)MOB_NONE;
    if (!built[id]) {
        cache[id] = buildRig(mobRegistry().get(id));
        built[id] = true;
    }
    return cache[id];
}

} // namespace mobs
