/**
 * @file mob_def.h
 * @brief Мобы: определения, конечный автомат ИИ, спавн, боссы.
 */
#pragma once
#include "../core/types.h"
#include <glm/glm.hpp>

namespace mobs {

enum class MobCategory : u8 {
    Passive,
    Hostile,
};

// Здесь лежало плоское описание модели: MobPart, PartAnim и
// массив из девяти коробок со смещениями от начала сущности. Слоты
// были зашиты — тело, голова, четыре ноги, хвост, две руки, — и
// ничего сверх этого выразить было нельзя: ни уха, ни морды, ни рога.
// Поле PartAnim к тому же давно ничего не решало: анимация выбирает
// движение по роли части в оснастке, а не по значению из таблицы.
//
// Вид теперь описывается тем, что он ЕСТЬ — зверем, двуногим или
// комком, — в entity/mob_rigs.cpp. Определение здесь оставляет за
// собой только то, что нужно игровой логике: скорости, урон, размеры
// тела для физики.

struct MobDef {
    const char* name;
    MobCategory category;

    f32 maxHealth;
    f32 walkSpeed;
    f32 chaseSpeed;
    f32 attackDamage;
    f32 attackRange;
    f32 aggroRange;
    f32 bodyRadius;
    f32 bodyHeight;
    f32 eyeHeight;

    bool canSwim;
    bool canFly;
    bool hostile;

    u16      dropBlock  = 0;
    u8       dropMin    = 0;
    u8       dropMax    = 0;

    /// Phase 9: награда опытом за убийство
    u64      xpReward   = 0;

    f32      spawnWeight = 1.f;
    bool     spawnsInLight = false;

    // ---- Боссы (ТЗ 4.5) ----
    /// Босс не появляется обычным спавном: его ставит генератор
    /// подземелья, один на подземелье.
    bool     isBoss      = false;
    /// Сколько фаз у боя. Фаза меняется по порогам здоровья:
    /// граница i — при health/max ниже (phaseCount - i) / phaseCount.
    u8       phaseCount  = 1;
    /// Множитель урона и скорости атаки на последней фазе.
    f32      enrageMult  = 1.6f;
    /// Радиус АоЕ-удара, который босс использует вместо обычной
    /// атаки раз в несколько ударов. 0 — нет особой атаки.
    f32      slamRadius  = 0.f;
    f32      slamDamage  = 0.f;
};

enum MobId : u16 {
    MOB_NONE = 0,
    MOB_SHEEP,
    MOB_COW,
    MOB_CHICKEN,
    MOB_WOLF,
    MOB_SKELETON,
    MOB_GOBLIN,
    MOB_SLIME,

    // Боссы подземелий: спавнятся генератором структур, не спавнером.
    MOB_BOSS_WARDEN,     ///< Каменный Страж — три фазы, удар по площади
    MOB_BOSS_HOLLOW,     ///< Полый Владыка — две фазы, быстрые серии

    MOB_COUNT
};

class MobRegistry {
public:
    static const MobRegistry& instance();
    const MobDef& get(u16 id) const;

private:
    MobRegistry();
    MobDef defs_[MOB_COUNT];
};

inline const MobRegistry& mobRegistry() { return MobRegistry::instance(); }

} // namespace mobs
