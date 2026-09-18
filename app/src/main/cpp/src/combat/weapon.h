/**
 * @file weapon.h
 * @brief Бой: урон, оружие, зачарования, система «Резонанс», статусы.
 */
#pragma once
#include "../core/types.h"
#include "damage.h"
#include <glm/glm.hpp>

namespace combat {

/// Класс оружия. Определяет базовое поведение.

enum class AttackStyle : u8 {
    Melee = 0,
    Ranged,
    Magic,
    AoE,
    Buff,
};

/// Полное определение оружия. Создаётся один раз в реестре.
struct WeaponDef {
    const char*  name;
    AttackStyle  style;

    DamageType   damageType;
    f32          baseDamage;
    f32          reach;            // метры (для melee — длина конуса)
    f32          coneAngle;        // радианы, полный угол конуса
    f32          projectileSpeed;  // для ranged/magic (м/с)
    f32          projectileGravity;// для ranged (м/с²), 0 у магии
    f32          manaCost;
    f32          staminaCost;
    f32          critChance;       // 0..1
    f32          critMult;
    f32          knockback;        // м/с, отбрасывание

    f32          windupTime;       // сек до активной фазы
    f32          recoveryTime;     // сек после активной фазы
};

/// Реестр. Singleton, создаётся при первом обращении.
enum WeaponId : u16 {
    WEAPON_NONE = 0,
    WEAPON_IRON_SWORD,
    WEAPON_IRON_AXE,
    WEAPON_IRON_SPEAR,
    WEAPON_IRON_DAGGER,
    WEAPON_HUNTING_BOW,
    WEAPON_HEAVY_CROSSBOW,
    WEAPON_THROWING_KNIFE,
    WEAPON_FIRE_STAFF,
    WEAPON_FROST_WAND,
    WEAPON_ARCANE_BRACELET,
    WEAPON_COUNT,
    WEAPON_MAX_DEFS = 64
};

class WeaponRegistry {
public:
    static const WeaponRegistry& instance();
    const WeaponDef& get(u16 id) const;

private:
    WeaponRegistry();
    WeaponDef defs_[WEAPON_MAX_DEFS];
};

inline const WeaponRegistry& weapons() { return WeaponRegistry::instance(); }

} // namespace combat
