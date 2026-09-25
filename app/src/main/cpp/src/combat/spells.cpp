/**
 * @file spells.cpp
 * @brief Бой: заклинания Древа — ледяные иглы и струя огня.
 */
#include "spells.h"
#include "combat_controller.h"
#include "hit_detection.h"
#include "projectile.h"
#include "status_effects.h"
#include "../audio/audio_events.h"
#include "../ecs/components.h"
#include "../items/inventory.h"
#include "../items/item_def.h"
#include "../physics/raycast.h"
#include "../progression/resource_regen.h"
#include "../progression/skill_tree.h"
#include "../world/fire.h"
#include "../world/particles.h"
#include <algorithm>
#include <cmath>
#include <vector>

namespace combat {

namespace {

/// Какой навык даёт какое заклинание и какой руной его держат.
struct TreeSpell {
    u16 weaponId;
    u16 runeItem;
    progression::SkillNodeId node;
};

constexpr TreeSpell TREE_SPELLS[] = {
    { WEAPON_ICE_NEEDLES, items::ITEM_RUNE_ICE_NEEDLES, progression::SkillNodeId::Wis_IceNeedles },
    { WEAPON_FLAME,       items::ITEM_RUNE_FLAME,       progression::SkillNodeId::Wis_Flame },
};

const TreeSpell* treeSpell(u16 weaponId) {
    for (const TreeSpell& s : TREE_SPELLS)
        if (s.weaponId == weaponId) return &s;
    return nullptr;
}

constexpr u32 NEEDLE_COLOR = 0xCFEFFFFFu;
constexpr u32 NEEDLE_SHARD = 0xBFE8FFFFu;
constexpr u32 FROST_MOTE   = 0xE8F8FFFFu;
constexpr u32 FROST_MOTE2  = 0x9FD8FFFFu;
constexpr u32 FLAME_CORE   = 0xFFE070FFu;
constexpr u32 FLAME_EDGE   = 0xFF6A1AFFu;

/// Сопротивление воздуха частиц (world::Particles::DRAG), 1/с.
constexpr f32 PARTICLE_DRAG = world::Particles::DRAG;

/// Направление вбок от взгляда. Глядя строго вверх или вниз, бока
/// у взгляда нет — берём любой.
glm::vec3 sideOf(const glm::vec3& aim) {
    glm::vec3 r = glm::cross(aim, glm::vec3(0.f, 1.f, 0.f));
    const f32 l = glm::length(r);
    return l > 1e-3f ? r / l : glm::vec3(1.f, 0.f, 0.f);
}

/// Детерминированный шум для разброса лучей и частиц струи: rand()
/// в бою не нужен, а свой генератор у струи — одна переменная.
f32 noise01(u32& s) {
    s ^= s << 13; s ^= s >> 17; s ^= s << 5;
    return (f32)(s & 0xFFFFFFu) / (f32)0x1000000;
}

} // namespace

bool isTreeSpell(u16 weaponId) {
    return treeSpell(weaponId) != nullptr;
}

u8 spellRank(ecs::Registry& reg, ecs::Entity e, u16 weaponId) {
    const TreeSpell* s = treeSpell(weaponId);
    if (!s) return 0;
    const auto* tree = reg.get<progression::SkillTree>(e);
    return tree ? tree->rank(s->node) : 0;
}

u16 syncSpellRunes(ecs::Registry& reg, ecs::Entity e) {
    auto* tree = reg.get<progression::SkillTree>(e);
    auto* inv  = reg.get<items::Inventory>(e);
    if (!tree || !inv) return 0;
    auto* eq = reg.get<EquippedWeapon>(e);

    u16 granted = 0;
    for (const TreeSpell& s : TREE_SPELLS) {
        const bool known = tree->rank(s.node) > 0;
        const bool inHand = eq && eq->weaponId == s.weaponId;
        const u32 carried = inv->countOf(s.runeItem);

        if (!known) {
            // Навык сброшен — руна уходит отовсюду. Из рук тоже:
            // держать невыученное заклинание нельзя.
            if (carried > 0) inv->removeItem(s.runeItem, (u16)carried);
            if (inHand) {
                eq->weaponId = WEAPON_NONE;
                eq->enchant = Enchantment{};
            }
            continue;
        }

        // Руна одна: вторая (скажем, вернувшаяся из рук, пока первая
        // уже лежала в сумке) лишняя.
        const u32 have = carried + (inHand ? 1u : 0u);
        if (have > 1) inv->removeItem(s.runeItem, (u16)(have - 1));
        if (have >= 1) continue;

        // Сначала в пояс: руну надевают оттуда, и искать её по сумке
        // ради первого же заклинания игрок не должен.
        items::ItemStack rune;
        rune.itemId = s.runeItem;
        rune.count = 1;
        bool placed = false;
        for (u32 i = 0; i < items::INV_HOTBAR_SLOTS && !placed; ++i) {
            if (!inv->hotbarSlot(i).empty()) continue;
            inv->hotbarSlot(i) = rune;
            placed = true;
        }
        if (!placed) placed = inv->addItem(s.runeItem, 1).leftover == 0;
        // Места нет — попробуем в следующий раз.
        if (placed && granted == 0) granted = s.runeItem;
    }
    return granted;
}

u32 iceNeedleCount(u8 rank) {
    return rank == 0 ? 0u : 2u + (u32)std::min<u8>(rank, 3);
}

u32 castIceNeedles(ecs::Registry& reg, ecs::Entity caster, u16 weaponId,
                   const Enchantment& enchant, u8 rank,
                   const glm::vec3& origin, const glm::vec3& aim,
                   f32 damageMult, f32 critChance, f32 critMult)
{
    const u32 count = iceNeedleCount(rank);
    if (count == 0) return 0;
    const WeaponDef& def = weapons().get(weaponId);

    const glm::vec3 side = sideOf(aim);
    const glm::vec3 up = glm::normalize(glm::cross(side, aim));
    const glm::vec3 hand = origin + aim * 0.6f;

    // Иглы рождаются в ладони: иней собирается из воздуха в тот
    // момент, когда рука уже бросает. Без этого залп выглядел бы
    // выстрелом из ниоткуда.
    {
        world::Burst b;
        b.origin  = hand;
        b.dir     = aim;
        b.count   = 8;
        b.speed   = 1.5f;
        b.spread  = 0.9f;
        b.size    = 0.035f;
        b.life    = 0.25f;
        b.gravity = -1.f;
        b.color   = FROST_MOTE;
        b.color2  = FROST_MOTE2;
        world::particles().emit(b);
    }

    // Веер: шаг между иглами ~3.4°, средняя летит точно в прицел.
    constexpr f32 FAN_STEP = 0.06f;
    const f32 mid = 0.5f * (f32)(count - 1);
    const f32 rankMult = 1.f + 0.10f * (f32)(rank - 1);
    const u32 faction = Faction::of(reg, caster);

    u32 spawned = 0;
    for (u32 i = 0; i < count; ++i) {
        const f32 k = (f32)i - mid;
        // Чуть вверх-вниз через одну: ровная линия игл читается как
        // гребёнка, а не как брошенная горсть.
        const f32 lift = (i % 2u == 0u ? 1.f : -1.f) * 0.012f;
        const glm::vec3 dir = glm::normalize(aim + side * std::tan(k * FAN_STEP) + up * lift);

        ProjectileSpawnParams p{};
        p.weaponId     = weaponId;
        p.origin       = hand + side * (k * 0.08f);
        p.direction    = dir;
        p.ownerEntity  = caster;
        p.ownerFaction = faction;

        p.damage.amount       = def.baseDamage * damageMult * rankMult;
        p.damage.type         = def.damageType;
        p.damage.sourceName   = def.name;
        p.damage.sourceEntity = (u32)caster;
        p.damage.isCritical   = rollCritical(critChance);
        p.damage.criticalMult = critMult;
        // Холод: игла замедляет, и каждая следующая продлевает.
        p.damage.slowAmount   = 0.25f + 0.05f * (f32)rank;
        p.damage.slowDuration = 1.6f;

        const EnchantResult er = applyEnchantment(p.damage, enchant);
        p.damage    = er.primary;
        p.secondary = er.secondary;
        p.lifesteal = er.lifestealFraction;

        p.speed      = def.projectileSpeed;
        p.gravity    = 0.f;
        p.lifeTime   = def.reach / std::max(1.f, def.projectileSpeed);
        p.colorRGBA  = NEEDLE_COLOR;
        p.scale      = 0.045f;
        p.isSpell    = true;
        p.needle     = true;
        p.shardColor = NEEDLE_SHARD;

        if (spawnProjectile(reg, p).valid()) ++spawned;
    }
    return spawned;
}

f32 flameReach(u8 rank) {
    const WeaponDef& def = weapons().get(WEAPON_FLAME);
    return def.reach + 0.8f * (f32)(std::max<u8>(rank, 1) - 1);
}

void updateFlameStream(world::ChunkManager& world, ecs::Registry& reg,
                       const SpatialHash* hash, ecs::Entity caster,
                       const WeaponDef& def, u8 rank,
                       const glm::vec3& origin, const glm::vec3& aim,
                       bool held, bool stunned,
                       f32 damageMult, f32 reachMult, f32 dt,
                       WeaponState& st, CombatAction& out)
{
    auto stop = [&]() {
        st.streaming = false;
        // Первый тик новой струи — сразу: пламя, которое жжёт через
        // десятую секунды после нажатия, ощущается запозданием.
        st.streamTick = FLAME_TICK;
        st.streamFx = 0.f;
        const f32 step = dt * 4.f;
        st.swingAnim = st.swingAnim > step ? st.swingAnim - step : 0.f;
    };

    if (!held || stunned || rank == 0) { stop(); return; }

    const f32 cost = def.manaCost * progression::manaCostMult(reg, caster) * dt;
    auto* mana = reg.get<ecs::Mana>(caster);
    if (!mana || mana->current < cost) { stop(); return; }
    progression::consumeMana(reg, caster, cost);

    const bool started = !st.streaming;
    st.streaming = true;
    st.swingAnim = 1.f;   // ладонь вытянута вперёд, пока льётся огонь
    st.swingDir = aim;

    // Гул пламени: при зажигании и дальше мерно, пока горит.
    st.streamSound -= dt;
    if (started || st.streamSound <= 0.f) {
        audio::events().spellCast(origin, (u8)DamageType::Fire);
        st.streamSound = 0.45f;
    }

    const f32 reach = flameReach(rank) * reachMult;
    const glm::vec3 hand = origin + aim * 0.45f;

    // Докуда струя долетает: в стену она упирается и растекается по
    // ней, а не проходит насквозь.
    const physics::RayHit wall = physics::raycastVoxels(world, origin, aim, reach);
    const f32 freeDist = wall.hit ? std::max(0.4f, wall.distance) : reach;

    u32 seed = ((u32)caster * 2654435761u ^ (u32)(st.streamTick * 104729.f) ^
                (u32)(origin.x * 131.f) ^ (u32)(origin.z * 173.f)) | 1u;

    // ---- Пламя из ладони ----
    //
    // Частица с сопротивлением воздуха D пролетает за время t путь
    // v/D·(1 − e^(−Dt)); скорость подбирается так, чтобы струя
    // долетала ровно до стены или до конца своей длины.
    {
        constexpr f32 LIFE = 0.45f;
        const f32 reachFrac = 1.f - std::exp(-PARTICLE_DRAG * LIFE);
        const f32 speed = std::clamp(freeDist * PARTICLE_DRAG / reachFrac, 3.f, 26.f);
        st.streamFx += dt * 70.f;
        while (st.streamFx >= 1.f) {
            st.streamFx -= 1.f;
            world::Burst b;
            b.origin  = hand;
            b.dir     = aim;
            b.count   = 1;
            b.speed   = speed;
            b.spread  = 0.10f;
            b.size    = 0.09f + noise01(seed) * 0.07f;
            b.life    = LIFE;
            b.gravity = -3.f;
            b.color   = FLAME_CORE;
            b.color2  = FLAME_EDGE;
            world::particles().emit(b);
        }
    }
    world::fires().addGlow(hand + aim * std::min(2.5f, freeDist * 0.6f), 0.9f, 9.f);

    // ---- Жар: тики урона и поджог ----
    st.streamTick += dt;
    const u32 faction = Faction::of(reg, caster);
    const f32 dps = def.baseDamage * damageMult * (1.f + 0.25f * (f32)(rank - 1));
    const f32 burnDps = 4.f + 2.f * (f32)(rank - 1);
    std::vector<HitTarget> hits;
    while (st.streamTick >= FLAME_TICK) {
        st.streamTick -= FLAME_TICK;

        hits.clear();
        meleeConeHits(world, reg, hash, origin, aim,
                      std::min(reach, freeDist + 0.6f), def.coneAngle,
                      faction, caster, hits);
        for (const HitTarget& h : hits) {
            DamageInstance d{};
            d.amount       = dps * FLAME_TICK;
            d.type         = DamageType::Fire;
            d.burnTime     = 2.5f;
            d.burnDps      = burnDps;
            d.sourceEntity = (u32)caster;
            d.targetEntity = (u32)h.entity;
            d.sourceName   = def.name;
            applyDamage(reg, h.entity, d);
        }
        out.hitCount += (i32)hits.size();
        if (!hits.empty()) out.hitPoint = hits.front().center;

        // Два луча в раствор конуса: куда упрутся — там и загорится.
        const glm::vec3 side = sideOf(aim);
        const glm::vec3 up = glm::normalize(glm::cross(side, aim));
        const f32 spread = std::tan(def.coneAngle * 0.5f);
        for (int r = 0; r < 2; ++r) {
            const f32 a = (noise01(seed) * 2.f - 1.f) * spread;
            const f32 b = (noise01(seed) * 2.f - 1.f) * spread;
            const glm::vec3 dir = glm::normalize(aim + side * a + up * b);
            const physics::RayHit hit = physics::raycastVoxels(world, origin, dir, reach);
            if (!hit.hit) continue;
            world::fires().ignite(world, hit.block, hit.block + hit.normal,
                                  (u32)caster, faction);
        }
    }
}

} // namespace combat
