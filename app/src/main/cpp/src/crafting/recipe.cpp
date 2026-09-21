/**
 * @file recipe.cpp
 * @brief Крафт: рецепты, станции, проверка требований.
 */
#include "recipe.h"
#include "../config/localization.h"
#include "../core/log.h"
#include "../items/item_def.h"

#include <utility>
#include <vector>

namespace crafting {

using namespace items;

const char* stationName(StationType t) {
    switch (t) {
        case StationType::None:      return "-";
        case StationType::Workbench: return config::tr("Workbench");
        case StationType::Anvil:     return config::tr("Anvil");
        case StationType::Alchemy:   return config::tr("Alchemy Table");
        default:                     return "?";
    }
}

RecipeRegistry::RecipeRegistry() {
    recipes_.reserve(64);

    auto add = [&](const char* name, StationType st,
                   u16 level, bool needsMaster,
                   std::vector<RecipeIngredient> inputs,
                   u16 outItem, u16 outCount)
    {
        Recipe r;
        r.id = (u16)recipes_.size() + 1;
        r.name = name;
        r.station = st;
        r.requiredLevel = level;
        r.requiresCraftMaster = needsMaster;
        r.inputs = std::move(inputs);
        r.output.itemId = outItem;
        r.output.count  = outCount;
        r.craftTime = 0.6f;
        recipes_.push_back(std::move(r));
    };

    // ============ None (крафт на ходу) ============
    //
    // Раздел был пуст, и StationType::None не значил ничего: весь
    // крафт до единого рецепта требовал станции, а станции стоят
    // только в деревнях. Игрок в поле не мог ни поджарить мясо, ни
    // намотать бинт, ни сделать сюрикен из имевшегося слитка — хотя
    // canCraft умел пропускать безстаночные рецепты с самого начала.
    //
    // Сюда идёт то, для чего довольно рук и костра: еда и простые
    // вещи. Плавка, оружие и зелья остаются за станками — иначе
    // деревня перестанет быть нужна.

    // Мясо на костре. Первое, чего хочется в поле, и первое, чего
    // было нельзя.
    add("Cooked Meat", StationType::None, 1, false,
        { { ITEM_MEAT_RAW, 1 }, { ITEM_WOOD, 1 } },
        ITEM_MEAT_COOKED, 1);

    // Лепёшка: мука тут условная, зато печётся на том же костре.
    add("Bread", StationType::None, 1, false,
        { { ITEM_WOOD, 1 }, { ITEM_LEAVES, 1 } },
        ITEM_BREAD, 2);

    // Полосы ткани из кожи. Ткань нужна зельям, а выпадает она редко
    // и не оттуда, откуда кожа.
    add("Cloth Strips", StationType::None, 1, false,
        { { ITEM_LEATHER, 1 } },
        ITEM_CLOTH, 2);

    // Факелы — четвёркой из одного полена. Без крафта на ходу факел
    // бесполезен: темнота застаёт в пещере, а не в деревне, и идти
    // за светом обратно к верстаку значит идти сквозь ту же темноту.
    add("Torches", StationType::None, 1, false,
        { { ITEM_WOOD, 1 } },
        ITEM_TORCH, 4);

    // Сюрикены — четвёркой: поштучно их ковать столько же раз,
    // сколько бросать, и крафт превратился бы в работу. Слиток ещё
    // надо выплавить на верстаке, так что в поле их не наделать из
    // ничего.
    add("Shuriken", StationType::None, 1, false,
        { { ITEM_IRON_INGOT, 1 } },
        ITEM_SHURIKEN, 4);

    // Батут: кожа на полотно, дерево на раму. Кидается под ноги и
    // подбрасывает — им перепрыгивают монстра и разгоняются. Вещь
    // расходная, и бегать за ней в деревню незачем.
    add("Trampoline", StationType::None, 1, false,
        { { ITEM_LEATHER, 2 }, { ITEM_WOOD, 3 } },
        ITEM_TRAMPOLINE, 1);

    // ============ Workbench ============

    // Iron Ingot from Iron Ore + Wood (топливо)
    add("Smelt Iron Ingot", StationType::Workbench, 1, false,
        { { ITEM_IRON_ORE, 2 }, { ITEM_WOOD, 1 } },
        ITEM_IRON_INGOT, 1);

    // Gold Ingot
    add("Smelt Gold Ingot", StationType::Workbench, 3, false,
        { { ITEM_GOLD_ORE, 2 }, { ITEM_WOOD, 1 } },
        ITEM_GOLD_INGOT, 1);

    // Leather обработка — не нужно.

    // Cloth из Wool? У нас нет wool — пропустим.

    // ============ Anvil ============

    // Iron Sword
    add("Iron Sword", StationType::Anvil, 2, false,
        { { ITEM_IRON_INGOT, 3 }, { ITEM_WOOD, 1 } },
        ITEM_IRON_SWORD, 1);

    // Iron Axe
    add("Iron Axe", StationType::Anvil, 2, false,
        { { ITEM_IRON_INGOT, 4 }, { ITEM_WOOD, 2 } },
        ITEM_IRON_AXE, 1);

    // Iron Spear
    add("Iron Spear", StationType::Anvil, 3, false,
        { { ITEM_IRON_INGOT, 4 }, { ITEM_WOOD, 3 } },
        ITEM_IRON_SPEAR, 1);

    // Iron Dagger
    add("Iron Dagger", StationType::Anvil, 2, false,
        { { ITEM_IRON_INGOT, 2 }, { ITEM_WOOD, 1 } },
        ITEM_IRON_DAGGER, 1);

    // Throwing Knife x4
    add("Throwing Knives", StationType::Anvil, 2, false,
        { { ITEM_IRON_INGOT, 1 }, { ITEM_WOOD, 1 } },
        ITEM_THROWING_KNIFE, 4);

    // Hunting Bow
    add("Hunting Bow", StationType::Workbench, 2, false,
        { { ITEM_WOOD, 4 }, { ITEM_LEATHER, 2 } },
        ITEM_HUNTING_BOW, 1);

    // Heavy Crossbow
    add("Heavy Crossbow", StationType::Anvil, 5, false,
        { { ITEM_IRON_INGOT, 6 }, { ITEM_WOOD, 4 }, { ITEM_LEATHER, 2 } },
        ITEM_HEAVY_CROSSBOW, 1);

    // Fire Staff (требует Wis_CraftMaster)
    add("Fire Staff", StationType::Anvil, 6, true,
        { { ITEM_GOLD_INGOT, 3 }, { ITEM_WOOD, 3 }, { ITEM_IRON_INGOT, 2 } },
        ITEM_FIRE_STAFF, 1);

    // Frost Wand
    add("Frost Wand", StationType::Anvil, 6, true,
        { { ITEM_GOLD_INGOT, 3 }, { ITEM_ICE, 5 }, { ITEM_IRON_INGOT, 2 } },
        ITEM_FROST_WAND, 1);

    // Arcane Bracelet
    add("Arcane Bracelet", StationType::Anvil, 8, true,
        { { ITEM_GOLD_INGOT, 5 }, { ITEM_ICE, 4 }, { ITEM_LEATHER, 3 } },
        ITEM_ARCANE_BRACELET, 1);

    // ============ Alchemy ============

    // Health Potion (small)
    add("Health Potion", StationType::Alchemy, 1, false,
        { { ITEM_MEAT_RAW, 2 }, { ITEM_WOOD, 1 } },
        ITEM_POTION_HEALTH_SMALL, 1);

    // Greater Health Potion
    add("Greater Health Potion", StationType::Alchemy, 4, false,
        { { ITEM_POTION_HEALTH_SMALL, 2 }, { ITEM_GOLD_ORE, 1 } },
        ITEM_POTION_HEALTH_LARGE, 1);

    // Mana Potion
    add("Mana Potion", StationType::Alchemy, 1, false,
        { { ITEM_LEAVES, 3 }, { ITEM_WOOD, 1 } },
        ITEM_POTION_MANA_SMALL, 1);

    // Greater Mana Potion
    add("Greater Mana Potion", StationType::Alchemy, 4, false,
        { { ITEM_POTION_MANA_SMALL, 2 }, { ITEM_GOLD_ORE, 1 } },
        ITEM_POTION_MANA_LARGE, 1);

    // Stamina Potion
    add("Stamina Potion", StationType::Alchemy, 2, false,
        { { ITEM_MEAT_RAW, 1 }, { ITEM_LEAVES, 2 } },
        ITEM_POTION_STAMINA, 1);

    // Elixir of Strength
    add("Elixir of Strength", StationType::Alchemy, 5, false,
        { { ITEM_IRON_INGOT, 1 }, { ITEM_MEAT_RAW, 3 }, { ITEM_BONE, 2 } },
        ITEM_ELIXIR_STRENGTH, 1);

    // Elixir of Agility
    add("Elixir of Agility", StationType::Alchemy, 5, false,
        { { ITEM_LEATHER, 2 }, { ITEM_LEAVES, 3 }, { ITEM_BONE, 2 } },
        ITEM_ELIXIR_AGILITY, 1);

    // Elixir of Intellect
    add("Elixir of Intellect", StationType::Alchemy, 5, false,
        { { ITEM_GOLD_INGOT, 1 }, { ITEM_LEAVES, 4 }, { ITEM_BONE, 2 } },
        ITEM_ELIXIR_INTELLECT, 1);

    // Elixir of Endurance
    add("Elixir of Endurance", StationType::Alchemy, 5, false,
        { { ITEM_IRON_INGOT, 1 }, { ITEM_LEATHER, 2 }, { ITEM_BONE, 3 } },
        ITEM_ELIXIR_ENDURANCE, 1);

    LOGI("RecipeRegistry: %zu рецептов", recipes_.size());
}

const RecipeRegistry& RecipeRegistry::instance() {
    static RecipeRegistry r;
    return r;
}

const Recipe& RecipeRegistry::get(u16 id) const {
    if (id == 0 || id > recipes_.size()) return dummy_;
    return recipes_[id - 1];
}

std::vector<const Recipe*> RecipeRegistry::findByOutput(u16 itemId) const {
    std::vector<const Recipe*> out;
    for (const auto& r : recipes_) {
        if (r.output.itemId == itemId) out.push_back(&r);
    }
    return out;
}

std::vector<const Recipe*> RecipeRegistry::findByStation(StationType s) const {
    std::vector<const Recipe*> out;
    for (const auto& r : recipes_) {
        if (r.station == s) out.push_back(&r);
    }
    return out;
}

} // namespace crafting
