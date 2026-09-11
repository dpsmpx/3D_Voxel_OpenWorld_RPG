/**
 * @file enchant_altar.h
 * @brief Мир: чанки, процедурная генерация, биомы, структуры, цикл суток.
 */
#pragma once
#include "../core/types.h"
#include "../ecs/registry.h"
#include "../items/item_stack.h"
#include "../items/inventory.h"
#include "../combat/enchantment.h"
#include "chunk_manager.h"
#include <glm/glm.hpp>
#include <vector>

namespace world {

/// Алтарь зачарования. Размещается в деревнях как часть
/// структуры (аналог кузнеца). Игрок приносит оружие, ресурсы
/// и золото, и алтарь накладывает зачарование.
struct EnchantAltar {
    f32 interactRadius = 3.0f;
    u32 colorRGBA      = 0x9040E0FF;
    f32 sizeX = 0.9f;
    f32 sizeY = 1.2f;
    f32 sizeZ = 0.9f;
};

/// Рецепт зачарования. Применяется к экипированному оружию.
struct EnchantRecipe {
    combat::EnchantmentId enchantId;
    u8                    level;        // 1..3
    u32                   goldCost;
    std::vector<items::ItemStack> materials;

    const char* name;
};

/// Реестр рецептов зачарования. Singleton.
class EnchantRecipeRegistry {
public:
    static const EnchantRecipeRegistry& instance();

    const EnchantRecipe* get(u16 id) const;
    usize count() const { return recipes_.size(); }

    /// Все рецепты, применимые к текущему оружию (не None).
    const std::vector<EnchantRecipe>& all() const { return recipes_; }

private:
    EnchantRecipeRegistry();
    std::vector<EnchantRecipe> recipes_;
};

inline const EnchantRecipeRegistry& enchantRecipes() {
    return EnchantRecipeRegistry::instance();
}

/// Результат применения зачарования.
enum class EnchantResult : u8 {
    Ok = 0,
    NoWeapon,
    AlreadyThis,
    MissingMaterials,
    NotEnoughGold,
    UnknownRecipe,
    LowerLevel,
};

/// Сообщение о результате зачарования — для строки статуса.
const char* enchantResultString(EnchantResult r);

/// Применить зачарование к экипированному оружию.
///   - Списывает материалы из инвентаря игрока
///   - Списывает золото из кошелька игрока
///   - Устанавливает enchantment на equipped weapon
EnchantResult applyEnchantment(ecs::Registry& reg,
                               ecs::Entity playerEntity,
                               u16 recipeId);

/// Спавнер алтарей в деревнях (аналог crafting_station).
class EnchantAltarSpawner {
public:
    void update(ecs::Registry& reg,
                ChunkManager& world,
                const glm::vec3& playerPos,
                u64 worldSeed);

    u32 activeCount() const { return activeCount_; }

private:
    f32 spawnTimer_   = 0.f;
    f32 despawnTimer_ = 0.f;
    u32 activeCount_  = 0;

    static constexpr i32 SUPER_BLOCKS = 256;
    static constexpr f32 SPAWN_DIST   = 180.f;
    static constexpr f32 DESPAWN_DIST = 240.f;
};

} // namespace world
