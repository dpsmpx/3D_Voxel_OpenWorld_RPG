/**
 * @file enchant_altar.cpp
 * @brief Мир: чанки, процедурная генерация, биомы, структуры, цикл суток.
 */
#include "enchant_altar.h"
#include "../ecs/components.h"
#include "../items/currency.h"
#include "../items/item_def.h"
#include "../combat/components.h"
#include "../combat/weapon.h"
#include "../core/log.h"
#include <cmath>
#include <vector>

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
        "Frost II");
    add(combat::EnchantmentId::Frost, 3, 900,
        { mat(items::ITEM_ICE, 24), mat(items::ITEM_GOLD_INGOT, 3) },
        "Frost III");

    // ---- Shock ----
    add(combat::EnchantmentId::Shock, 1, 120,
        { mat(items::ITEM_IRON_INGOT, 3), mat(items::ITEM_BONE, 2) },
        "Shock I");
    add(combat::EnchantmentId::Shock, 2, 350,
        { mat(items::ITEM_IRON_INGOT, 6), mat(items::ITEM_GOLD_ORE, 2) },
        "Shock II");
    add(combat::EnchantmentId::Shock, 3, 1000,
        { mat(items::ITEM_GOLD_INGOT, 5), mat(items::ITEM_BONE, 10) },
        "Shock III");

    // ---- Poison ----
    add(combat::EnchantmentId::Poison, 1, 80,
        { mat(items::ITEM_LEAVES, 6), mat(items::ITEM_MEAT_RAW, 2) },
        "Poison I");
    add(combat::EnchantmentId::Poison, 2, 240,
        { mat(items::ITEM_LEAVES, 12), mat(items::ITEM_BONE, 4) },
        "Poison II");

    // ---- Sharpness ----
    add(combat::EnchantmentId::Sharpness, 1, 150,
        { mat(items::ITEM_IRON_INGOT, 4) },
        "Sharpness I");
    add(combat::EnchantmentId::Sharpness, 2, 450,
        { mat(items::ITEM_GOLD_INGOT, 2), mat(items::ITEM_IRON_INGOT, 6) },
        "Sharpness II");

    // ---- Swift ----
    add(combat::EnchantmentId::Swift, 1, 150,
        { mat(items::ITEM_LEATHER, 5) },
        "Swift I");
    add(combat::EnchantmentId::Swift, 2, 450,
        { mat(items::ITEM_LEATHER, 10), mat(items::ITEM_GOLD_ORE, 2) },
        "Swift II");

    // ---- Vampiric ----
    add(combat::EnchantmentId::Vampiric, 1, 400,
        { mat(items::ITEM_BONE, 8), mat(items::ITEM_GOLD_INGOT, 2) },
        "Vampiric I");

    LOGI("EnchantRecipeRegistry: %zu рецептов", recipes_.size());
}

const EnchantRecipeRegistry& EnchantRecipeRegistry::instance() {
    static EnchantRecipeRegistry r;
    return r;
}

const EnchantRecipe* EnchantRecipeRegistry::get(u16 id) const {
    if (id >= recipes_.size()) return nullptr;
    return &recipes_[id];
}

const char* enchantResultString(EnchantResult r) {
    switch (r) {
        case EnchantResult::Ok:              return "Enchanted!";
        case EnchantResult::NoWeapon:        return "No weapon equipped";
        case EnchantResult::AlreadyThis:     return "Already enchanted this way";
        case EnchantResult::MissingMaterials:return "Missing materials";
        case EnchantResult::NotEnoughGold:   return "Not enough gold";
        case EnchantResult::UnknownRecipe:   return "Unknown recipe";
        case EnchantResult::LowerLevel:      return "Weaker than current";
    }
    return "?";
}

// ============================================================
// Применение зачарования
// ============================================================
EnchantResult applyEnchantment(ecs::Registry& reg,
                               ecs::Entity playerEntity,
                               u16 recipeId)
{
    const auto* r = enchantRecipes().get(recipeId);
    if (!r) return EnchantResult::UnknownRecipe;

    auto* eq = reg.get<combat::EquippedWeapon>(playerEntity);
    if (!eq || eq->weaponId == 0) return EnchantResult::NoWeapon;

    // Если уже такое же — не тратим ресурсы.
    if (eq->enchant.id == r->enchantId && eq->enchant.level >= r->level) {
        return EnchantResult::AlreadyThis;
    }
    // Не позволяем ухудшать зачарование.
    if (eq->enchant.id == r->enchantId && eq->enchant.level > r->level) {
        return EnchantResult::LowerLevel;
    }

    auto* inv = reg.get<items::Inventory>(playerEntity);
    auto* wal = reg.get<items::Wallet>(playerEntity);
    if (!inv) return EnchantResult::MissingMaterials;

    // Проверка материалов.
    for (const auto& m : r->materials) {
        if (inv->countOf(m.itemId) < m.count) {
            return EnchantResult::MissingMaterials;
        }
    }

    // Проверка золота.
    if (wal && wal->gold < r->goldCost) {
        return EnchantResult::NotEnoughGold;
    }

    // Списание.
    for (const auto& m : r->materials) {
        inv->removeItem(m.itemId, m.count);
    }
    if (wal) wal->spend(r->goldCost);

    // Применение.
    eq->enchant.id    = r->enchantId;
    eq->enchant.level = r->level;

    LOGI("Enchant: применение %s к weaponId=%u",
         r->name ? r->name : "?", (unsigned)eq->weaponId);

    return EnchantResult::Ok;
}

// ============================================================
// Спавнер алтарей
// ============================================================
namespace {

u32 hashXZ(i32 x, i32 z, u64 seed) {
    u64 h = (u64)(u32)x * 0x9E3779B97F4A7C15ULL;
    h ^= (u64)(u32)z * 0xC4CEB9FE1A85EC53ULL;
    h ^= seed;
    h ^= h >> 33; h *= 0xFF51AFD7ED558CCDULL;
    h ^= h >> 33;
    return (u32)h;
}

bool isVillage(i32 sx, i32 sz, u64 worldSeed) {
    u32 h = hashXZ(sx, sz, worldSeed ^ 0x517);
    return (h & 0xFF) < 51;
}

} // namespace

void EnchantAltarSpawner::update(ecs::Registry& reg,
                                 ChunkManager& world,
                                 const glm::vec3& playerPos,
                                 u64 worldSeed)
{
    spawnTimer_ += 1.f / 60.f;
    despawnTimer_ += 1.f / 60.f;

    // ---- Спавн ----
    if (spawnTimer_ >= 0.75f) {
        spawnTimer_ = 0.f;

        const i32 playerSx = (i32)std::floor(playerPos.x / (f32)SUPER_BLOCKS);
        const i32 playerSz = (i32)std::floor(playerPos.z / (f32)SUPER_BLOCKS);

        for (i32 dz = -2; dz <= 2; ++dz) {
            for (i32 dx = -2; dx <= 2; ++dx) {
                const i32 sx = playerSx + dx;
                const i32 sz = playerSz + dz;

                if (!isVillage(sx, sz, worldSeed)) continue;

                // Уже спавнили?
                bool anyExists = false;
                {
                    auto& pool = reg.pool<EnchantAltar>();
                    for (usize i = 0; i < pool.size(); ++i) {
                        ecs::Entity e = pool.entityAt((u32)i);
                        auto* tf = reg.get<Transform>(e);
                        if (!tf) continue;
                        i32 nSx = (i32)std::floor(tf->position.x / (f32)SUPER_BLOCKS);
                        i32 nSz = (i32)std::floor(tf->position.z / (f32)SUPER_BLOCKS);
                        if (nSx == sx && nSz == sz) {
                            anyExists = true;
                            break;
                        }
                    }
                }
                if (anyExists) continue;

                const u32 h = hashXZ(sx, sz, worldSeed ^ 0x517);
                const i32 baseX = sx * SUPER_BLOCKS;
                const i32 baseZ = sz * SUPER_BLOCKS;
                const i32 ox = (i32)((h >> 8) & 0x3F);
                const i32 oz = (i32)((h >> 14) & 0x3F);
                const i32 cx = baseX + ox + 32 + 10;   // смещение от центра
                const i32 cz = baseZ + oz + 32 - 8;
                const i32 cy = world.generator().surfaceHeight(cx, cz);

                glm::vec3 pos{ (f32)cx + 0.5f, (f32)cy + 0.2f, (f32)cz + 0.5f };

                f32 ddx = pos.x - playerPos.x;
                f32 ddz = pos.z - playerPos.z;
                if (ddx * ddx + ddz * ddz > SPAWN_DIST * SPAWN_DIST) continue;

                ecs::Entity e = reg.create();

                Transform tf;
                tf.position = pos;
                reg.add(e, tf);

                EnchantAltar alt{};
                reg.add(e, alt);

                Collider col;
                col.halfExtents = glm::vec3(alt.sizeX * 0.5f,
                                            alt.sizeY * 0.5f,
                                            alt.sizeZ * 0.5f);
                col.isStatic = true;
                reg.add(e, col);

                reg.add(e, Kind{ EntityKind::Structure });

                ++activeCount_;
            }
        }
    }

    // ---- Деспавн ----
    if (despawnTimer_ >= 2.0f) {
        despawnTimer_ = 0.f;

        std::vector<ecs::Entity> toRemove;
        auto& pool = reg.pool<EnchantAltar>();
        for (usize i = 0; i < pool.size(); ++i) {
            ecs::Entity e = pool.entityAt((u32)i);
            auto* tf = reg.get<Transform>(e);
            if (!tf) continue;
            glm::vec3 d = tf->position - playerPos;
            d.y = 0.f;
            if (glm::length(d) > DESPAWN_DIST) toRemove.push_back(e);
        }
        for (auto e : toRemove) {
            reg.destroy(e);
            if (activeCount_ > 0) --activeCount_;
        }
    }
}

} // namespace world
