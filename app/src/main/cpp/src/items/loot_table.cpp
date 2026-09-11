/**
 * @file loot_table.cpp
 * @brief Предметы: определения, инвентарь, лут, подбор, использование.
 */
#include "loot_table.h"
#include "item_def.h"
#include "../mobs/mob_def.h"
#include "../core/log.h"
#include <random>

namespace items {

namespace {
std::mt19937& rng() {
    static std::mt19937 g(0xFACE);
    return g;
}

f32 frand(f32 lo, f32 hi) {
    return std::uniform_real_distribution<f32>(lo, hi)(rng());
}

i32 irand(i32 lo, i32 hi) {
    if (hi <= lo) return lo;
    return std::uniform_int_distribution<i32>(lo, hi)(rng());
}
}

void LootTable::roll(std::vector<ItemStack>& out, f32 chanceModifier) const {
    for (const auto& e : entries) {
        if (e.itemId == 0) continue;
        f32 c = e.chance * chanceModifier;
        if (c > 1.f) c = 1.f;
        if (frand(0.f, 1.f) > c) continue;

        i32 n = irand((i32)e.minCount, (i32)e.maxCount);
        if (n <= 0) continue;

        ItemStack s;
        s.itemId = e.itemId;
        s.count  = (u16)n;
        out.push_back(s);
    }
}

LootRegistry::LootRegistry() {
    using namespace mobs;

    // ---- Sheep ----
    {
        LootTable t;
        t.entries.push_back({ ITEM_LEATHER, 1, 2, 0.9f, 0.f });
        t.entries.push_back({ ITEM_MEAT_RAW, 1, 2, 0.7f, 0.f });
        tables_[MOB_SHEEP] = t;
    }

    // ---- Cow ----
    {
        LootTable t;
        t.entries.push_back({ ITEM_LEATHER, 1, 3, 1.0f, 0.f });
        t.entries.push_back({ ITEM_MEAT_RAW, 2, 3, 0.9f, 0.f });
        tables_[MOB_COW] = t;
    }

    // ---- Chicken ----
    {
        LootTable t;
        t.entries.push_back({ ITEM_MEAT_RAW, 1, 1, 0.8f, 0.f });
        t.entries.push_back({ ITEM_BONE,    1, 1, 0.4f, 0.f });
        tables_[MOB_CHICKEN] = t;
    }

    // ---- Wolf ----
    {
        LootTable t;
        t.entries.push_back({ ITEM_LEATHER, 1, 2, 0.8f, 0.f });
        t.entries.push_back({ ITEM_BONE,    1, 2, 0.6f, 0.f });
        t.entries.push_back({ ITEM_MEAT_RAW, 1, 2, 0.5f, 0.f });
        tables_[MOB_WOLF] = t;
    }

    // ---- Skeleton ----
    {
        LootTable t;
        t.entries.push_back({ ITEM_BONE,     1, 3, 1.0f, 0.f });
        t.entries.push_back({ ITEM_IRON_ORE, 1, 1, 0.3f, 0.f });
        t.entries.push_back({ ITEM_IRON_INGOT, 1, 1, 0.15f, 0.2f });
        tables_[MOB_SKELETON] = t;
    }

    // ---- Goblin ----
    {
        LootTable t;
        t.entries.push_back({ ITEM_LEATHER,    1, 2, 0.7f, 0.f });
        t.entries.push_back({ ITEM_IRON_ORE,   1, 2, 0.4f, 0.f });
        t.entries.push_back({ ITEM_GOLD_ORE,   1, 1, 0.15f, 0.f });
        t.entries.push_back({ ITEM_IRON_DAGGER,1, 1, 0.08f, 0.3f });
        tables_[MOB_GOBLIN] = t;
    }

    // ---- Slime ----
    {
        LootTable t;
        t.entries.push_back({ ITEM_CLOTH, 1, 2, 0.9f, 0.f });
        t.entries.push_back({ ITEM_LEAVES,1, 1, 0.3f, 0.f });
        tables_[MOB_SLIME] = t;
    }

    LOGI("LootRegistry: %u таблиц", 8);
}

const LootRegistry& LootRegistry::instance() {
    static LootRegistry r;
    return r;
}

const LootTable& LootRegistry::get(u16 mobId) const {
    if (mobId >= 256) return tables_[0];
    return tables_[mobId];
}

} // namespace items
