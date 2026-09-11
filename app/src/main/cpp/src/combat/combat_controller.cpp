#include "combat_controller.h"
#include "status_effects.h"
#include "projectile.h"
#include "../ecs/components.h"
#include "../progression/progression.h"
#include "../progression/resource_regen.h"
#include "../core/log.h"
#include <cmath>
#include <algorithm>

namespace combat {

namespace {

void enterPhase(WeaponState& st, WeaponState::Phase p) {
    st.phase = p;
    st.phaseTime = 0.f;
}

bool tryConsumeAttackCost(ecs::Registry& reg,
                          ecs::Entity e,
                          const WeaponDef& def)
{
    const f32 manaMult = progression::manaCostMult(reg, e);
    const f32 stamMult = progression::staminaCostMult(reg, e);

    const f32 manaCost = def.manaCost * manaMult;
    const f32 stamCost = def.staminaCost * stamMult;

    if (manaCost > 0.f) {
        auto* m = reg.get<ecs::Mana>(e);
        if (!m || m->current < manaCost) return false;
    }
    if (stamCost > 0.f) {
        auto* s = reg.get<ecs::Stamina>(e);
        if (!s || s->current < stamCost) return false;
    }

    if (manaCost > 0.f) progression::consumeMana(reg, e, manaCost);
    if (stamCost > 0.f) progression::consumeStamina(reg, e, stamCost);

    return true;
}

f32 applyHits(ecs::Registry& reg,
              ecs::Entity attacker,
              u32 attackerFaction,
              const std::vector<HitTarget>& hits,
              const WeaponDef& def,
              const DamageInstance& baseDamage,
              const EnchantResult& enchResult,
              f32 damageMult,
              f32 knockbackMult,
              CombatAction& out)
{
    f32 total = 0.f;
    i32 count = 0;

    for (const HitTarget& h : hits) {
        DamageInstance d = baseDamage;
        d.amount *= damageMult;
        d.isCritical = rollCritical(def.critChance);
        d.criticalMult = def.critMult;
        d.sourceEntity = (u32)attacker;
        d.targetEntity = (u32)h.entity;

        EnchantResult enchThis = enchResult;
        enchThis.primary = d;

        const f32 dmgMain = applyDamage(reg, h.entity, enchThis.primary);
        total += dmgMain;

        if (enchThis.secondary.amount > 0.f ||
            enchThis.secondary.burnTime > 0.f ||
            enchThis.secondary.poisonTime > 0.f ||
            enchThis.secondary.slowDuration > 0.f ||
            enchThis.secondary.stunDuration > 0.f)
        {
            const f32 dmgSec = applyDamage(reg, h.entity, enchThis.secondary);
            total += dmgSec;
        }

        if (enchResult.lifestealFraction > 0.f && dmgMain > 0.f) {
            auto* ah = reg.get<ecs::Health>(attacker);
            if (ah) {
                ah->current = std::min(ah->max,
                    ah->current + dmgMain * enchResult.lifestealFraction);
            }
        }

        auto* v = reg.get<ecs::Velocity>(h.entity);
        if (v && def.knockback > 0.f) {
            auto* cb = reg.get<Combatant>(h.entity);
            f32 resist = cb ? cb->knockbackResist : 0.f;
            f32 impulse = def.knockback * knockbackMult * (1.f - std::min(0.9f, resist));

            auto* atf = reg.get<ecs::Transform>(attacker);
            if (atf) {
                glm::vec3 kd = h.center - atf->position;
                kd.y = 0.f;
                f32 len = glm::length(kd);
                if (len > 0.01f) kd /= len;
                v->linear += kd * impulse;
                v->linear.y += 0.4f * impulse;
            }
        }

        if (count == 0) out.hitPoint = h.center;
        ++count;
    }

    out.hitCount = count;
    out.totalDamage = total;
    return total;
}

} // namespace

void updateCombat(world::ChunkManager& world,
                  ecs::Registry& reg,
                  const SpatialHash* hash,
                  ecs::Entity entity,
                  const glm::vec3& origin,
                  const glm::vec3& aimDir,
                  const CombatInput& in,
                  f32 dt,
                  CombatAction& out)
{
    out = CombatAction{};

    auto* weapon = reg.get<EquippedWeapon>(entity);
    auto* wstate = reg.get<WeaponState>(entity);
    auto* res    = reg.get<ResonanceState>(entity);

    if (!weapon || !wstate) return;
    if (weapon->weaponId == 0 || weapon->weaponId >= WEAPON_MAX_DEFS) return;

    const WeaponDef& def = weapons().get(weapon->weaponId);

    const auto& derived = progression::derivedOf(reg, entity);

    if (res) res->update(dt);

    DamageInstance base{};
    base.amount = def.baseDamage;
    base.type   = def.damageType;
    base.sourceName = def.name;

    EnchantResult enchResult = applyEnchantment(base, weapon->enchant);
    if (derived.enchantPowerMult != 1.f) {
        enchResult.secondary.amount *= derived.enchantPowerMult;
        enchResult.secondary.poisonDps *= derived.enchantPowerMult;
        enchResult.lifestealFraction *= derived.enchantPowerMult;
    }

    const f32 resonanceSpeed = res ? res->bonuses().speedMult : 1.f;
    const f32 speedMult = derived.attackSpeedMult *
                          resonanceSpeed *
                          enchResult.attackSpeedMult;

    const f32 windupDur   = def.windupTime   / std::max(0.2f, speedMult);
    const f32 recoveryDur = def.recoveryTime / std::max(0.2f, speedMult);

    f32 damageMult = 1.f;
    if (def.style == AttackStyle::Melee || def.style == AttackStyle::Ranged) {
        damageMult *= derived.meleeDamageMult;
    } else if (def.style == AttackStyle::Magic || def.style == AttackStyle::AoE) {
        damageMult *= derived.magicDamageMult;
    }
    if (res) damageMult *= res->bonuses().damageMult;

    const f32 critChance = std::min(1.f,
        def.critChance + derived.critChanceBonus + (res ? res->bonuses().critBonus : 0.f));
    const f32 critMult = def.critMult + derived.critDamageBonus;

    const f32 reachMult = (res ? res->bonuses().rangeMult : 1.f) * derived.rangeMult;
    const f32 knockbackMult = derived.knockbackMult;

    bool stunned = false;
    if (auto* se = reg.get<StatusEffects>(entity)) {
        stunned = se->stunned();
    }

    switch (wstate->phase) {

    case WeaponState::Idle: {
        wstate->swingAnim = std::max(0.f, wstate->swingAnim - dt * 4.f);
        wstate->inputConsumed = false;

        if (stunned) break;

        const bool wants = in.attackPressed || in.attackHeld;
        if (wants) {
            if (!tryConsumeAttackCost(reg, entity, def)) break;
            enterPhase(*wstate, WeaponState::Windup);
            wstate->swingDir = aimDir;
            wstate->inputConsumed = true;
            ++wstate->comboCounter;
            wstate->comboTimer = 1.2f;
        }
        break;
    }

    case WeaponState::Windup: {
        wstate->phaseTime += dt;
        wstate->swingAnim = std::min(1.f, wstate->phaseTime / std::max(0.01f, windupDur));

        if (wstate->phaseTime >= windupDur) {
            if (def.style == AttackStyle::Melee) {
                std::vector<HitTarget> hits;
                meleeConeHits(world, reg, hash, origin, aimDir,
                              def.reach * reachMult,
                              def.coneAngle,
                              Faction::of(reg, entity),
                              entity,
                              hits);

                if (!hits.empty()) {
                    applyHits(reg, entity, Faction::of(reg, entity),
                              hits, def, base, enchResult,
                              damageMult, knockbackMult, out);
                    out.didMeleeHit = true;
                    if (res) res->onHit(false, derived.resonanceGainMult);
                } else {
                    if (res) res->onMiss();
                }
            }
            else if (def.style == AttackStyle::Ranged ||
                     def.style == AttackStyle::Magic)
            {
                ProjectileSpawnParams params{};
                params.weaponId      = weapon->weaponId;
                params.origin        = origin + aimDir * 0.6f;
                params.direction     = aimDir;
                params.ownerEntity   = entity;
                params.ownerFaction  = Faction::of(reg, entity);

                params.damage        = base;
                params.damage.amount *= damageMult;
                params.damage.sourceEntity = (u32)entity;
                params.damage.isCritical   = rollCritical(critChance);
                params.damage.criticalMult = critMult;

                EnchantResult er = applyEnchantment(params.damage, weapon->enchant);
                params.damage    = er.primary;
                params.secondary = er.secondary;
                params.lifesteal = er.lifestealFraction;

                params.speed       = def.projectileSpeed;
                params.gravity     = def.projectileGravity;
                params.lifeTime    = 4.0f;
                params.colorRGBA   = 0xFFFFFFFF;
                params.scale       = 0.15f;

                if (def.style == AttackStyle::Magic) {
                    if (def.damageType == DamageType::Fire)   params.colorRGBA = 0xFF8030FF;
                    if (def.damageType == DamageType::Frost)  params.colorRGBA = 0x80D0FFFF;
                    if (def.damageType == DamageType::Arcane) params.colorRGBA = 0xC080FFFF;
                    params.scale = 0.22f;
                    params.isSpell = true;
                } else {
                    params.colorRGBA = 0xC0A080FF;
                }

                ecs::Entity projE = spawnProjectile(reg, params);
                if (projE.valid()) {
                    out.didShoot = (def.style == AttackStyle::Ranged);
                    out.didCastSpell = (def.style == AttackStyle::Magic);
                }
            }
            else if (def.style == AttackStyle::AoE) {
                std::vector<HitTarget> hits;
                sphereHits(world, reg, hash, origin,
                           def.reach * reachMult,
                           Faction::of(reg, entity),
                           entity,
                           hits);
                if (!hits.empty()) {
                    applyHits(reg, entity, Faction::of(reg, entity),
                              hits, def, base, enchResult,
                              damageMult, knockbackMult, out);
                    if (res) res->onHit(false, derived.resonanceGainMult);
                } else {
                    if (res) res->onMiss();
                }
                out.didCastSpell = true;
            }
            else if (def.style == AttackStyle::Buff) {
                out.didCastSpell = true;
            }

            enterPhase(*wstate, WeaponState::Active);
        }
        break;
    }

    case WeaponState::Active: {
        wstate->phaseTime += dt;
        wstate->swingAnim = 1.f;
        if (wstate->phaseTime >= 0.05f) {
            enterPhase(*wstate, WeaponState::Recovery);
        }
        break;
    }

    case WeaponState::Recovery: {
        wstate->phaseTime += dt;
        f32 t = wstate->phaseTime / std::max(0.01f, recoveryDur);
        wstate->swingAnim = std::max(0.f, 1.f - t);

        if (wstate->phaseTime >= recoveryDur) {
            enterPhase(*wstate, WeaponState::Idle);
        }

        if (wstate->comboTimer > 0.f) {
            wstate->comboTimer -= dt;
            if (wstate->comboTimer <= 0.f) {
                wstate->comboCounter = 0;
            }
        }
        break;
    }

    } // switch
}

bool tryStartAttack(ecs::Registry& reg, ecs::Entity entity) {
    auto* wstate = reg.get<WeaponState>(entity);
    if (!wstate) return false;
    if (wstate->phase != WeaponState::Idle) return false;
    auto* weapon = reg.get<EquippedWeapon>(entity);
    if (!weapon || weapon->weaponId == 0) return false;
    enterPhase(*wstate, WeaponState::Windup);
    return true;
}

} // namespace combat