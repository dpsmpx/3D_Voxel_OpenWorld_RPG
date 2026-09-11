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

enum MobPartSlot : u8 {
    Part_Body = 0,
    Part_Head,
    Part_LegFR, Part_LegFL, Part_LegBR, Part_LegBL,
    Part_Tail,
    Part_ArmR, Part_ArmL,
    Part_MAX
};

enum class PartAnim : u8 {
    None,
    Leg,
    LegOpp,
    Head,
    Tail,
    Arm,
    ArmOpp,
};

struct MobPart {
    glm::vec3 offset{0};
    glm::vec3 size{0};
    u32       color = 0xFFFFFFFF;
    PartAnim  anim  = PartAnim::None;
    u8        _pad[3] = {0,0,0};
};

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

    u8       partCount = 0;
    MobPart  parts[Part_MAX];

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
