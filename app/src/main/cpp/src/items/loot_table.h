#pragma once
#include "../core/types.h"
#include "item_stack.h"
#include <vector>

namespace items {

// ============================================================
// Запись в таблице дропа.
//   itemId      — что дропает
//   minCount    — минимальное количество
//   maxCount    — максимальное количество
//   chance      — вероятность 0..1 (1.0 = 100%)
//   rarityBonus — шанс апгрейда редкости
// ============================================================
struct LootEntry {
    u16 itemId    = 0;
    u16 minCount  = 1;
    u16 maxCount  = 1;
    f32 chance    = 1.0f;
    f32 rarityBonus = 0.0f;
};

// ============================================================
// Таблица дропа. Для каждого моба — свой набор записей.
// Также используется для сундуков / структур (Phase 15).
// ============================================================
struct LootTable {
    std::vector<LootEntry> entries;

    // Бросить лут. Возвращает список стеков.
    // chanceModifier позволяет бонусы от редкости/удачи игрока.
    void roll(std::vector<ItemStack>& out,
              f32 chanceModifier = 1.0f) const;
};

// ============================================================
// Реестр лут-таблиц. Привязан к mob id (u16).
// ============================================================
class LootRegistry {
public:
    static const LootRegistry& instance();
    const LootTable& get(u16 mobId) const;

private:
    LootRegistry();
    LootTable tables_[256];
};

inline const LootRegistry& loot() { return LootRegistry::instance(); }

} // namespace items
