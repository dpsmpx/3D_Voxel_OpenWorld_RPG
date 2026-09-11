/**
 * @file recipe.h
 * @brief Крафт: рецепты, станции, проверка требований.
 */
#pragma once
#include "../core/types.h"
#include "../items/item_stack.h"
#include <vector>

namespace crafting {

/// Тип станции, необходимой для рецепта.
///   None      — крафт доступен в любом месте (базовые вещи).
///   Workbench — верстак (дерево + инструменты).
///   Anvil     — наковальня (металл, оружие).
///   Alchemy   — алхимический стол (зелья).
enum class StationType : u8 {
    None = 0,
    Workbench,
    Anvil,
    Alchemy,
    Count
};

/// Название станции крафта: верстак, наковальня, алхимический стол.
const char* stationName(StationType t);

/// Один ингредиент рецепта: конкретный itemId + количество.
struct RecipeIngredient {
    u16 itemId = 0;
    u16 count  = 1;
};

/// Рецепт: вход → выход.
///
///   - output всегда имеет count = 1..N
///   - станция определяется stationType
///   - requiredLevel — минимальный уровень игрока
///   - unlockNode — узел дерева (Wis_CraftMaster), если требуется
///   - craftTime   — задержка (пока не используется, Phase 13)
struct Recipe {
    u16 id = 0;
    const char* name = nullptr;

    StationType station = StationType::None;
    u16         requiredLevel = 1;
    bool        requiresCraftMaster = false;   // узел Wis_CraftMaster >= 1

    std::vector<RecipeIngredient> inputs;
    items::ItemStack              output;

    f32 craftTime = 0.f;   // зарезервировано
};

/// Реестр рецептов. Строится один раз при старте.
class RecipeRegistry {
public:
    static const RecipeRegistry& instance();

    const Recipe& get(u16 id) const;
    usize count() const { return recipes_.size(); }

    const std::vector<Recipe>& all() const { return recipes_; }

    /// ---- Поиск по выходному предмету ----
    /// Возвращает список рецептов, у которых output.itemId == itemId.
    std::vector<const Recipe*> findByOutput(u16 itemId) const;

    /// ---- Поиск по станции ----
    std::vector<const Recipe*> findByStation(StationType s) const;

private:
    RecipeRegistry();
    std::vector<Recipe> recipes_;
    Recipe dummy_{};
};

inline const RecipeRegistry& recipes() { return RecipeRegistry::instance(); }

} // namespace crafting
