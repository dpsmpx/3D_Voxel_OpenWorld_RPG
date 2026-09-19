/**
 * @file mob_def.cpp
 * @brief Мобы: определения, конечный автомат ИИ, спавн, боссы.
 */
#include "mob_def.h"
#include "../core/log.h"

namespace mobs {

// Цвета ушли вместе с плоским описанием модели: вид теперь
// описывается оснасткой в entity/mob_rigs.cpp, там же и его палитра.

MobRegistry::MobRegistry() {
    // MOB_NONE — «никто». Имя ему нужно не для показа, а чтобы
    // get() на неизвестный id возвращал def с ЧИТАЕМЫМ name:
    // именно его подставляют в текст квеста, и нулевой указатель
    // там валит игру в printf, а не в понятном месте.
    defs_[MOB_NONE].name = "nothing";

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
        d.xpReward = 12;
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
        d.xpReward = 20;
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
        d.xpReward = 8;
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
        d.xpReward = 40;
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
        d.xpReward = 55;
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
        d.xpReward = 35;
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
        d.xpReward = 50;
        defs_[MOB_SLIME] = d;
    }

    // -------------------- ВЕДЬМА --------------------
    //
    // Не сильнее гоблина в лоб, и в этом весь смысл: её опасность в
    // том, что бой с ней НЕ КОНЧАЕТСЯ. Удар травит, а сама она
    // залечивается — переждать её, отбежав и отдышавшись, нельзя.
    {
        MobDef d{};
        d.name = "Witch";
        d.category = MobCategory::Hostile;
        d.maxHealth = 34.f;
        d.walkSpeed = 1.5f;
        d.chaseSpeed = 3.0f;
        d.attackDamage = 5.f;
        d.attackRange = 2.0f;
        d.aggroRange = 14.f;
        d.bodyRadius = 0.32f;
        d.bodyHeight = 1.75f;
        d.eyeHeight = 1.6f;
        d.hostile = true;
        d.canSwim = true;
        d.xpReward = 55;
        // Яд: вдвое больше урона, чем сам удар, но растянуто. Бежать
        // от неё бесполезно — отрава идёт следом.
        d.poisonDps  = 2.0f;
        d.poisonTime = 5.f;
        // Лечится, когда ей плохо: раз в шесть секунд по восьмой
        // части запаса. Добивать надо быстро.
        d.healAmount = 8.f;
        d.healPeriod = 6.f;
        d.healBelow  = 0.6f;
        defs_[MOB_WITCH] = d;
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
        d.xpReward = 4000;

        d.isBoss     = true;
        d.phaseCount = 3;
        d.enrageMult = 1.7f;
        d.slamRadius = 5.0f;
        d.slamDamage = 26.f;
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
        d.xpReward = 2800;

        d.isBoss     = true;
        d.phaseCount = 2;
        d.enrageMult = 2.0f;
        d.slamRadius = 0.f;       // без АоЕ: берёт скоростью
        d.slamDamage = 0.f;
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
