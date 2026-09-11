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

    // Phase 9: награда опытом за убийство
    u64      xpReward   = 0;

    f32      spawnWeight = 1.f;
    bool     spawnsInLight = false;
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