/**
 * @file progression.h
 * @brief Прогрессия: опыт, уровни, атрибуты, Древо Познания.
 */
#pragma once
#include "../core/types.h"
#include "../ecs/components.h"
#include "../ecs/registry.h"
#include "attributes.h"
#include "skill_tree.h"

namespace progression {

/// Прогрессия: опыт, уровень, очки, кэш производных.
constexpr u32 MAX_LEVEL = 100;

/// Сколько очков навыков даётся за уровень
constexpr i32 SKILL_POINTS_PER_LEVEL = 1;

/// Формула опыта. Используется UI и логикой.
///   xpForLevel(n) — сколько XP нужно, чтобы перейти с (n-1) на n.
///   xpTotalForLevel(n) — сколько XP суммарно нужно для достижения n.
u64 xpForLevel(u32 level);
/// Суммарный опыт, необходимый для достижения уровня.
/// @param level целевой уровень, 1..MAX_LEVEL
/// @return накопленный опыт с первого уровня
u64 xpTotalForLevel(u32 level);

/// Компонент прогрессии.
struct Progression {
    u64 xp                = 0;      // текущий суммарный опыт
    u32 level             = 1;
    i32 availableAttrPoints = 0;    // очки атрибутов (не навыков)
    u32 pendingLevelUps    = 0;     // для всплывающего уведомления

    /// Кэш производных (пересчитывается при изменении)
    DerivedStats derived{};
    bool derivedDirty = true;

    /// XP для следующего уровня, уже посчитанное
    u64 xpForNext = 0;

    /// Возвращает true, если в результате добавления XP подняли уровень.
    /// levelUpsOut — сколько уровней получено за один вызов.
    bool addXP(u64 amount, u32& levelUpsOut);

    /// Пересчитать производные
    void recalc(const ecs::Attributes& attr, const SkillTree& tree);

    /// Полный XP внутри текущего уровня
    u64 xpWithinLevel() const {
        u64 base = xpTotalForLevel(level);
        return (xp > base) ? (xp - base) : 0;
    }
    u64 xpForNextLevel() const {
        return xpForLevel(level + 1);
    }
    f32 levelProgress() const {
        u64 need = xpForNextLevel();
        if (need == 0) return 1.f;
        u64 have = xpWithinLevel();
        f32 p = (f32)have / (f32)need;
        if (p < 0.f) p = 0.f;
        if (p > 1.f) p = 1.f;
        return p;
    }
};

/// Точка входа: обработка смерти врага — награда XP.
/// Возвращает true, если игрок поднял уровень (для UI-эффектов).
struct LevelUpEvent {
    bool happened = false;
    u32  newLevel = 1;
    u32  levelsGained = 0;
};

/// Начислить награду за убийство моба.
/// attacker должен иметь Progression.
LevelUpEvent rewardKillXP(ecs::Registry& reg,
                          ecs::Entity attacker,
                          u64 xpReward);

/// Обновление всех прогрессий на кадр:
///   - пересчёт производных, если derivedDirty
///   - health/mana/stamina clamped к max
///   - регенерация ресурсов
///   - тик статусов (вынесено отдельно, см. combat)
void tickProgression(ecs::Registry& reg, f32 dt);

/// Производные — быстрый доступ через кэш компонента.
const DerivedStats& derivedOf(ecs::Registry& reg, ecs::Entity e);

} // namespace progression
