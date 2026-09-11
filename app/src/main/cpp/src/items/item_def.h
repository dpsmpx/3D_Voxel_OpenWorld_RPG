#pragma once
#include "../core/types.h"
#include "../world/block.h"
#include "../combat/weapon.h"
#include "../combat/enchantment.h"

namespace items {

// ============================================================
// Категория предмета. Определяет, как UI его отображает
// и как логика с ним обращается.
// ============================================================
enum class ItemCategory : u8 {
    Block = 0,       // ставится в мир
    Weapon,          // экипируется как оружие
    Tool,            // кирка/топор/лопата (Phase 13)
    Potion,          // расходник
    Material,        // ресурс для крафта
    Food,            // восстанавливает HP/SP
    Key,             // открывает что-то
    Currency,        // золото и подобное
    Count
};

// ============================================================
// Редкость. Определяет цвет иконки в UI и вес в дропе.
// ============================================================
enum class ItemRarity : u8 {
    Common = 0,
    Uncommon,
    Rare,
    Epic,
    Legendary,
    Count
};

// ============================================================
// Одно определение предмета. Неизменяемая структура,
// создаётся один раз при старте.
// ============================================================
struct ItemDef {
    const char*  name;
    const char*  description;
    ItemCategory category;
    ItemRarity   rarity;

    u16  maxStack;         // 1 для оружия/брони, 64 для блоков
    u32  value;            // базовая цена в золоте

    // Ссылка на игровой объект — интерпретируется по категории
    union {
        u16 blockId;       // для Block
        u16 weaponId;      // для Weapon
        u16 effectId;      // для Potion / Food
    } payload{};

    // Атлас-иконка — либо block id (тайл в block atlas),
    // либо специальный тайл (для не-блочных предметов).
    u8  iconTile     = 0;   // 0..255
    bool iconIsBlock = true;  // true → block atlas, false → item atlas (Phase 13)

    // Для Potion / Food — эффекты
    f32 restoreHealth  = 0.f;
    f32 restoreMana    = 0.f;
    f32 restoreStamina = 0.f;
    f32 effectDuration = 0.f;

    // Требования для крафта
    u16 requiredCraftLevel = 0;
};

// ============================================================
// Реестр. Singleton, строится при первом обращении.
// ============================================================
enum ItemId : u16 {
    ITEM_NONE = 0,

    // --- Материалы ---
    ITEM_STONE        = 1,
    ITEM_DIRT         = 2,
    ITEM_GRASS        = 3,
    ITEM_SAND         = 4,
    ITEM_WOOD         = 5,
    ITEM_LEAVES       = 6,
    ITEM_SNOW         = 7,
    ITEM_ICE          = 8,
    ITEM_IRON_ORE     = 9,
    ITEM_GOLD_ORE     = 10,
    ITEM_IRON_INGOT   = 11,
    ITEM_GOLD_INGOT   = 12,
    ITEM_LEATHER      = 13,
    ITEM_BONE         = 14,
    ITEM_CLOTH        = 15,

    // --- Оружие ---
    ITEM_IRON_SWORD     = 50,
    ITEM_IRON_AXE       = 51,
    ITEM_IRON_SPEAR     = 52,
    ITEM_IRON_DAGGER    = 53,
    ITEM_HUNTING_BOW    = 54,
    ITEM_HEAVY_CROSSBOW = 55,
    ITEM_THROWING_KNIFE = 56,
    ITEM_FIRE_STAFF     = 57,
    ITEM_FROST_WAND     = 58,
    ITEM_ARCANE_BRACELET= 59,

    // --- Зелья ---
    ITEM_POTION_HEALTH_SMALL = 100,
    ITEM_POTION_HEALTH_LARGE = 101,
    ITEM_POTION_MANA_SMALL   = 102,
    ITEM_POTION_MANA_LARGE   = 103,
    ITEM_POTION_STAMINA      = 104,
    ITEM_ELIXIR_STRENGTH     = 105,
    ITEM_ELIXIR_AGILITY      = 106,
    ITEM_ELIXIR_INTELLECT    = 107,
    ITEM_ELIXIR_ENDURANCE    = 108,

    // --- Еда ---
    ITEM_BREAD      = 200,
    ITEM_MEAT_RAW   = 201,
    ITEM_MEAT_COOKED= 202,
    ITEM_APPLE      = 203,

    // --- Валюта ---
    ITEM_GOLD_COIN  = 250,

    ITEM_COUNT = 260,
    ITEM_MAX_DEFS = 512
};

class ItemRegistry {
public:
    static const ItemRegistry& instance();
    const ItemDef& get(u16 id) const;

    // ---- Быстрые проверки ----
    bool   isBlock(u16 id)    const { return get(id).category == ItemCategory::Block; }
    bool   isWeapon(u16 id)   const { return get(id).category == ItemCategory::Weapon; }
    bool   isPotion(u16 id)   const { return get(id).category == ItemCategory::Potion; }
    bool   isFood(u16 id)     const { return get(id).category == ItemCategory::Food; }
    bool   isMaterial(u16 id) const { return get(id).category == ItemCategory::Material; }

    u16    maxStack(u16 id) const { return get(id).maxStack; }
    u32    value(u16 id)    const { return get(id).value; }
    ItemRarity rarity(u16 id) const { return get(id).rarity; }
    const char* name(u16 id) const { return get(id).name; }

    // ---- Обратные преобразования ----
    // Для block id → item id.
    u16 blockToItem(u16 blockId) const;

private:
    ItemRegistry();
    ItemDef defs_[ITEM_MAX_DEFS];

    // Таблица block → item (перестраивается при создании).
    u16 blockToItem_[world::BLOCK_COUNT] = {};
};

inline const ItemRegistry& items() { return ItemRegistry::instance(); }

// ============================================================
// Утилиты UI
// ============================================================
const char* rarityName(ItemRarity r);
u32         rarityColor(ItemRarity r);   // RGBA packed

} // namespace items