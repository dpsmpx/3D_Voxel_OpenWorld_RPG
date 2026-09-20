/**
 * @file weapon.h
 * @brief Бой: урон, оружие, зачарования, система «Резонанс», статусы.
 */
#pragma once
#include "../core/types.h"
#include "../entity/locomotion.h"
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

    /// Каким движением бьют. Отличает оружие НА ВЗГЛЯД: по дуге
    /// видно, что в руках, без чтения полоски в углу.
    ///
    /// Поле, а не ветка по WeaponId: одиннадцатое оружие не должно
    /// требовать одиннадцатой ветки в анимации.
    anim::AttackShape shape;

    /// Какую долю удара держит поднятая защита, 0..1.
    ///
    /// Третья грань, по которой оружие отличается от оружия, — после
    /// урона и дуги. Кинжалом отбиваться нечем, и играть им придётся
    /// парированием; топор держит средне; меч — лучше всех
    /// клинков. Отбивает же ЛЮБОЕ оружие одинаково: точность — это
    /// умение игрока, и отнимать его у лёгкого оружия незачем.
    f32 guardFraction;
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
