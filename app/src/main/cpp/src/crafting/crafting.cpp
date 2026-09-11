#include "crafting.h"
#include "../ecs/components.h"
#include "crafting_station.h"
#include "../ecs/registry.h"
#include "../core/log.h"

namespace crafting {

using namespace items;

const char* statusString(CraftStatus s) {
    switch (s) {
        case CraftStatus::Ok:                return "OK";
        case CraftStatus::RecipeNotFound:    return "Recipe not found";
        case CraftStatus::MissingIngredients:return "Missing ingredients";
        case CraftStatus::MissingStation:    return "Requires station";
        case CraftStatus::LevelTooLow:       return "Level too low";
        case CraftStatus::MissingSkill:      return "Requires Craft Master";
        case CraftStatus::InventoryFull:     return "Inventory full";
        case CraftStatus::OutputBlocked:     return "No space for output";
    }
    return "?";
}

CraftStatus canCraft(const CraftContext& ctx, const Recipe& r) {
    if (!ctx.inventory) return CraftStatus::InventoryFull;

    // Уровень
    if (ctx.playerLevel < r.requiredLevel) {
        return CraftStatus::LevelTooLow;
    }

    // Навык
    if (r.requiresCraftMaster) {
        if (!ctx.skillTree) return CraftStatus::MissingSkill;
        if (ctx.skillTree->rank(progression::SkillNodeId::Wis_CraftMaster) < 1) {
            return CraftStatus::MissingSkill;
        }
    }

    // Станция
    if (r.station != StationType::None && ctx.nearbyStation != r.station) {
        return CraftStatus::MissingStation;
    }

    // Ингредиенты
    for (const auto& in : r.inputs) {
        if (ctx.inventory->countOf(in.itemId) < in.count) {
            return CraftStatus::MissingIngredients;
        }
    }

    // Место под output
    if (!ctx.inventory->hasSpaceFor(r.output.itemId, r.output.count)) {
        return CraftStatus::OutputBlocked;
    }

    return CraftStatus::Ok;
}

CraftStatus craft(const CraftContext& ctx, const Recipe& r) {
    CraftStatus st = canCraft(ctx, r);
    if (st != CraftStatus::Ok) return st;

    // ---- Списать ингредиенты ----
    // Сначала проверяем ещё раз (защита от гонки — на всякий случай).
    for (const auto& in : r.inputs) {
        if (ctx.inventory->countOf(in.itemId) < in.count) {
            return CraftStatus::MissingIngredients;
        }
    }
    for (const auto& in : r.inputs) {
        u16 removed = ctx.inventory->removeItem(in.itemId, in.count);
        if (removed < in.count) {
            // Это не должно произойти после проверки — но если да,
            // возвращаем как есть, остаток «теряется». В реальной игре —
            // транзакция. Здесь — простая логика.
            LOGE("Craft: не удалось списать %u x itemId=%u",
                 (unsigned)in.count, (unsigned)in.itemId);
        }
    }

    // ---- Добавить output ----
    auto res = ctx.inventory->addStack(r.output);
    if (res.leftover > 0) {
        // Инвентарь переполнился — output частично потерян.
        // Такое возможно только при race condition. Логируем.
        LOGW("Craft: output частично потерян (%u)", (unsigned)res.leftover);
    }

    LOGI("Craft: %s → itemId=%u x%u",
         r.name ? r.name : "?", (unsigned)r.output.itemId,
         (unsigned)r.output.count);

    return CraftStatus::Ok;
}

void gatherAvailable(const CraftContext& ctx,
                     std::vector<AvailableRecipe>& out)
{
    out.clear();
    const auto& all = recipes().all();
    out.reserve(all.size());

    for (const auto& r : all) {
        AvailableRecipe a;
        a.recipe = &r;
        a.status = canCraft(ctx, r);
        // «unlocked» — открыт ли вообще (станция+уровень+навык), вне
        // зависимости от ингредиентов.
        a.unlocked = (a.status != CraftStatus::LevelTooLow &&
                      a.status != CraftStatus::MissingSkill &&
                      a.status != CraftStatus::MissingStation);
        out.push_back(a);
    }
}

StationType detectNearbyStation(ecs::Registry& reg,
                                const glm::vec3& pos,
                                f32 radius)
{
    auto& pool = reg.pool<CraftingStation>();
    f32 bestD2 = radius * radius;
    StationType best = StationType::None;

    for (usize i = 0; i < pool.size(); ++i) {
        ecs::Entity e = pool.entityAt((u32)i);
        auto* st = pool.get(e);
        auto* tf = reg.get<ecs::Transform>(e);
        if (!st || !tf) continue;

        glm::vec3 d = tf->position - pos;
        d.y = 0.f;
        f32 d2 = glm::dot(d, d);
        if (d2 < bestD2) {
            bestD2 = d2;
            best = st->type;
        }
    }
    return best;
}

} // namespace crafting
