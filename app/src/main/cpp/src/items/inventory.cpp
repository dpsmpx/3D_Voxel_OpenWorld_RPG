/**
 * @file inventory.cpp
 * @brief Предметы: определения, инвентарь, лут, подбор, использование.
 */
#include "inventory.h"
#include "../core/log.h"
#include <algorithm>
#include <cstring>

namespace items {

// ============================================================
// addStack — основная логика стакирования.
//
// Алгоритм:
//   1. Пройти по существующим стекам с тем же itemId и дозалить.
//   2. Если осталось — в пустые слоты (сначала хотбар).
// ============================================================
AddResult Inventory::addStack(const ItemStack& in) {
    AddResult res{};
    if (in.empty()) return res;

    u16 remaining = in.count;

    // Шаг 1 — существующие стеки.
    // Порядок: сначала хотбар, потом main, чтобы игрок видел свежие
    // предметы в хотбаре.
    auto tryMerge = [&](u32 idx) {
        if (remaining == 0) return;
        auto& s = slots[idx];
        if (s.empty()) return;
        if (s.itemId != in.itemId) return;
        if (!s.canStackWith(in)) return;
        u16 canAdd = s.accept(remaining);
        s.count += canAdd;
        remaining -= canAdd;
        res.added += canAdd;
    };

    for (u32 i = 0; i < INV_HOTBAR_SLOTS && remaining; ++i)
        tryMerge(INV_HOTBAR_OFFSET + i);
    for (u32 i = 0; i < INV_MAIN_SLOTS && remaining; ++i)
        tryMerge(INV_MAIN_OFFSET + i);

    // Шаг 2 — пустые слоты. Хотбар приоритетнее.
    auto tryFillEmpty = [&](u32 idx) {
        if (remaining == 0) return;
        auto& s = slots[idx];
        if (!s.empty()) return;
        u16 maxS = items().maxStack(in.itemId);
        if (maxS == 0) maxS = 1;
        u16 put = (remaining < maxS) ? remaining : maxS;
        s.itemId = in.itemId;
        s.count  = put;
        s.enchant = in.enchant;
        remaining -= put;
        res.added += put;
    };

    for (u32 i = 0; i < INV_HOTBAR_SLOTS && remaining; ++i)
        tryFillEmpty(INV_HOTBAR_OFFSET + i);
    for (u32 i = 0; i < INV_MAIN_SLOTS && remaining; ++i)
        tryFillEmpty(INV_MAIN_OFFSET + i);

    res.leftover = remaining;
    return res;
}

AddResult Inventory::addItem(u16 itemId, u16 count) {
    ItemStack s;
    s.itemId = itemId;
    s.count  = count;
    return addStack(s);
}

// ============================================================
// removeItem — удалить N штук.
// Итерируем по хотбару + main. Не трогаем экипировку.
// ============================================================
u16 Inventory::removeItem(u16 itemId, u16 count) {
    u16 removed = 0;

    auto tryRemove = [&](u32 idx) {
        if (removed >= count) return;
        auto& s = slots[idx];
        if (s.empty() || s.itemId != itemId) return;
        u16 need = count - removed;
        u16 take = (s.count < need) ? s.count : need;
        s.count -= take;
        removed += take;
        if (s.count == 0) s.clear();
    };

    for (u32 i = 0; i < INV_HOTBAR_SLOTS && removed < count; ++i)
        tryRemove(INV_HOTBAR_OFFSET + i);
    for (u32 i = 0; i < INV_MAIN_SLOTS && removed < count; ++i)
        tryRemove(INV_MAIN_OFFSET + i);

    return removed;
}

u16 Inventory::removeFromSlot(u32 slotIndex, u16 count) {
    if (slotIndex >= INV_TOTAL_SLOTS) return 0;
    auto& s = slots[slotIndex];
    if (s.empty()) return 0;
    u16 take = (s.count < count) ? s.count : count;
    s.count -= take;
    if (s.count == 0) s.clear();
    return take;
}

void Inventory::swapSlots(u32 a, u32 b) {
    if (a >= INV_TOTAL_SLOTS || b >= INV_TOTAL_SLOTS) return;
    if (a == b) return;
    std::swap(slots[a], slots[b]);
}

bool Inventory::splitStack(u32 slotIndex) {
    if (slotIndex >= INV_TOTAL_SLOTS) return false;
    auto& s = slots[slotIndex];
    if (s.empty() || s.count < 2) return false;

    i32 emptyIdx = findEmpty();
    if (emptyIdx < 0) return false;

    u16 half = s.count / 2;
    auto& dst = slots[emptyIdx];
    dst.itemId = s.itemId;
    dst.count  = half;
    dst.enchant = s.enchant;

    s.count -= half;
    return true;
}

ItemStack Inventory::takeStack(u32 slotIndex) {
    ItemStack out;
    if (slotIndex >= INV_TOTAL_SLOTS) return out;
    out = slots[slotIndex];
    slots[slotIndex].clear();
    return out;
}

AddResult Inventory::putStack(u32 slotIndex, const ItemStack& in) {
    AddResult res{};
    if (slotIndex >= INV_TOTAL_SLOTS) {
        res.leftover = in.count;
        return res;
    }
    if (in.empty()) return res;

    auto& s = slots[slotIndex];

    // Пустой слот — просто ставим.
    if (s.empty()) {
        u16 maxS = items().maxStack(in.itemId);
        if (maxS == 0) maxS = 1;
        u16 put = (in.count < maxS) ? in.count : maxS;
        s.itemId = in.itemId;
        s.count  = put;
        s.enchant = in.enchant;
        res.added = put;
        res.leftover = in.count - put;
        return res;
    }

    // Занятый слот — только если совместим.
    if (!s.canStackWith(in)) {
        res.leftover = in.count;
        return res;
    }
    u16 canAdd = s.accept(in.count);
    s.count += canAdd;
    res.added = canAdd;
    res.leftover = in.count - canAdd;
    return res;
}

u32 Inventory::countOf(u16 itemId) const {
    u32 total = 0;
    for (u32 i = 0; i < INV_TOTAL_SLOTS; ++i) {
        const auto& s = slots[i];
        if (!s.empty() && s.itemId == itemId) total += s.count;
    }
    return total;
}

bool Inventory::has(u16 itemId, u16 amount) const {
    return countOf(itemId) >= (u32)amount;
}

bool Inventory::hasSpaceFor(u16 itemId, u16 amount) const {
    u16 maxS = items().maxStack(itemId);
    if (maxS == 0) maxS = 1;

    u32 free = 0;
    for (u32 i = 0; i < INV_TOTAL_SLOTS; ++i) {
        const auto& s = slots[i];
        if (s.empty()) { free += maxS; continue; }
        if (s.itemId == itemId && s.enchant.id == combat::EnchantmentId::None) {
            free += s.space();
        }
    }
    return free >= (u32)amount;
}

i32 Inventory::findItem(u16 itemId) const {
    for (u32 i = 0; i < INV_TOTAL_SLOTS; ++i) {
        const auto& s = slots[i];
        if (!s.empty() && s.itemId == itemId) return (i32)i;
    }
    return -1;
}

i32 Inventory::findEmpty() const {
    // Сначала хотбар, потом main.
    for (u32 i = 0; i < INV_HOTBAR_SLOTS; ++i) {
        if (slots[INV_HOTBAR_OFFSET + i].empty()) return (i32)(INV_HOTBAR_OFFSET + i);
    }
    for (u32 i = 0; i < INV_MAIN_SLOTS; ++i) {
        if (slots[INV_MAIN_OFFSET + i].empty()) return (i32)(INV_MAIN_OFFSET + i);
    }
    return -1;
}

void Inventory::clearAll() {
    for (auto& s : slots) s.clear();
    activeHotbar = 0;
}

void Inventory::clearHotbar() {
    for (u32 i = 0; i < INV_HOTBAR_SLOTS; ++i) {
        slots[INV_HOTBAR_OFFSET + i].clear();
    }
}

bool Inventory::isEmpty() const {
    for (const auto& s : slots) if (!s.empty()) return false;
    return true;
}

void Inventory::sortMain() {
    // Собираем все непустые стеки из main-части.
    std::vector<ItemStack> stacks;
    stacks.reserve(INV_MAIN_SLOTS);

    for (u32 i = 0; i < INV_MAIN_SLOTS; ++i) {
        auto& s = slots[INV_MAIN_OFFSET + i];
        if (s.empty()) continue;
        stacks.push_back(s);
        s.clear();
    }

    std::sort(stacks.begin(), stacks.end(),
              [](const ItemStack& a, const ItemStack& b) {
                  const auto& da = items().get(a.itemId);
                  const auto& db = items().get(b.itemId);
                  if (da.category != db.category)
                      return (u8)da.category < (u8)db.category;
                  if (da.rarity != db.rarity)
                      return (u8)da.rarity > (u8)db.rarity;  // редкое — выше
                  return a.itemId < b.itemId;
              });

    // Кладём обратно, стакируя одинаковые.
    u32 write = 0;
    ItemStack carry;
    for (const auto& s : stacks) {
        if (carry.empty()) { carry = s; continue; }
        if (carry.itemId == s.itemId &&
            carry.enchant.id == combat::EnchantmentId::None)
        {
            u16 maxS = items().maxStack(carry.itemId);
            u16 need = maxS - carry.count;
            u16 take = (s.count < need) ? s.count : need;
            carry.count += take;
            u16 rem = s.count - take;
            if (rem > 0) {
                if (write < INV_MAIN_SLOTS) {
                    slots[INV_MAIN_OFFSET + write] = carry;
                    ++write;
                }
                carry = s;
                carry.count = rem;
            }
        } else {
            if (write < INV_MAIN_SLOTS) {
                slots[INV_MAIN_OFFSET + write] = carry;
                ++write;
            }
            carry = s;
        }
    }
    if (!carry.empty() && write < INV_MAIN_SLOTS) {
        slots[INV_MAIN_OFFSET + write] = carry;
        ++write;
    }
}

u32 Inventory::totalItemCount() const {
    u32 n = 0;
    for (const auto& s : slots) n += s.count;
    return n;
}

u32 Inventory::usedSlotCount() const {
    u32 n = 0;
    for (const auto& s : slots) if (!s.empty()) ++n;
    return n;
}

u32 Inventory::totalInventoryValue() const {
    u32 n = 0;
    for (const auto& s : slots) {
        if (!s.empty()) n += s.totalValue();
    }
    return n;
}

} // namespace items
