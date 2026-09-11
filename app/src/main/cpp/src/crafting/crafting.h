#pragma once
#include "../core/types.h"
#include "../items/inventory.h"
#include "../progression/skill_tree.h"
#include "recipe.h"
#include <vector>

namespace crafting {

// ============================================================
// Результат попытки крафта.
// ============================================================
enum class CraftStatus : u8 {
    Ok = 0,
    RecipeNotFound,
    MissingIngredients,
    MissingStation,
    LevelTooLow,
    MissingSkill,
    InventoryFull,
    OutputBlocked,   // Нет места в инвентаре под output
};

const char* statusString(CraftStatus s);

// ============================================================
// Контекст крафта: что доступно игроку.
// ============================================================
struct CraftContext {
    items::Inventory*           inventory = nullptr;
    const progression::SkillTree* skillTree = nullptr;
    u32                         playerLevel = 1;
    StationType                 nearbyStation = StationType::None;
};

// ============================================================
// Проверить, можно ли крафтить рецепт в данном контексте.
// ============================================================
CraftStatus canCraft(const CraftContext& ctx, const Recipe& r);

// ============================================================
// Применить крафт: списать ингредиенты, добавить output.
// Возвращает статус. Не выполняет списание, если вернул != Ok.
// ============================================================
CraftStatus craft(const CraftContext& ctx, const Recipe& r);

// ============================================================
// Собрать все рецепты, доступные в данном контексте.
// Полезно для UI-списка.
// ============================================================
struct AvailableRecipe {
    const Recipe* recipe = nullptr;
    CraftStatus   status = CraftStatus::Ok;   // можно ли прямо сейчас
    bool          unlocked = false;           // открыт ли вообще (по уровню/станции)
};

void gatherAvailable(const CraftContext& ctx,
                     std::vector<AvailableRecipe>& out);

// ============================================================
// Вспомогательное: проверить станцию в радиусе.
// Используется в UI, чтобы понять, что рядом есть верстак/наковальня.
// ============================================================
StationType detectNearbyStation(ecs::Registry& reg,
                                const glm::vec3& pos,
                                f32 radius);

} // namespace crafting