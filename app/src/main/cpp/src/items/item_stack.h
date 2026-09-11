#pragma once
#include "../core/types.h"
#include "../combat/enchantment.h"
#include "item_def.h"

namespace items {

// ============================================================
// Стек предметов. Пустой, если itemId == 0 или count == 0.
//
// Для оружия enchantment применяется к единственному экземпляру.
// Для блоков/материалов enchantment не используется.
// ============================================================
struct ItemStack {
    u16 itemId = 0;
    u16 count  = 0;
    combat::Enchantment enchant{};   // только для оружия

    bool empty() const {
        return itemId == 0 || count == 0;
    }

    // Свободное место в стеке.
    u16 space() const {
        if (empty()) return items().maxStack(itemId ? itemId : 1);
        u16 maxS = items().maxStack(itemId);
        return (count >= maxS) ? 0 : (maxS - count);
    }

    bool isFull() const {
        return !empty() && count >= items().maxStack(itemId);
    }

    // Сколько добавится из запрошенного количества. Возвращает
    // сколько реально влезло (не больше amount).
    u16 accept(u16 amount) const {
        if (empty()) {
            // Стек пустой — можем принять до maxStack нового предмета.
            return amount;
        }
        return (space() < amount) ? space() : amount;
    }

    // Очистить.
    void clear() {
        itemId = 0;
        count = 0;
        enchant = combat::Enchantment{};
    }

    // Сравнение «совместимости» для стакирования.
    bool canStackWith(const ItemStack& other) const {
        if (empty() || other.empty()) return true;
        if (itemId != other.itemId) return false;
        // Зачарованное оружие не стакается даже с таким же.
        if (enchant.id != combat::EnchantmentId::None) return false;
        if (other.enchant.id != combat::EnchantmentId::None) return false;
        return true;
    }

    // Стоимость всего стека (для UI).
    u32 totalValue() const {
        return items().value(itemId) * (u32)count;
    }

    const ItemDef& def() const {
        return items().get(itemId);
    }
};

} // namespace items
