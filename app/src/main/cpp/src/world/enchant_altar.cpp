/**
 * @file enchant_altar.cpp
 * @brief Мир: чанки, процедурная генерация, биомы, структуры, цикл суток.
 */
#include "enchant_altar.h"
#include "../config/localization.h"
#include "../ecs/components.h"
#include "../items/currency.h"
#include "../items/item_def.h"
#include "../combat/components.h"
#include "../combat/weapon.h"
#include "../core/log.h"
#include <algorithm>
#include <cmath>
#include <vector>
#include <utility>

namespace world {

using namespace ecs;

// ============================================================
// Registry
// ============================================================
EnchantRecipeRegistry::EnchantRecipeRegistry() {
    auto add = [&](combat::EnchantmentId id, u8 lvl, u32 gold,
                   std::vector<items::ItemStack> mats, const char* name)
    {
        EnchantRecipe r{};
        r.enchantId  = id;
        r.level      = lvl;
        r.goldCost   = gold;
        r.materials  = std::move(mats);
        r.name       = name;
        recipes_.push_back(std::move(r));
    };

    auto mat = [](u16 id, u16 c) {
        items::ItemStack s;
        s.itemId = id;
        s.count  = c;
        return s;
    };

    // ---- Fire ----
    add(combat::EnchantmentId::Fire, 1, 100,
        { mat(items::ITEM_IRON_INGOT, 2), mat(items::ITEM_WOOD, 3) },
        "Fire I");
    add(combat::EnchantmentId::Fire, 2, 300,
        { mat(items::ITEM_IRON_INGOT, 5), mat(items::ITEM_GOLD_ORE, 3) },
        "Fire II");
    add(combat::EnchantmentId::Fire, 3, 900,
        { mat(items::ITEM_GOLD_INGOT, 4), mat(items::ITEM_IRON_INGOT, 8) },
        "Fire III");

    // ---- Frost ----
    add(combat::EnchantmentId::Frost, 1, 100,
        { mat(items::ITEM_ICE, 4), mat(items::ITEM_WOOD, 2) },
        "Frost I");
    add(combat::EnchantmentId::Frost, 2, 300,
        { mat(items::ITEM_ICE, 10), mat(items::ITEM_IRON_INGOT, 3) },