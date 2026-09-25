/**
 * @file weapon.cpp
 * @brief Бой: урон, оружие, зачарования, система «Резонанс», статусы.
 */
#include "weapon.h"
#include "../core/log.h"

namespace combat {

namespace {
constexpr DamageType PHYS   = DamageType::Physical;
constexpr DamageType FIRE   = DamageType::Fire;
constexpr DamageType FROST  = DamageType::Frost;
constexpr DamageType ARCANE = DamageType::Arcane;
}

WeaponRegistry::WeaponRegistry() {
    // Пустая рука тоже бьёт: голыми руками дерутся безоружные NPC, и
    // движение им нужно такое же настоящее. Заполнено только то, чем
    // это движение задаётся; «нет оружия» WEAPON_NONE остаётся во
    // всём остальном — урон, досягаемость и цена у него нулевые.
    defs_[WEAPON_NONE].shape = anim::AttackShape::Stab;
    // Голыми руками тоже закрываются — предплечьем. Немного, но не
    // ноль: иначе безоружный не мог бы ни блокировать, ни отбивать.
    defs_[WEAPON_NONE].guardFraction = 0.15f;

    // -------------------- Iron Sword --------------------
    {
        WeaponDef d{};
        d.name              = "Iron Sword";
        d.style             = AttackStyle::Melee;
        d.damageType        = PHYS;
        d.baseDamage        = 8.f;
        d.reach             = 2.4f;
        d.coneAngle         = 1.2f;
        d.projectileSpeed   = 0.f;
        d.projectileGravity = 0.f;
        d.manaCost          = 0.f;
        d.staminaCost       = 4.f;
        d.critChance        = 0.10f;
        d.critMult          = 1.5f;
        d.knockback         = 1.5f;
        d.windupTime        = 0.14f;
        d.recoveryTime      = 0.32f;
        d.shape             = anim::AttackShape::Slash;
        // Меч держит удар лучше всех клинков: за него и берут.
        d.guardFraction     = 0.70f;
        defs_[WEAPON_IRON_SWORD] = d;
    }

    // -------------------- Iron Axe --------------------
    {
        WeaponDef d{};
        d.name              = "Iron Axe";
        d.style             = AttackStyle::Melee;
        d.damageType        = PHYS;
        d.baseDamage        = 14.f;
        d.reach             = 2.2f;
        d.coneAngle         = 1.4f;
        d.projectileSpeed   = 0.f;
        d.projectileGravity = 0.f;
        d.manaCost          = 0.f;
        d.staminaCost       = 8.f;
        d.critChance        = 0.08f;
        d.critMult          = 1.7f;
        d.knockback         = 2.8f;
        d.windupTime        = 0.24f;
        d.recoveryTime      = 0.55f;
        d.shape             = anim::AttackShape::Chop;
        // Топор тяжёл, но его не подставишь плашмя.
        d.guardFraction     = 0.55f;
        defs_[WEAPON_IRON_AXE] = d;
    }

    // -------------------- Iron Spear --------------------
    {
        WeaponDef d{};
        d.name              = "Iron Spear";
        d.style             = AttackStyle::Melee;
        d.damageType        = PHYS;
        d.baseDamage        = 11.f;
        d.reach             = 3.2f;
        d.coneAngle         = 0.5f;
        d.projectileSpeed   = 0.f;
        d.projectileGravity = 0.f;
        d.manaCost          = 0.f;
        d.staminaCost       = 5.f;
        d.critChance        = 0.09f;
        d.critMult          = 1.6f;
        d.knockback         = 2.0f;
        d.windupTime        = 0.18f;
        d.recoveryTime      = 0.42f;
        d.shape             = anim::AttackShape::Thrust;
        // Древком отбиваются, но держит оно немного.
        d.guardFraction     = 0.45f;
        defs_[WEAPON_IRON_SPEAR] = d;
    }

    // -------------------- Iron Dagger --------------------
    {
        WeaponDef d{};
        d.name              = "Iron Dagger";
        d.style             = AttackStyle::Melee;
        d.damageType        = PHYS;
        d.baseDamage        = 5.f;
        d.reach             = 1.7f;
        d.coneAngle         = 1.0f;
        d.projectileSpeed   = 0.f;
        d.projectileGravity = 0.f;
        d.manaCost          = 0.f;
        d.staminaCost       = 3.f;
        d.critChance        = 0.25f;
        d.critMult          = 2.0f;
        d.knockback         = 0.8f;
        d.windupTime        = 0.08f;
        d.recoveryTime      = 0.22f;
        d.shape             = anim::AttackShape::Stab;
        // Кинжалом закрыться нечем. Его игра — парирование,
        // а не блок: окно у всех одно, а вот держать удар он
        // не умеет.
        d.guardFraction     = 0.25f;
        defs_[WEAPON_IRON_DAGGER] = d;
    }

    // -------------------- Hunting Bow --------------------
    {
        WeaponDef d{};
        d.name              = "Hunting Bow";
        d.style             = AttackStyle::Ranged;
        d.damageType        = PHYS;
        d.baseDamage        = 10.f;
        d.reach             = 40.f;
        d.coneAngle         = 0.f;
        d.projectileSpeed   = 38.f;
        d.projectileGravity = 9.8f;
        d.manaCost          = 0.f;
        d.staminaCost       = 4.f;
        d.critChance        = 0.15f;
        d.critMult          = 1.8f;
        d.knockback         = 1.0f;
        d.windupTime        = 0.40f;
        d.recoveryTime      = 0.20f;
        d.shape             = anim::AttackShape::Draw;
        d.guardFraction     = 0.20f;
        defs_[WEAPON_HUNTING_BOW] = d;
    }

    // -------------------- Heavy Crossbow --------------------
    {
        WeaponDef d{};
        d.name              = "Heavy Crossbow";
        d.style             = AttackStyle::Ranged;
        d.damageType        = PHYS;
        d.baseDamage        = 22.f;
        d.reach             = 55.f;
        d.coneAngle         = 0.f;
        d.projectileSpeed   = 60.f;
        d.projectileGravity = 4.5f;
        d.manaCost          = 0.f;
        d.staminaCost       = 8.f;
        d.critChance        = 0.20f;
        d.critMult          = 2.0f;
        d.knockback         = 2.5f;
        d.windupTime        = 1.00f;
        d.recoveryTime      = 0.35f;
        d.shape             = anim::AttackShape::Draw;
        // Ложе арбалета — хоть какая-то доска перед лицом.
        d.guardFraction     = 0.30f;
        defs_[WEAPON_HEAVY_CROSSBOW] = d;
    }

    // -------------------- Throwing Knife --------------------
    {
        WeaponDef d{};
        d.name              = "Throwing Knife";
        d.style             = AttackStyle::Ranged;
        d.damageType        = PHYS;
        d.baseDamage        = 6.f;
        d.reach             = 25.f;
        d.coneAngle         = 0.f;
        d.projectileSpeed   = 28.f;
        d.projectileGravity = 6.0f;
        d.manaCost          = 0.f;
        d.staminaCost       = 2.f;
        d.critChance        = 0.18f;
        d.critMult          = 1.9f;
        d.knockback         = 0.6f;
        d.windupTime        = 0.10f;
        d.recoveryTime      = 0.15f;
        d.shape             = anim::AttackShape::Chop;
        d.guardFraction     = 0.15f;
        defs_[WEAPON_THROWING_KNIFE] = d;
    }

    // -------------------- Fire Staff --------------------
    {
        WeaponDef d{};
        d.name              = "Fire Staff";
        d.style             = AttackStyle::Magic;
        d.damageType        = FIRE;
        d.baseDamage        = 12.f;
        d.reach             = 30.f;
        d.coneAngle         = 0.f;
        d.projectileSpeed   = 22.f;
        d.projectileGravity = 0.f;
        d.manaCost          = 12.f;
        d.staminaCost       = 0.f;
        d.critChance        = 0.10f;
        d.critMult          = 1.8f;
        d.knockback         = 0.5f;
        d.windupTime        = 0.35f;
        d.recoveryTime      = 0.35f;
        d.shape             = anim::AttackShape::Cast;
        // Посох длинный: им закрываются, как шестом.
        d.guardFraction     = 0.35f;
        defs_[WEAPON_FIRE_STAFF] = d;
    }

    // -------------------- Frost Wand --------------------
    {
        WeaponDef d{};
        d.name              = "Frost Wand";
        d.style             = AttackStyle::AoE;
        d.damageType        = FROST;
        d.baseDamage        = 7.f;
        d.reach             = 6.5f;
        d.coneAngle         = 6.28318f;   // полное круговое AoE
        d.projectileSpeed   = 0.f;
        d.projectileGravity = 0.f;
        d.manaCost          = 18.f;
        d.staminaCost       = 0.f;
        d.critChance        = 0.08f;
        d.critMult          = 1.6f;
        d.knockback         = 1.0f;
        d.windupTime        = 0.50f;
        d.recoveryTime      = 0.60f;
        d.shape             = anim::AttackShape::Cast;
        d.guardFraction     = 0.30f;
        defs_[WEAPON_FROST_WAND] = d;
    }

    // -------------------- Arcane Bracelet --------------------
    {
        WeaponDef d{};
        d.name              = "Arcane Bracelet";
        d.style             = AttackStyle::Buff;
        d.damageType        = ARCANE;
        d.baseDamage        = 0.f;
        d.reach             = 4.f;
        d.coneAngle         = 6.28318f;
        d.projectileSpeed   = 0.f;
        d.projectileGravity = 0.f;
        d.manaCost          = 25.f;
        d.staminaCost       = 0.f;
        d.critChance        = 0.0f;
        d.critMult          = 1.0f;
        d.knockback         = 0.f;
        d.windupTime        = 0.60f;
        d.recoveryTime      = 0.40f;
        d.shape             = anim::AttackShape::Cast;
        // Браслет не наносит урона вовсе — и держит лучше
        // всего в игре. Это и есть его роль: не оружие, а оберег,
        // и теперь у него она есть.
        d.guardFraction     = 0.75f;
        defs_[WEAPON_ARCANE_BRACELET] = d;
    }

    // -------------------- Ice Needles --------------------
    //
    // Иглы не носят с собой и не куют: каждую лепит мана в момент
    // броска. Поэтому числа здесь — на ОДНУ иглу залпа, а цена — на
    // весь залп; сколько игл в залпе, решает ранг навыка
    // (spells.cpp). Бросок частый, игла лёгкая: это оружие темпа,
    // а не одного тяжёлого удара, как огненный посох.
    {
        WeaponDef d{};
        d.name              = "Ice Needles";
        d.style             = AttackStyle::Magic;
        d.damageType        = FROST;
        d.baseDamage        = 5.f;
        d.reach             = 40.f;
        d.coneAngle         = 0.f;
        d.projectileSpeed   = 38.f;
        d.projectileGravity = 0.f;
        d.manaCost          = 7.f;
        d.staminaCost       = 0.f;
        d.critChance        = 0.12f;
        d.critMult          = 1.6f;
        d.knockback         = 0.2f;
        d.windupTime        = 0.10f;
        d.recoveryTime      = 0.30f;
        d.shape             = anim::AttackShape::Cast;
        // Голая ладонь: закрываются ею чуть лучше, чем кулаком.
        d.guardFraction     = 0.20f;
        defs_[WEAPON_ICE_NEEDLES] = d;
    }

    // -------------------- Flame --------------------
    //
    // Струя огня из ладони. manaCost здесь — ЗА СЕКУНДУ удержания,
    // baseDamage — урон в секунду по всем, кто в конусе; reach —
    // длина струи, coneAngle — её раствор. Поджигает и цель, и то,
    // во что упирается (world/fire.h).
    {
        WeaponDef d{};
        d.name              = "Flame";
        d.style             = AttackStyle::Stream;
        d.damageType        = FIRE;
        d.baseDamage        = 16.f;
        d.reach             = 5.5f;
        d.coneAngle         = 0.55f;
        d.projectileSpeed   = 0.f;
        d.projectileGravity = 0.f;
        d.manaCost          = 14.f;
        d.staminaCost       = 0.f;
        d.critChance        = 0.f;
        d.critMult          = 1.f;
        d.knockback         = 0.f;
        d.windupTime        = 0.f;
        d.recoveryTime      = 0.f;
        d.shape             = anim::AttackShape::Cast;
        d.guardFraction     = 0.20f;
        defs_[WEAPON_FLAME] = d;
    }

    LOGI("WeaponRegistry: зарегистрировано %d оружий", (int)WEAPON_COUNT - 1);
}

const WeaponRegistry& WeaponRegistry::instance() {
    static WeaponRegistry r;
    return r;
}

const WeaponDef& WeaponRegistry::get(u16 id) const {
    if (id >= WEAPON_MAX_DEFS) return defs_[WEAPON_NONE];
    return defs_[id];
}

} // namespace combat
