/**
 * @file mob_def.cpp
 * @brief Мобы: определения, конечный автомат ИИ, спавн, боссы.
 */
#include "mob_def.h"
#include "../core/log.h"

namespace mobs {

namespace {
constexpr u32 rgb(u8 r, u8 g, u8 b) {
    return ((u32)r << 24) | ((u32)g << 16) | ((u32)b << 8) | 0xFF;
}
}

MobRegistry::MobRegistry() {
    // -------------------- SHEEP --------------------
    {
        MobDef d{};
        d.name = "Sheep";
        d.category = MobCategory::Passive;
        d.maxHealth = 8.f;
        d.walkSpeed = 1.6f;
        d.chaseSpeed = 2.2f;
        d.attackDamage = 0.f;
        d.attackRange = 0.f;
        d.aggroRange = 0.f;
        d.bodyRadius = 0.5f;
        d.bodyHeight = 1.2f;
        d.eyeHeight = 1.0f;
        d.hostile = false;
        d.canSwim = true;
        d.spawnWeight = 1.2f;
        d.spawnsInLight = true;
        d.dropBlock = 15;
        d.dropMin = 1; d.dropMax = 2;
        d.xpReward = 12;

        d.partCount = 6;
        d.parts[Part_Body] = { {0, 0.6f, 0}, {0.7f, 0.6f, 1.1f}, rgb(220,220,215), PartAnim::None };
        d.parts[Part_Head] = { {0, 0.95f, 0.72f}, {0.55f, 0.55f, 0.5f}, rgb(200,200,195), PartAnim::Head };
        d.parts[Part_LegFR] = { { 0.22f, 0.3f,  0.35f}, {0.18f, 0.6f, 0.18f}, rgb(120,120,115), PartAnim::Leg };
        d.parts[Part_LegFL] = { {-0.22f, 0.3f,  0.35f}, {0.18f, 0.6f, 0.18f}, rgb(120,120,115), PartAnim::LegOpp };
        d.parts[Part_LegBR] = { { 0.22f, 0.3f, -0.35f}, {0.18f, 0.6f, 0.18f}, rgb(120,120,115), PartAnim::LegOpp };
        d.parts[Part_LegBL] = { {-0.22f, 0.3f, -0.35f}, {0.18f, 0.6f, 0.18f}, rgb(120,120,115), PartAnim::Leg };
        defs_[MOB_SHEEP] = d;
    }

    // -------------------- COW --------------------
    {
        MobDef d{};
        d.name = "Cow";
        d.category = MobCategory::Passive;
        d.maxHealth = 12.f;
        d.walkSpeed = 1.4f;
        d.chaseSpeed = 2.0f;
        d.bodyRadius = 0.55f;
        d.bodyHeight = 1.35f;
        d.eyeHeight = 1.15f;
        d.canSwim = true;
        d.spawnWeight = 1.0f;
        d.spawnsInLight = true;
        d.dropBlock = 0;
        d.dropMin = 0; d.dropMax = 0;
        d.xpReward = 20;

        d.partCount = 6;
        d.parts[Part_Body] = { {0, 0.75f, 0}, {0.8f, 0.75f, 1.3f}, rgb(80,60,40), PartAnim::None };
        d.parts[Part_Head] = { {0, 1.15f, 0.85f}, {0.6f, 0.55f, 0.55f}, rgb(230,230,220), PartAnim::Head };
        d.parts[Part_LegFR] = { { 0.25f, 0.35f,  0.4f}, {0.2f, 0.7f, 0.2f}, rgb(60,45,30), PartAnim::Leg };
        d.parts[Part_LegFL] = { {-0.25f, 0.35f,  0.4f}, {0.2f, 0.7f, 0.2f}, rgb(60,45,30), PartAnim::LegOpp };
        d.parts[Part_LegBR] = { { 0.25f, 0.35f, -0.4f}, {0.2f, 0.7f, 0.2f}, rgb(60,45,30), PartAnim::LegOpp };
        d.parts[Part_LegBL] = { {-0.25f, 0.35f, -0.4f}, {0.2f, 0.7f, 0.2f}, rgb(60,45,30), PartAnim::Leg };
        defs_[MOB_COW] = d;
    }

    // -------------------- CHICKEN --------------------
    {
        MobDef d{};
        d.name = "Chicken";
        d.category = MobCategory::Passive;
        d.maxHealth = 4.f;
        d.walkSpeed = 1.8f;
        d.chaseSpeed = 2.4f;
        d.bodyRadius = 0.3f;
        d.bodyHeight = 0.7f;
        d.eyeHeight = 0.55f;
        d.canSwim = false;
        d.spawnWeight = 0.9f;
        d.spawnsInLight = true;
        d.dropBlock = 0;
        d.dropMin = 0; d.dropMax = 0;
        d.xpReward = 8;

        d.partCount = 4;
        d.parts[Part_Body] = { {0, 0.4f, 0}, {0.35f, 0.35f, 0.5f}, rgb(240,240,240), PartAnim::None };
        d.parts[Part_Head] = { {0, 0.65f, 0.3f}, {0.25f, 0.25f, 0.25f}, rgb(240,240,240), PartAnim::Head };
        d.parts[Part_LegFR] = { { 0.1f, 0.15f,  0.12f}, {0.06f, 0.3f, 0.06f}, rgb(220,140,20), PartAnim::Leg };
        d.parts[Part_LegFL] = { {-0.1f, 0.15f,  0.12f}, {0.06f, 0.3f, 0.06f}, rgb(220,140,20), PartAnim::LegOpp };
        defs_[MOB_CHICKEN] = d;
    }

    // -------------------- WOLF --------------------
    {
        MobDef d{};
        d.name = "Wolf";
        d.category = MobCategory::Hostile;
        d.maxHealth = 14.f;
        d.walkSpeed = 2.5f;
        d.chaseSpeed = 5.5f;
        d.attackDamage = 4.f;
        d.attackRange = 1.7f;
        d.aggroRange = 14.f;
        d.bodyRadius = 0.5f;
        d.bodyHeight = 1.0f;
        d.eyeHeight = 0.85f;
        d.hostile = true;
        d.canSwim = true;
        d.spawnWeight = 0.8f;
        d.spawnsInLight = false;
        d.dropBlock = 0;
        d.dropMin = 0; d.dropMax = 0;
        d.xpReward = 40;

        d.partCount = 7;
        d.parts[Part_Body] = { {0, 0.55f, 0}, {0.6f, 0.55f, 1.2f}, rgb(80,80,90), PartAnim::None };
        d.parts[Part_Head] = { {0, 0.75f, 0.75f}, {0.45f, 0.4f, 0.5f}, rgb(70,70,80), PartAnim::Head };
        d.parts[Part_Tail] = { {0, 0.6f, -0.7f}, {0.15f, 0.15f, 0.4f}, rgb(60,60,70), PartAnim::Tail };
        d.parts[Part_LegFR] = { { 0.2f, 0.25f,  0.4f}, {0.15f, 0.5f, 0.15f}, rgb(50,50,60), PartAnim::Leg };
        d.parts[Part_LegFL] = { {-0.2f, 0.25f,  0.4f}, {0.15f, 0.5f, 0.15f}, rgb(50,50,60), PartAnim::LegOpp };
        d.parts[Part_LegBR] = { { 0.2f, 0.25f, -0.4f}, {0.15f, 0.5f, 0.15f}, rgb(50,50,60), PartAnim::LegOpp };
        d.parts[Part_LegBL] = { {-0.2f, 0.25f, -0.4f}, {0.15f, 0.5f, 0.15f}, rgb(50,50,60), PartAnim::Leg };
        defs_[MOB_WOLF] = d;
    }

    // -------------------- SKELETON --------------------
    {
        MobDef d{};
        d.name = "Skeleton";
        d.category = MobCategory::Hostile;
        d.maxHealth = 18.f;
        d.walkSpeed = 1.8f;
        d.chaseSpeed = 3.6f;
        d.attackDamage = 5.f;
        d.attackRange = 2.0f;
        d.aggroRange = 16.f;
        d.bodyRadius = 0.35f;
        d.bodyHeight = 1.9f;
        d.eyeHeight = 1.7f;
        d.hostile = true;
        d.canSwim = false;
        d.spawnWeight = 0.6f;
        d.spawnsInLight = false;
        d.dropBlock = 0;
        d.dropMin = 0; d.dropMax = 0;
        d.xpReward = 55;

        d.partCount = 7;
        d.parts[Part_Body]  = { {0, 1.15f, 0}, {0.45f, 0.65f, 0.3f}, rgb(225,225,220), PartAnim::None };
        d.parts[Part_Head]  = { {0, 1.75f, 0}, {0.4f, 0.4f, 0.4f}, rgb(230,230,225), PartAnim::Head };
        d.parts[Part_LegFR] = { { 0.12f, 0.4f, 0}, {0.12f, 0.8f, 0.12f}, rgb(210,210,205), PartAnim::Leg };
        d.parts[Part_LegFL] = { {-0.12f, 0.4f, 0}, {0.12f, 0.8f, 0.12f}, rgb(210,210,205), PartAnim::LegOpp };
        d.parts[Part_ArmR]  = { { 0.3f, 1.1f, 0}, {0.12f, 0.7f, 0.12f}, rgb(215,215,210), PartAnim::Arm };
        d.parts[Part_ArmL]  = { {-0.3f, 1.1f, 0}, {0.12f, 0.7f, 0.12f}, rgb(215,215,210), PartAnim::ArmOpp };
        d.parts[Part_Tail]  = { {0,0,0}, {0,0,0}, 0, PartAnim::None };
        defs_[MOB_SKELETON] = d;
    }

    // -------------------- GOBLIN --------------------
    {
        MobDef d{};
        d.name = "Goblin";
        d.category = MobCategory::Hostile;
        d.maxHealth = 12.f;
        d.walkSpeed = 2.4f;
        d.chaseSpeed = 4.8f;
        d.attackDamage = 3.5f;
        d.attackRange = 1.6f;
        d.aggroRange = 12.f;
        d.bodyRadius = 0.4f;
        d.bodyHeight = 1.4f;
        d.eyeHeight = 1.2f;
        d.hostile = true;
        d.canSwim = true;
        d.spawnWeight = 0.9f;
        d.spawnsInLight = false;
        d.dropBlock = 0;
        d.dropMin = 0; d.dropMax = 0;
        d.xpReward = 35;

        d.partCount = 7;
        d.parts[Part_Body]  = { {0, 0.85f, 0}, {0.55f, 0.6f, 0.4f}, rgb(90,150,60), PartAnim::None };
        d.parts[Part_Head]  = { {0, 1.35f, 0}, {0.5f, 0.5f, 0.45f}, rgb(100,160,70), PartAnim::Head };
        d.parts[Part_LegFR] = { { 0.15f, 0.3f, 0}, {0.15f, 0.6f, 0.15f}, rgb(70,120,50), PartAnim::Leg };
        d.parts[Part_LegFL] = { {-0.15f, 0.3f, 0}, {0.15f, 0.6f, 0.15f}, rgb(70,120,50), PartAnim::LegOpp };
        d.parts[Part_ArmR]  = { { 0.4f, 0.9f, 0}, {0.15f, 0.5f, 0.15f}, rgb(80,140,60), PartAnim::Arm };
        d.parts[Part_ArmL]  = { {-0.4f, 0.9f, 0}, {0.15f, 0.5f, 0.15f}, rgb(80,140,60), PartAnim::ArmOpp };
        d.parts[Part_Tail]  = { {0,0,0}, {0,0,0}, 0, PartAnim::None };
        defs_[MOB_GOBLIN] = d;
    }

    // -------------------- SLIME --------------------
    {
        MobDef d{};
        d.name = "Slime";
        d.category = MobCategory::Hostile;
        d.maxHealth = 20.f;
        d.walkSpeed = 0.9f;
        d.chaseSpeed = 2.8f;
        d.attackDamage = 3.f;
        d.attackRange = 1.4f;
        d.aggroRange = 10.f;
        d.bodyRadius = 0.6f;
        d.bodyHeight = 1.0f;
        d.eyeHeight = 0.6f;
        d.hostile = true;
        d.canSwim = true;
        d.spawnWeight = 0.5f;
        d.spawnsInLight = false;
        d.dropBlock = 0;
        d.dropMin = 0; d.dropMax = 0;
        d.xpReward = 50;

        d.partCount = 2;
        d.parts[Part_Body]  = { {0, 0.5f, 0}, {1.0f, 1.0f, 1.0f}, rgb(80,200,80), PartAnim::None };
        d.parts[Part_Head]  = { {0, 0.5f, 0}, {0.5f, 0.5f, 0.5f}, rgb(120,240,120), PartAnim::None };
        defs_[MOB_SLIME] = d;
    }

    // -------------------- БОСС: КАМЕННЫЙ СТРАЖ --------------------
    // Три фазы: обычные удары, затем добавляется удар по площади,
    // на последней трети здоровья — ускорение и усиленный урон.
    {
        MobDef d{};
        d.name = "Stone Warden";
        d.category = MobCategory::Hostile;
        d.maxHealth = 600.f;
        d.walkSpeed = 1.6f;
        d.chaseSpeed = 3.4f;
        d.attackDamage = 18.f;
        d.attackRange = 3.2f;
        d.aggroRange = 26.f;
        d.bodyRadius = 1.3f;
        d.bodyHeight = 3.4f;
        d.eyeHeight = 2.9f;
        d.hostile = true;
        d.canSwim = false;
        d.spawnWeight = 0.f;      // обычным спавном не появляется
        d.spawnsInLight = true;   // подземелье тёмное, но босс стоит всегда
        d.dropBlock = 0;
        d.dropMin = 0; d.dropMax = 0;
        d.xpReward = 4000;

        d.isBoss     = true;
        d.phaseCount = 3;
        d.enrageMult = 1.7f;
        d.slamRadius = 5.0f;
        d.slamDamage = 26.f;

        d.partCount = 7;
        d.parts[Part_Body]  = { {0, 1.7f, 0},        {1.9f, 2.0f, 1.3f}, rgb(96,102,112),  PartAnim::None };
        d.parts[Part_Head]  = { {0, 3.0f, 0},        {1.1f, 0.9f, 1.1f}, rgb(120,126,138), PartAnim::Head };
        d.parts[Part_ArmR]  = { {1.35f, 1.9f, 0},    {0.7f, 1.9f, 0.7f}, rgb(84,90,100),   PartAnim::Arm };
        d.parts[Part_ArmL]  = { {-1.35f, 1.9f, 0},   {0.7f, 1.9f, 0.7f}, rgb(84,90,100),   PartAnim::ArmOpp };
        d.parts[Part_LegFR] = { {0.55f, 0.4f, 0},    {0.8f, 1.6f, 0.8f}, rgb(72,78,88),    PartAnim::Leg };
        d.parts[Part_LegFL] = { {-0.55f, 0.4f, 0},   {0.8f, 1.6f, 0.8f}, rgb(72,78,88),    PartAnim::LegOpp };
        d.parts[Part_Tail]  = { {0, 3.7f, 0},        {0.5f, 0.5f, 0.5f}, rgb(190,140,60),  PartAnim::None };
        defs_[MOB_BOSS_WARDEN] = d;
    }

    // -------------------- БОСС: ПОЛЫЙ ВЛАДЫКА --------------------
    // Две фазы: во второй резко ускоряется и бьёт сериями.
    {
        MobDef d{};
        d.name = "Hollow Lord";
        d.category = MobCategory::Hostile;
        d.maxHealth = 420.f;
        d.walkSpeed = 2.4f;
        d.chaseSpeed = 5.2f;
        d.attackDamage = 14.f;
        d.attackRange = 2.4f;
        d.aggroRange = 24.f;
        d.bodyRadius = 0.8f;
        d.bodyHeight = 2.6f;
        d.eyeHeight = 2.3f;
        d.hostile = true;
        d.canSwim = false;
        d.spawnWeight = 0.f;
        d.spawnsInLight = true;
        d.xpReward = 2800;

        d.isBoss     = true;
        d.phaseCount = 2;
        d.enrageMult = 2.0f;
        d.slamRadius = 0.f;       // без АоЕ: берёт скоростью
        d.slamDamage = 0.f;

        d.partCount = 6;
        d.parts[Part_Body]  = { {0, 1.3f, 0},      {1.0f, 1.4f, 0.7f}, rgb(48,44,66),    PartAnim::None };
        d.parts[Part_Head]  = { {0, 2.25f, 0},     {0.8f, 0.8f, 0.8f}, rgb(214,208,190), PartAnim::Head };
        d.parts[Part_ArmR]  = { {0.75f, 1.4f, 0},  {0.4f, 1.4f, 0.4f}, rgb(40,36,56),    PartAnim::Arm };
        d.parts[Part_ArmL]  = { {-0.75f, 1.4f, 0}, {0.4f, 1.4f, 0.4f}, rgb(40,36,56),    PartAnim::ArmOpp };
        d.parts[Part_LegFR] = { {0.3f, 0.3f, 0},   {0.4f, 1.2f, 0.4f}, rgb(34,30,48),    PartAnim::Leg };
        d.parts[Part_LegFL] = { {-0.3f, 0.3f, 0},  {0.4f, 1.2f, 0.4f}, rgb(34,30,48),    PartAnim::LegOpp };
        defs_[MOB_BOSS_HOLLOW] = d;
    }

    LOGI("MobRegistry: зарегистрировано %d мобов", (int)MOB_COUNT - 1);
}

const MobRegistry& MobRegistry::instance() {
    static MobRegistry r;
    return r;
}

const MobDef& MobRegistry::get(u16 id) const {
    if (id >= MOB_COUNT) return defs_[MOB_NONE];
    return defs_[id];
}

} // namespace mobs
