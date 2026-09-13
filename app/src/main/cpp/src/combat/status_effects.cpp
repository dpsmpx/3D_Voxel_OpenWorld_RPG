/**
 * @file status_effects.cpp
 * @brief Бой: урон, оружие, зачарования, система «Резонанс», статусы.
 */
#include "status_effects.h"
#include "projectile.h"
#include "../ecs/components.h"
#include "../mobs/mob_def.h"
#include "../mobs/mob_ai.h"
#include "../progression/progression.h"
#include "../quests/quest.h"
#include <algorithm>

namespace combat {

void applyStatuses(StatusEffects& se, const DamageInstance& dmg) {
    if (dmg.burnTime > 0.f) {
        se.burnTime = std::min(10.f, se.burnTime + dmg.burnTime);
        se.burnDps = std::max(se.burnDps, dmg.amount * 0.10f);
    }

    if (dmg.slowDuration > 0.f) {
        se.slowTime = std::max(se.slowTime, dmg.slowDuration);
        se.slowAmount = std::max(se.slowAmount, dmg.slowAmount);
    }

    if (dmg.stunDuration > 0.f) {
        se.stunTime = std::max(se.stunTime, dmg.stunDuration);
    }

    if (dmg.poisonTime > 0.f) {
        se.poisonTime = std::max(se.poisonTime, dmg.poisonTime);
        se.poisonDps  = std::max(se.poisonDps, dmg.poisonDps);
    }

    if (dmg.sourceEntity != 0) {
        se.lastAttacker = dmg.sourceEntity;
    }

    se.flashTimer = std::max(se.flashTimer, 0.15f);
}

// Внутренний хелпер: награда XP при смерти.
static void onTargetDeath(ecs::Registry& reg,
                          ecs::Entity target,
                          u32 killerEntity)
{
    if (killerEntity == 0) return;

    auto* prog = reg.get<progression::Progression>(killerEntity);
    if (!prog) return;

    u64 xpReward = 0;
    if (auto* tag = reg.get<mobs::MobTag>(target)) {
        const auto& def = mobs::mobRegistry().get(tag->id);
        xpReward = def.xpReward;

        // Цели вида «убить N таких-то» отмечаются здесь же, где
        // начисляется опыт: это единственное место, которое знает и
        // убийцу, и вид убитого, и срабатывает ровно один раз.
        // Функция quests::notifyMobKilled существовала, но её никто
        // не вызывал — а на такие цели приходится большинство
        // выдаваемых квестов, и счётчик у них навсегда оставался в
        // нуле. Проверка на Progression выше заодно отсекает мобов,
        // убивающих друг друга: квесты считают только игрока.
        quests::notifyMobKilled(reg, killerEntity, tag->id);
    }

    if (xpReward > 0) {
        progression::rewardKillXP(reg, reg.fromId(killerEntity), xpReward);
    }
}

f32 applyDamage(ecs::Registry& reg, ecs::Entity target, const DamageInstance& dmg) {
    auto* h = reg.get<ecs::Health>(target);
    if (!h) return 0.f;

    if (h->invulnTime > 0.f) {
        if (dmg.burnTime <= 0.f && dmg.poisonTime <= 0.f) {
            return 0.f;
        }
    }

    // Уклонение
    bool dodged = false;
    if (dmg.sourceEntity != 0) {
        const auto& derived = progression::derivedOf(reg, target);
        if (derived.dodgeChance > 0.f && rollCritical(derived.dodgeChance)) {
            dodged = true;
        }
    }

    if (dodged) {
        if (auto* tf = reg.get<ecs::Transform>(target)) {
            glm::vec3 pos = tf->position + glm::vec3(0.f, 0.9f, 0.f);
            spawnHitFx(reg, pos, 0xA0A0A0FF, 0.10f, 0.40f, 0.12f);
        }
        return 0.f;
    }

    // Сопротивления
    const DamageResistance* res = nullptr;
    auto* combatant = reg.get<Combatant>(target);
    DamageResistance defaultRes{};
    if (combatant) res = &combatant->resistance;
    else           res = &defaultRes;

    // Phase 9: производные характеристики цели дают доп. сопротивление
    const auto& targetDerived = progression::derivedOf(reg, target);
    DamageResistance effective = *res;
    if (targetDerived.damageResistPhysical > 0.f) {
        effective.physical = std::min(0.80f,
            effective.physical + targetDerived.damageResistPhysical);
    }
    if (targetDerived.damageResistMagic > 0.f) {
        effective.fire   = std::min(0.80f, effective.fire   + targetDerived.damageResistMagic);
        effective.frost  = std::min(0.80f, effective.frost  + targetDerived.damageResistMagic);
        effective.shock  = std::min(0.80f, effective.shock  + targetDerived.damageResistMagic);
        effective.arcane = std::min(0.80f, effective.arcane + targetDerived.damageResistMagic);
    }

    const f32 final = computeFinalDamage(dmg, effective);

    h->current -= final;
    if (final > 0.f) {
        h->invulnTime = std::max(h->invulnTime, 0.15f);
    }

    auto* se = reg.get<StatusEffects>(target);
    if (se) applyStatuses(*se, dmg);

    if (h->current <= 0.f) {
        auto* agent = reg.get<ecs::AIAgent>(target);
        if (agent) agent->state = ecs::AIAgent::Dead;
        onTargetDeath(reg, target, dmg.sourceEntity);
    }

    if (final > 0.f) {
        if (auto* tf = reg.get<ecs::Transform>(target)) {
            u32 color = 0xFFFF80FF;
            switch (dmg.type) {
                case DamageType::Fire:   color = 0xFF8040FF; break;
                case DamageType::Frost:  color = 0x80D0FFFF; break;
                case DamageType::Shock:  color = 0xFFFF60FF; break;
                case DamageType::Poison: color = 0x80FF80FF; break;
                case DamageType::Arcane: color = 0xC080FFFF; break;
                default:                 color = 0xFFFFFFC0; break;
            }
            glm::vec3 pos = tf->position + glm::vec3(0.f, 0.8f, 0.f);
            spawnHitFx(reg, pos, color, 0.20f, 0.65f, 0.16f);
        }
    }

    return final;
}

void tickStatuses(ecs::Registry& reg, f32 dt) {
    auto& pool = reg.pool<StatusEffects>();
    const usize n = pool.size();

    for (usize i = 0; i < n; ++i) {
        ecs::Entity e = pool.entityAt((u32)i);
        auto* se = pool.get(e);
        auto* h  = reg.get<ecs::Health>(e);
        if (!se || !h) continue;

        // --- Burn DoT ---
        if (se->burnTime > 0.f) {
            const f32 amount = se->burnDps * dt;
            se->burnTime -= dt;
            if (se->burnTime < 0.f) se->burnTime = 0.f;
            h->current -= amount;
            if (h->current <= 0.f) {
                auto* agent = reg.get<ecs::AIAgent>(e);
                if (agent) agent->state = ecs::AIAgent::Dead;
                onTargetDeath(reg, e, se->lastAttacker);
            }
        } else {
            se->burnDps = 0.f;
        }

        // --- Poison DoT ---
        if (se->poisonTime > 0.f) {
            const f32 amount = se->poisonDps * dt;
            se->poisonTime -= dt;
            if (se->poisonTime < 0.f) se->poisonTime = 0.f;
            h->current -= amount;
            if (h->current <= 0.f) {
                auto* agent = reg.get<ecs::AIAgent>(e);
                if (agent) agent->state = ecs::AIAgent::Dead;
                onTargetDeath(reg, e, se->lastAttacker);
            }
        } else {
            se->poisonDps = 0.f;
        }

        // --- Slow ---
        if (se->slowTime > 0.f) {
            se->slowTime -= dt;
            if (se->slowTime <= 0.f) {
                se->slowTime = 0.f;
                se->slowAmount = 0.f;
            } else if (se->slowTime < 1.f) {
                se->slowAmount *= se->slowTime;
            }
        }

        // --- Stun ---
        if (se->stunTime > 0.f) {
            se->stunTime -= dt;
            if (se->stunTime < 0.f) se->stunTime = 0.f;
        }

        // --- Flash ---
        if (se->flashTimer > 0.f) {
            se->flashTimer -= dt;
            if (se->flashTimer < 0.f) se->flashTimer = 0.f;
        }
    }
}

} // namespace combat
