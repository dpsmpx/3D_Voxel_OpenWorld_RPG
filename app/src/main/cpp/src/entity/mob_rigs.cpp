/**
 * @file mob_rigs.cpp
 * @brief Оснастка мобов: описание модели по видам.
 */
#include "mob_rigs.h"
#include "beast_rig.h"
#include "humanoid_rig.h"
#include "../mobs/mob_def.h"
#include <array>

namespace mobs {

namespace {

using entity::BeastSpec;
using entity::HumanoidSpec;
using entity::Part;
using entity::PartRole;
using entity::Rig;

constexpr u32 rgba(u8 r, u8 g, u8 b) {
    return ((u32)r << 24) | ((u32)g << 16) | ((u32)b << 8) | 0xFFu;
}

// ============================================================
// Описания моделей
// ============================================================
//
// Раньше вид описывался плоским списком из девяти коробок со
// смещениями от начала сущности. Слоты были зашиты — тело, голова,
// четыре ноги, хвост, две руки, — и ничего сверх этого выразить было
// нельзя: ни уха, ни морды, ни рога. Звери отличались друг от друга
// цветом и размерами параллелепипеда, и только.
//
// Здесь вид описывается тем, что он ЕСТЬ: зверь, двуногий или комок.
// Приметы — уши, морда, рога, хвост — часть описания, а не
// недостающий слот.
//
// Рост во всех описаниях равен `bodyHeight` из MobDef, то есть
// высоте коллайдера. Это не совпадение и не аккуратность: сборщик
// ставит голову так, чтобы макушка пришлась ровно на заданный рост.
// Раньше они расходились — у Каменного стража модель была 4.35 при
// коллайдере 3.40, почти на метр выше, чем то, во что попадают.

Rig sheepRig(const MobDef& def) {
    BeastSpec s;
    s.height     = def.bodyHeight;
    s.bodyLength = 1.05f;
    s.bodyWidth  = 0.72f;
    s.bodyDepth  = 0.62f;
    s.legLength  = 0.42f;
    s.legThick   = 0.17f;
    s.headSize   = 0.42f;
    s.headWidth  = 0.36f;
    // Висячие уши и короткая морда — овца читается по ним даже
    // издали, когда шерсть неотличима от любой другой белой коробки.
    s.earSize    = 0.20f;
    s.earLean    = -1.15f;
    s.snoutLen   = 0.16f;
    s.tailLength = 0.16f;
    s.tailThick  = 0.13f;
    s.bodyColor   = rgba(226, 226, 220);
    s.headColor   = rgba(202, 202, 196);
    s.legColor    = rgba(120, 120, 115);
    s.accentColor = rgba(60, 58, 56);
    return entity::beastRig(s);
}

Rig cowRig(const MobDef& def) {
    BeastSpec s;
    s.height     = def.bodyHeight;
    s.bodyLength = 1.30f;
    s.bodyWidth  = 0.80f;
    s.bodyDepth  = 0.72f;
    s.legLength  = 0.52f;
    s.legThick   = 0.20f;
    s.headSize   = 0.46f;
    s.headWidth  = 0.42f;
    s.earSize    = 0.16f;
    s.earLean    = -0.5f;        // уши в стороны, не висят
    s.snoutLen   = 0.22f;
    s.hornLen    = 0.20f;        // ради рогов всё и затевалось
    s.tailLength = 0.55f;
    s.tailThick  = 0.09f;
    s.tailLift   = -0.35f;       // хвост свисает
    s.bodyColor   = rgba(84, 62, 42);
    s.headColor   = rgba(230, 230, 220);
    s.legColor    = rgba(60, 45, 30);
    s.accentColor = rgba(232, 226, 200);
    return entity::beastRig(s);
}

Rig chickenRig(const MobDef& def) {
    BeastSpec s;
    s.height     = def.bodyHeight;
    s.bodyLength = 0.46f;
    s.bodyWidth  = 0.32f;
    s.bodyDepth  = 0.34f;
    s.legLength  = 0.22f;
    s.legThick   = 0.055f;
    s.legPairs   = 1;            // птица
    s.headSize   = 0.22f;
    s.headWidth  = 0.20f;
    s.snoutLen   = 0.11f;        // клюв
    s.tailLength = 0.16f;
    s.tailThick  = 0.16f;
    s.tailLift   = 0.55f;        // хвост торчком
    s.bodyColor   = rgba(242, 242, 242);
    s.headColor   = rgba(248, 248, 248);
    s.legColor    = rgba(222, 142, 24);
    s.accentColor = rgba(230, 150, 30);
    return entity::beastRig(s);
}

Rig wolfRig(const MobDef& def) {
    BeastSpec s;
    s.height     = def.bodyHeight;
    s.bodyLength = 1.15f;
    s.bodyWidth  = 0.52f;
    s.bodyDepth  = 0.48f;
    s.legLength  = 0.44f;
    s.legThick   = 0.14f;
    s.headSize   = 0.36f;
    s.headWidth  = 0.32f;
    // Уши торчком и длинная морда — ровно то, чем волк отличается от
    // овцы, когда обе коробка на четырёх ногах.
    s.earSize    = 0.17f;
    s.earLean    = 0.25f;
    s.snoutLen   = 0.26f;
    s.tailLength = 0.42f;
    s.tailThick  = 0.15f;
    s.tailLift   = -0.25f;
    s.bodyColor   = rgba(82, 82, 92);
    s.headColor   = rgba(72, 72, 82);
    s.legColor    = rgba(52, 52, 62);
    s.accentColor = rgba(40, 40, 46);
    return entity::beastRig(s);
}

Rig humanoidMob(const MobDef& def, u32 body, u32 head, u32 limb,
                f32 shoulder, f32 legFrac)
{
    HumanoidSpec s;
    s.height       = def.bodyHeight;
    s.bodyColor    = body;
    s.headColor    = head;
    s.accentColor  = limb;
    s.shoulderFrac = shoulder;
    s.legFrac      = legFrac;
    s.torsoFrac    = 1.f - legFrac - s.headFrac;
    return entity::humanoidRig(s);
}

/// Комок: ни ног, ни головы. Единственный вид, которому оснастка не
/// нужна, — и это тоже описание, а не исключение в коде.
Rig blobRig(const MobDef& def) {
    Rig rig;

    Part root;
    root.parent  = -1;
    root.role    = PartRole::Root;
    root.visible = false;
    const u8 iRoot = rig.add(root);

    const f32 h = def.bodyHeight;

    Part body;
    body.parent    = (i8)iRoot;
    body.role      = PartRole::Torso;
    body.pivot     = glm::vec3(0.f);
    body.boxOffset = glm::vec3(0.f, h * 0.5f, 0.f);
    body.size      = glm::vec3(h, h, h);
    body.color     = rgba(80, 200, 80);
    const i8 iBody = (i8)rig.add(body);

    // Ядро внутри — комок просвечивает, и его видно.
    Part core;
    core.parent    = iBody;
    core.role      = PartRole::Prop;
    core.pivot     = glm::vec3(0.f, h * 0.5f, 0.f);
    core.boxOffset = glm::vec3(0.f);
    core.size      = glm::vec3(h * 0.5f);
    core.color     = rgba(120, 240, 120);
    rig.add(core);

    rig.groundOffset = -entity::lowestPoint(rig, rig.rest, 0.f);
    rig.strideLength = h * 1.2f;   // ног нет, но прыжки соразмерны росту
    return rig;
}

Rig buildRig(u16 id, const MobDef& def) {
    switch (id) {
        case MOB_SHEEP:   return sheepRig(def);
        case MOB_COW:     return cowRig(def);
        case MOB_CHICKEN: return chickenRig(def);
        case MOB_WOLF:    return wolfRig(def);
        case MOB_SLIME:   return blobRig(def);

        case MOB_SKELETON:
            return humanoidMob(def, rgba(226, 226, 214), rgba(238, 238, 228),
                               rgba(206, 206, 194), 0.28f, 0.48f);
        case MOB_GOBLIN:
            return humanoidMob(def, rgba(92, 132, 70), rgba(116, 156, 88),
                               rgba(74, 106, 56), 0.34f, 0.40f);
        case MOB_WITCH:
            // Тёмное платье и бледное лицо: фигура узнаётся силуэтом
            // раньше, чем цветом, поэтому она узкая и высокая.
            return humanoidMob(def, rgba(58, 44, 78), rgba(206, 196, 178),
                               rgba(44, 34, 60), 0.26f, 0.52f);
        case MOB_BOSS_WARDEN:
            // Приземистый и широкий: угроза читается пропорциями, а
            // не одним лишь размером.
            return humanoidMob(def, rgba(96, 102, 112), rgba(120, 126, 138),
                               rgba(84, 90, 100), 0.46f, 0.42f);
        case MOB_BOSS_HOLLOW:
            return humanoidMob(def, rgba(48, 44, 66), rgba(214, 208, 190),
                               rgba(40, 36, 56), 0.32f, 0.46f);

        default: return Rig{};
    }
}

} // namespace

const entity::Rig& rigFor(u16 mobId) {
    static std::array<entity::Rig, MOB_COUNT> cache{};
    static std::array<bool, MOB_COUNT> built{};

    const u16 id = (mobId < MOB_COUNT) ? mobId : (u16)MOB_NONE;
    if (!built[id]) {
        cache[id] = buildRig(id, mobRegistry().get(id));
        built[id] = true;
    }
    return cache[id];
}

} // namespace mobs
