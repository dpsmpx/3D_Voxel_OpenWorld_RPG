/**
 * @file progression.cpp
 * @brief Прогрессия: опыт, уровни, атрибуты, Древо Познания.
 */
#include "progression.h"
#include "../combat/resonance.h"
#include "resource_regen.h"
#include "../core/log.h"
#include "../ecs/components.h"
#include <algorithm>
#include <cmath>

namespace progression {

// ============================================================
// Кривые XP. Квадратичный рост с плавным ускорением.
// 1 → 2: 150 XP
// 5 → 6: 3000 XP
// 10 → 11: 11500 XP
// 50 → 51: 265500 XP
// 99 → 100: 1 000 000 XP
// ============================================================
u64 xpForLevel(u32 level) {
    if (level <= 1) return 0;
    if (level > MAX_LEVEL) return 0;
    const u64 n = (u64)level - 1;   // 0-based
    // Формула: 50 * n^2 + 100 * n
    // Проверка на переполнение: для MAX_LEVEL=100 → 100^2 * 50 = 500 000
    return 50ULL * n * n + 100ULL * n;
}

u64 xpTotalForLevel(u32 level) {
    if (level <= 1) return 0;
    u64 sum = 0;
    for (u32 i = 2; i <= level; ++i) {
        sum += xpForLevel(i);
    }
    return sum;
}

// ============================================================
// Progression
// ============================================================
bool Progression::addXP(u64 amount, u32& levelUpsOut) {
    levelUpsOut = 0;
    if (amount == 0) return false;
    if (level >= MAX_LEVEL) {
        xp += amount;
        return false;
    }

    xp += amount;

    bool leveled = false;
    while (level < MAX_LEVEL) {
        u64 need = xpTotalForLevel(level + 1);
        if (xp < need) break;

        ++level;
        ++levelUpsOut;
        ++pendingLevelUps;
        availableAttrPoints += ATTR_POINTS_PER_LEVEL;
        derivedDirty = true;
        leveled = true;
    }

    return leveled;
}

void Progression::recalc(const ecs::Attributes& attr, const SkillTree& tree) {
    derived = computeDerived(attr, tree, level);
    derivedDirty = false;
}

// ============================================================
// Награда за убийство
// ============================================================
void rewardKillXP(ecs::Registry& reg,
                  ecs::Entity attacker,
                  u64 xpReward)
{
    auto* prog = reg.get<Progression>(attacker);
    if (!prog) return;

    // Сколько уровней взято за раз, наружу не отдаём: уведомление
    // игрок поднимает сам, по pendingLevelUps. Возвращался LevelUpEvent,
    // и все три его поля выбрасывал единственный вызывающий.
    u32 gained = 0;
    prog->addXP(xpReward, gained);
}

// ============================================================
// tickProgression
// ============================================================
namespace {

void clampResourcesToMax(ecs::Registry& reg, ecs::Entity e,
                         const DerivedStats& d)
{
    if (auto* h = reg.get<ecs::Health>(e)) {
        if (h->max != d.maxHealth) {
            f32 pct = (h->max > 0.f) ? h->current / h->max : 1.f;
            h->max = d.maxHealth;
            h->current = pct * h->max;
            if (h->current > h->max) h->current = h->max;
            if (h->current < 0.f) h->current = 0.f;
        }
    }
    if (auto* m = reg.get<ecs::Mana>(e)) {
        if (m->max != d.maxMana) {
            f32 pct = (m->max > 0.f) ? m->current / m->max : 1.f;
            m->max = d.maxMana;
            m->current = pct * m->max;
            if (m->current > m->max) m->current = m->max;
            if (m->current < 0.f) m->current = 0.f;
        }
    }
    // Личный потолок резонанса. Узел «Resonance Master» считался и
    // никуда не доходил: RESONANCE_MAX — константа компиляции, и три
    // ранга в самом конце ветки Мудрости не делали ничего.
    if (auto* res = reg.get<combat::ResonanceState>(e)) {
        res->maxValue = combat::RESONANCE_MAX * d.maxResonanceMult;
        if (res->value > res->maxValue) res->value = res->maxValue;
    }
    if (auto* s = reg.get<ecs::Stamina>(e)) {
        if (s->max != d.maxStamina) {
            f32 pct = (s->max > 0.f) ? s->current / s->max : 1.f;
            s->max = d.maxStamina;
            s->current = pct * s->max;
            if (s->current > s->max) s->current = s->max;
            if (s->current < 0.f) s->current = 0.f;
        }
    }
}

} // namespace

void tickProgression(ecs::Registry& reg, f32 dt) {
    if (dt <= 0.f) return;

    auto& progPool = reg.pool<Progression>();
    const usize n = progPool.size();

    for (usize i = 0; i < n; ++i) {
        ecs::Entity e = progPool.entityAt((u32)i);
        auto* prog = progPool.get(e);
        if (!prog) continue;

        // Действие эликсиров. Кончилось — производные пересчитать:
        // иначе сила осталась бы поднятой навсегда.
        auto* buffs = reg.get<AttributeBuffs>(e);
        if (buffs && buffs->tick(dt)) prog->derivedDirty = true;

        // Пересчёт производных при изменении
        if (prog->derivedDirty) {
            auto* attr = reg.get<ecs::Attributes>(e);
            auto* tree = reg.get<SkillTree>(e);
            ecs::Attributes defaultAttr{};
            SkillTree defaultTree{};
            if (!attr) attr = &defaultAttr;
            if (!tree) tree = &defaultTree;
            prog->recalc(boosted(*attr, buffs), *tree);
        }

        const auto& d = prog->derived;

        // Применяем max к ресурсам (если менялись)
        clampResourcesToMax(reg, e, d);

        // Регенерация ресурсов
        if (auto* h = reg.get<ecs::Health>(e)) {
            h->sinceHurt += dt;
            if (h->current > 0.f && h->current < h->max &&
                h->sinceHurt >= HEALTH_REGEN_DELAY) {
                h->current = std::min(h->max,
                    h->current + d.healthRegen * dt);
            }
            if (h->invulnTime > 0.f) {
                h->invulnTime -= dt;
                if (h->invulnTime < 0.f) h->invulnTime = 0.f;
            }
        }
        if (auto* m = reg.get<ecs::Mana>(e)) {
            if (m->current < m->max) {
                m->current = std::min(m->max,
                    m->current + d.manaRegen * dt);
            }
        }
        // Утомление спадает само, но не сразу: отдыхом считается
        // время БЕЗ нагрузки, и отсчёт его сбрасывает всякий рывок и
        // всякий удар (см. addFatigue).
        if (auto* f = reg.get<ecs::Fatigue>(e)) {
            f->restTimer += dt;
            if (f->restTimer >= FATIGUE_REST_DELAY && f->value > 0.f) {
                f->value = std::max(0.f,
                    f->value - dt / FATIGUE_RECOVER_SEC);
            }
        }

        if (auto* s = reg.get<ecs::Stamina>(e)) {
            // Реген идёт не всегда: пока тратишь — не восстанавливается.
            // Иначе бег не стоил бы ничего, реген его перекрывал бы.
            // У кого нет утомления (мобы, жители) — реген как был.
            const auto* fat = reg.get<ecs::Fatigue>(e);
            const bool resting = !fat || fat->restTimer >= STAMINA_REGEN_DELAY;

            // И упирается он в ПОТОЛОК, который опускает утомление.
            // Прежде он упирался в max, и разницы между свежим героем
            // и тем, кто третью минуту отбивается от стаи, не
            // оставалось никакой.
            const f32 cap = staminaCeiling(reg, e);
            if (resting && s->current < cap) {
                s->current = std::min(cap,
                    s->current + d.staminaRegen * dt);
            }
        }
    }
}

const DerivedStats& derivedOf(ecs::Registry& reg, ecs::Entity e) {
    static DerivedStats defaultStats{};
    auto* prog = reg.get<Progression>(e);
    if (!prog) return defaultStats;
    return prog->derived;
}

} // namespace progression
