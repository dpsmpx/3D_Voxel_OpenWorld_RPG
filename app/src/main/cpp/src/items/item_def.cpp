/**
 * @file item_def.cpp
 * @brief Предметы: определения, инвентарь, лут, подбор, использование.
 */
#include "item_def.h"
#include "../config/localization.h"
#include "../core/log.h"
#include <cstring>

namespace items {

namespace {
constexpr u32 rarityColorValue(ItemRarity r) {
    switch (r) {
        case ItemRarity::Common:    return 0xCCCCCCFF;
        case ItemRarity::Uncommon:  return 0x60D060FF;
        case ItemRarity::Rare:      return 0x4090FFFF;
        case ItemRarity::Epic:      return 0xB060FFFF;
        case ItemRarity::Legendary: return 0xFFB040FF;
        default:                    return 0xFFFFFFFF;
    }
}
}

ItemRegistry::ItemRegistry() {
    auto reg = [&](u16 id, const ItemDef& def) {
        if (id >= ITEM_MAX_DEFS) return;
        defs_[id] = def;
    };

    auto blockItem = [&](u16 id, u16 blockId, const char* name,
                         u16 maxStack, u32 value, ItemRarity rarity)
    {
        ItemDef d{};
        d.name = name;
        d.description = "";
        d.category = ItemCategory::Block;
        d.rarity = rarity;
        d.maxStack = maxStack;
        d.value = value;
        d.payload.blockId = blockId;
        d.iconTile = (u8)blockId;
        d.iconIsBlock = true;
        reg(id, d);
        if (blockId < world::BLOCK_COUNT) blockToItem_[blockId] = id;
    };

    auto weaponItem = [&](u16 id, u16 weaponId, const char* name,
                          u32 value, ItemRarity rarity)
    {
        ItemDef d{};
        d.name = name;
        d.description = "";
        d.category = ItemCategory::Weapon;
        d.rarity = rarity;
        d.maxStack = 1;
        d.value = value;
        d.payload.weaponId = weaponId;
        d.iconTile = 0;
        d.iconIsBlock = false;
        reg(id, d);
    };

    auto materialItem = [&](u16 id, const char* name, u32 value,
                            ItemRarity rarity, u8 iconTile)
    {
        ItemDef d{};
        d.name = name;
        d.description = "";
        d.category = ItemCategory::Material;
        d.rarity = rarity;
        d.maxStack = 64;
        d.value = value;
        d.iconTile = iconTile;
        d.iconIsBlock = false;
        reg(id, d);
    };

    auto potionItem = [&](u16 id, const char* name, u32 value,
                          f32 hp, f32 mp, f32 sp, f32 dur,
                          ItemRarity rarity, u8 iconTile)
    {
        ItemDef d{};
        d.name = name;
        d.description = "";
        d.category = ItemCategory::Potion;
        d.rarity = rarity;
        d.maxStack = 8;
        d.value = value;
        d.restoreHealth  = hp;
        d.restoreMana    = mp;
        d.restoreStamina = sp;
        d.effectDuration = dur;
        d.iconTile = iconTile;
        d.iconIsBlock = false;
        reg(id, d);
    };

    // Эликсир: не восстанавливает ничего, а поднимает атрибут на
    // время. Отдельная фабрика, а не седьмой параметр к potionItem:
    // общего у них только категория.
    auto elixirItem = [&](u16 id, const char* name, u32 value,
                          BuffAttr attr, i32 amount, f32 dur,
                          ItemRarity rarity, u8 iconTile)
    {
        ItemDef d{};
        d.name = name;
        d.description = "";
        d.category = ItemCategory::Potion;
        d.rarity = rarity;
        d.maxStack = 8;
        d.value = value;
        d.buffAttr       = attr;
        d.buffAmount     = amount;
        d.effectDuration = dur;
        d.iconTile = iconTile;
        d.iconIsBlock = false;
        reg(id, d);
    };

    auto throwableItem = [&](u16 id, const char* name, u32 value,
                             ThrowKind kind, u16 stack,
                             ItemRarity rarity, u8 iconTile)
    {
        ItemDef d{};
        d.name = name;
        d.description = "";
        d.category = ItemCategory::Throwable;
        d.rarity = rarity;
        d.maxStack = stack;
        d.value = value;
        d.throwKind = kind;
        d.iconTile = iconTile;
        d.iconIsBlock = false;
        reg(id, d);
    };

    auto foodItem = [&](u16 id, const char* name, u32 value,
                        f32 hp, f32 sp, ItemRarity rarity, u8 iconTile)
    {
        ItemDef d{};
        d.name = name;
        d.description = "";
        d.category = ItemCategory::Food;
        d.rarity = rarity;
        d.maxStack = 16;
        d.value = value;
        d.restoreHealth  = hp;
        d.restoreStamina = sp;
        d.iconTile = iconTile;
        d.iconIsBlock = false;
        reg(id, d);
    };

    // ============ Материалы (block items) ============
    blockItem(ITEM_STONE,    world::STONE,    "Stone",     64,  1, ItemRarity::Common);
    blockItem(ITEM_DIRT,     world::DIRT,     "Dirt",      64,  1, ItemRarity::Common);
    blockItem(ITEM_GRASS,    world::GRASS,    "Grass",     64,  1, ItemRarity::Common);
    blockItem(ITEM_SAND,     world::SAND,     "Sand",      64,  1, ItemRarity::Common);
    blockItem(ITEM_WOOD,     world::WOOD,     "Wood",      64,  2, ItemRarity::Common);
    blockItem(ITEM_LEAVES,   world::LEAVES,   "Leaves",    64,  1, ItemRarity::Common);
    blockItem(ITEM_SNOW,     world::SNOW,     "Snow",      64,  1, ItemRarity::Common);
    blockItem(ITEM_ICE,      world::ICE,      "Ice",       64,  2, ItemRarity::Common);
    blockItem(ITEM_IRON_ORE, world::IRON_ORE, "Iron Ore",  64,  8, ItemRarity::Uncommon);
    blockItem(ITEM_GOLD_ORE, world::GOLD_ORE, "Gold Ore",  64, 15, ItemRarity::Rare);
    blockItem(ITEM_CACTUS,   world::CACTUS,   "Cactus",    64,  2, ItemRarity::Common);
    blockItem(ITEM_BRICK,    world::BRICK,    "Brick",     64,  3, ItemRarity::Common);
    // Факел. Стопка большая: в пещеру их берут пачками.
    blockItem(ITEM_TORCH,    world::TORCH,    "Torch",     64,  2, ItemRarity::Common);
    // Природные блоки без своего предмета: щебень рассыпается камнем,
    // сухая трава выкапывается травой.
    blockToItem_[world::GRAVEL]    = ITEM_STONE;
    blockToItem_[world::DRY_GRASS] = ITEM_GRASS;

    // ============ Материалы (не block) ============
    materialItem(ITEM_IRON_INGOT, "Iron Ingot", 25, ItemRarity::Uncommon, 20);
    materialItem(ITEM_GOLD_INGOT, "Gold Ingot", 50, ItemRarity::Rare,     21);
    materialItem(ITEM_LEATHER,    "Leather",     8, ItemRarity::Common,   22);
    materialItem(ITEM_BONE,       "Bone",        5, ItemRarity::Common,   23);
    materialItem(ITEM_CLOTH,      "Cloth",       4, ItemRarity::Common,   24);

    // ============ Метательное ============
    throwableItem(ITEM_TRAMPOLINE, "Trampoline", 35,
                  ThrowKind::Trampoline, 8, ItemRarity::Uncommon, 40);
    // Стопка большая: сюрикен — расходник, и бегать за десятком
    // штук в сумку игрок не должен.
    throwableItem(ITEM_SHURIKEN,   "Shuriken",    6,
                  ThrowKind::Shuriken,  32, ItemRarity::Common,   41);

    // ============ Оружие ============
    weaponItem(ITEM_IRON_SWORD,      combat::WEAPON_IRON_SWORD,      "Iron Sword",        30, ItemRarity::Common);
    weaponItem(ITEM_IRON_AXE,        combat::WEAPON_IRON_AXE,        "Iron Axe",          35, ItemRarity::Common);
    weaponItem(ITEM_IRON_SPEAR,      combat::WEAPON_IRON_SPEAR,      "Iron Spear",        40, ItemRarity::Uncommon);
    weaponItem(ITEM_IRON_DAGGER,     combat::WEAPON_IRON_DAGGER,     "Iron Dagger",       25, ItemRarity::Common);
    weaponItem(ITEM_HUNTING_BOW,     combat::WEAPON_HUNTING_BOW,     "Hunting Bow",       45, ItemRarity::Uncommon);
    weaponItem(ITEM_HEAVY_CROSSBOW,  combat::WEAPON_HEAVY_CROSSBOW,  "Heavy Crossbow",   120, ItemRarity::Rare);
    weaponItem(ITEM_THROWING_KNIFE,  combat::WEAPON_THROWING_KNIFE,  "Throwing Knife",    10, ItemRarity::Common);
    weaponItem(ITEM_FIRE_STAFF,      combat::WEAPON_FIRE_STAFF,      "Fire Staff",       150, ItemRarity::Rare);
    weaponItem(ITEM_FROST_WAND,      combat::WEAPON_FROST_WAND,      "Frost Wand",       150, ItemRarity::Rare);
    weaponItem(ITEM_ARCANE_BRACELET, combat::WEAPON_ARCANE_BRACELET, "Arcane Bracelet",  200, ItemRarity::Epic);

    // ============ Руны заклинаний ============
    //
    // Не оружие в привычном смысле: руну не куют, не покупают и не
    // находят — её кладёт в сумку навык Древа, а бьёт она маной.
    // Надевается как оружие, потому что держат заклинание в той же
    // руке и бьют той же кнопкой.
    auto runeItem = [&](u16 id, u16 weaponId, const char* name,
                        ItemRarity rarity)
    {
        ItemDef d{};
        d.name = name;
        d.description = "";
        d.category = ItemCategory::Weapon;
        d.rarity = rarity;
        d.maxStack = 1;
        d.value = 0;
        d.payload.weaponId = weaponId;
        d.iconTile = 0;
        d.iconIsBlock = false;
        d.bound = true;
        reg(id, d);
    };
    runeItem(ITEM_RUNE_ICE_NEEDLES, combat::WEAPON_ICE_NEEDLES, "Ice Needles Rune",
             ItemRarity::Rare);
    runeItem(ITEM_RUNE_FLAME, combat::WEAPON_FLAME, "Flame Rune",
             ItemRarity::Epic);

    // ============ Зелья ============
    potionItem(ITEM_POTION_HEALTH_SMALL, "Health Potion",        25,  40.f, 0.f,   0.f, 0.f, ItemRarity::Common,   30);
    potionItem(ITEM_POTION_HEALTH_LARGE, "Greater Health Potion",80, 120.f, 0.f,   0.f, 0.f, ItemRarity::Uncommon, 31);
    potionItem(ITEM_POTION_MANA_SMALL,   "Mana Potion",          25,   0.f,40.f,   0.f, 0.f, ItemRarity::Common,   32);
    potionItem(ITEM_POTION_MANA_LARGE,   "Greater Mana Potion",  80,   0.f,120.f,  0.f, 0.f, ItemRarity::Uncommon, 33);
    potionItem(ITEM_POTION_STAMINA,      "Stamina Potion",       20,   0.f,  0.f, 100.f,0.f, ItemRarity::Common,   34);
    elixirItem(ITEM_ELIXIR_STRENGTH,  "Elixir of Strength",  120,
               BuffAttr::Strength,     5, 60.f, ItemRarity::Rare, 35);
    elixirItem(ITEM_ELIXIR_AGILITY,   "Elixir of Agility",   120,
               BuffAttr::Agility,      5, 60.f, ItemRarity::Rare, 36);
    elixirItem(ITEM_ELIXIR_INTELLECT, "Elixir of Intellect", 120,
               BuffAttr::Intelligence, 5, 60.f, ItemRarity::Rare, 37);
    elixirItem(ITEM_ELIXIR_ENDURANCE, "Elixir of Endurance", 120,
               BuffAttr::Endurance,    5, 60.f, ItemRarity::Rare, 38);

    // ============ Еда ============
    foodItem(ITEM_BREAD,       "Bread",        5,  15.f, 10.f, ItemRarity::Common, 40);
    foodItem(ITEM_MEAT_RAW,    "Raw Meat",     4,   5.f,  5.f, ItemRarity::Common, 41);
    foodItem(ITEM_MEAT_COOKED, "Cooked Meat", 10,  25.f, 20.f, ItemRarity::Common, 42);
    foodItem(ITEM_APPLE,       "Apple",        3,   8.f,  5.f, ItemRarity::Common, 43);

    // ============ Валюта ============
    {
        ItemDef d{};
        d.name = "Gold";
        d.description = "";
        d.category = ItemCategory::Currency;
        d.rarity = ItemRarity::Common;
        d.maxStack = 9999;
        d.value = 1;
        d.iconTile = 50;
        d.iconIsBlock = false;
        reg(ITEM_GOLD_COIN, d);
    }

    LOGI("ItemRegistry: %u предметов зарегистрировано", (unsigned)ITEM_COUNT);
}

const ItemRegistry& ItemRegistry::instance() {
    static ItemRegistry r;
    return r;
}

const char* ItemRegistry::name(u16 id) const {
    return config::tr(get(id).name);
}

const ItemDef& ItemRegistry::get(u16 id) const {
    if (id >= ITEM_MAX_DEFS) return defs_[ITEM_NONE];
    return defs_[id];
}

u16 ItemRegistry::blockToItem(u16 blockId) const {
    if (blockId >= world::BLOCK_COUNT) return ITEM_NONE;
    return blockToItem_[blockId];
}

const char* rarityName(ItemRarity r) {
    switch (r) {
        case ItemRarity::Common:    return config::tr("Common");
        case ItemRarity::Uncommon:  return config::tr("Uncommon");
        case ItemRarity::Rare:      return config::tr("Rare");
        case ItemRarity::Epic:      return config::tr("Epic");
        case ItemRarity::Legendary: return config::tr("Legendary");
        default:                    return "?";
    }
}

u32 rarityColor(ItemRarity r) {
    return rarityColorValue(r);
}

} // namespace items
