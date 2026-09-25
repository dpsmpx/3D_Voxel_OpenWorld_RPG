/**
 * @file mob_def.cpp
 * @brief Мобы: определения, конечный автомат ИИ, спавн, боссы.
 */
#include "mob_def.h"
#include "../config/localization.h"
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

    // ---- Урон ----
    //
    // Числа урона подобраны против игрока первого уровня: полтораста
    // здоровья, шесть процентов защиты. Раньше волк кусал на четыре
    // раз в полторы секунды, а здоровье отрастало и посреди драки —
    // стоящего столбом игрока первый зверь убивал больше минуты, и
    // бой не стоил ничего. Теперь одиночный волк убивает за двадцать
    // с небольшим секунд, стая из трёх — за семь-восемь, разбойник —
    // секунд за пятнадцать. Уйти из-под замаха, закрыться, парировать
    // и отступить — всё это снова имеет цену.

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
        d.attackDamage = 10.f;
        d.attackRange = 1.7f;
        d.aggroRange = 14.f;
        d.bodyRadius = 0.5f;
        d.bodyHeight = 1.0f;
        d.eyeHeight = 0.85f;
        d.hostile = true;
        d.canSwim = true;
        d.xpReward = 40;
        // Волк охотится и днём. Он зверь, а не нежить: прятаться от
        // солнца ему незачем.
        d.dayActive = true;
        // Волк бьёт коротко: это его характер — успеть отойти можно,
        // но только если смотришь на него.
        d.windupTime   = 0.30f;
        d.attackShape  = anim::AttackShape::Bite;
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
        d.attackDamage = 12.f;
        d.attackRange = 2.0f;
        d.aggroRange = 16.f;
        d.bodyRadius = 0.35f;
        d.bodyHeight = 1.9f;
        d.eyeHeight = 1.7f;
        d.hostile = true;
        d.canSwim = false;
        d.xpReward = 55;
        d.windupTime   = 0.45f;
        d.attackShape  = anim::AttackShape::Slash;
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
        d.attackDamage = 8.f;
        d.attackRange = 1.6f;
        d.aggroRange = 12.f;
        d.bodyRadius = 0.4f;
        d.bodyHeight = 1.4f;
        d.eyeHeight = 1.2f;
        d.hostile = true;
        d.canSwim = true;
        d.xpReward = 35;
        // Гоблин мельче и быстрее всех: замах едва заметен. Против него
        // помогает не уклонение, а то, чтобы не подпускать.
        d.windupTime   = 0.26f;
        d.attackShape  = anim::AttackShape::Stab;
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
        d.attackDamage = 8.f;
        d.attackRange = 1.4f;
        d.aggroRange = 10.f;
        d.bodyRadius = 0.6f;
        d.bodyHeight = 1.0f;
        d.eyeHeight = 0.6f;
        d.hostile = true;
        d.canSwim = true;
        d.xpReward = 50;
        // Слизень собирается в комок и прыгает: долго и очень заметно.
        d.windupTime   = 0.55f;
        d.attackShape  = anim::AttackShape::Bite;
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
        d.attackDamage = 8.f;
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
        d.poisonDps  = 3.2f;
        d.poisonTime = 5.f;
        // Лечится, когда ей плохо: раз в шесть секунд по восьмой
        // части запаса. Добивать надо быстро.
        d.healAmount = 8.f;
        d.healPeriod = 6.f;
        d.healBelow  = 0.6f;
        // Ведьма читает заклинание — самый длинный замах среди обычных
        // тварей, и единственная, от кого уклониться проще всего.
        d.windupTime   = 0.65f;
        d.attackShape  = anim::AttackShape::Cast;
        defs_[MOB_WITCH] = d;
    }

    // -------------------- КАБАН --------------------
    //
    // Первый зверь, который опасен при свете. Медленнее волка на
    // разгоне, но здоровее и бьёт сильнее: волка переживают бегом,
    // кабана — только дракой или деревом.
    {
        MobDef d{};
        d.name = "Boar";
        d.category = MobCategory::Hostile;
        d.maxHealth = 26.f;
        d.walkSpeed = 1.9f;
        d.chaseSpeed = 4.6f;
        d.attackDamage = 16.f;
        d.attackRange = 1.8f;
        d.aggroRange = 11.f;
        d.bodyRadius = 0.55f;
        d.bodyHeight = 1.1f;
        d.eyeHeight = 0.9f;
        d.hostile = true;
        d.canSwim = true;
        d.dayActive = true;
        d.xpReward = 60;
        // Кабан разгоняется всем телом: тяжело и небыстро.
        d.windupTime   = 0.50f;
        d.attackShape  = anim::AttackShape::Bite;
        defs_[MOB_BOAR] = d;
    }

    // -------------------- РАЗБОЙНИК --------------------
    //
    // Двуногий и с оружием: бьёт дальше зверя и быстрее его думает.
    // Водится у дорог — из-за него дорога и перестаёт быть простой
    // дорогой.
    {
        MobDef d{};
        d.name = "Bandit";
        d.category = MobCategory::Hostile;
        d.maxHealth = 30.f;
        d.walkSpeed = 2.2f;
        d.chaseSpeed = 4.4f;
        d.attackDamage = 15.f;
        d.attackRange = 2.1f;
        d.aggroRange = 16.f;
        d.bodyRadius = 0.35f;
        d.bodyHeight = 1.8f;
        d.eyeHeight = 1.62f;
        d.hostile = true;
        d.canSwim = true;
        d.dayActive = true;
        d.xpReward = 90;
        d.windupTime   = 0.38f;
        d.attackShape  = anim::AttackShape::Slash;
        defs_[MOB_BANDIT] = d;
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
        d.attackDamage = 32.f;
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
        d.slamDamage = 45.f;
        // Страж бьёт каменной рукой сверху вниз. Замах вдвое длиннее
        // волчьего, и это не поблажка: под удар по площади (ещё
        // вдвое дольше) надо успеть выйти из круга в пять метров.
        d.windupTime   = 0.80f;
        d.attackShape  = anim::AttackShape::Chop;
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
        d.attackDamage = 26.f;
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
        // Владыка берёт скоростью: замах вдвое короче, чем у Стража, и
        // на последней фазе укорачивается ещё.
        d.windupTime   = 0.42f;
        d.attackShape  = anim::AttackShape::Slash;
        defs_[MOB_BOSS_HOLLOW] = d;
    }

    LOGI("MobRegistry: зарегистрировано %d мобов", (int)MOB_COUNT - 1);
}

const MobRegistry& MobRegistry::instance() {
    static MobRegistry r;
    return r;
}

const char* MobRegistry::name(u16 id) const {
    return config::tr(get(id).name);
}

const MobDef& MobRegistry::get(u16 id) const {
    if (id >= MOB_COUNT) return defs_[MOB_NONE];
    return defs_[id];
}

} // namespace mobs
