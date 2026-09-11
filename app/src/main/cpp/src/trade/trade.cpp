#include "trade.h"
#include "../items/item_def.h"
#include "../core/log.h"
#include <cmath>
#include <random>
#include <algorithm>

namespace trade {

using namespace items;

namespace {
std::mt19937& rng() {
    static std::mt19937 g(0xCAFE);
    return g;
}

u32 urand() { return rng()(); }

i32 irand(i32 lo, i32 hi) {
    if (hi <= lo) return lo;
    return std::uniform_int_distribution<i32>(lo, hi)(rng());
}
}

// ============================================================
// TradeInventory
// ============================================================
void TradeInventory::tick(f32 dt) {
    restockTimer += dt;
    if (restockTimer >= RESTOCK_INTERVAL_SEC) {
        restockTimer = 0.f;
        restock();
    }
}

void TradeInventory::restock() {
    for (auto& e : entries) {
        if (e.stock < e.maxStock) {
            e.stock = e.maxStock;
        }
    }
}

TradeEntry* TradeInventory::find(u16 itemId) {
    for (auto& e : entries) if (e.itemId == itemId) return &e;
    return nullptr;
}

const TradeEntry* TradeInventory::find(u16 itemId) const {
    for (const auto& e : entries) if (e.itemId == itemId) return &e;
    return nullptr;
}

// ============================================================
// Price
// ============================================================
PriceInfo priceFor(u16 itemId, u32 basePrice,
                   factions::ReputationTier tier,
                   bool buyable, bool sellable)
{
    PriceInfo p{};
    (void)itemId;

    auto mods = factions::modifiersFor(tier);

    // Покупка: player → trader. Чем выше репутация, тем дешевле.
    // Формула: base * priceMult.
    if (buyable) {
        f32 price = (f32)basePrice * mods.priceMult;
        if (price < 1.f) price = 1.f;
        p.buyPrice = (u32)price;
    }

    // Продажа: trader → player. Игрок получает меньшую сумму (спред).
    // Формула: base * 0.5 / priceMult (инвертируем: лучшая репутация —
    // лучше цена продажи).
    if (sellable) {
        f32 inv = 1.f;
        if (mods.priceMult > 0.1f) inv = 1.f / mods.priceMult;
        f32 price = (f32)basePrice * 0.5f * inv;
        if (price < 1.f) price = 1.f;
        p.sellPrice = (u32)price;
    }

    return p;
}

const char* statusString(TradeResult r) {
    switch (r) {
        case TradeResult::Ok:              return "OK";
        case TradeResult::NoEntry:         return "Not traded";
        case TradeResult::NotEnoughStock:  return "Out of stock";
        case TradeResult::NotEnoughGold:   return "Not enough gold";
        case TradeResult::NotEnoughItems:  return "Not enough items";
        case TradeResult::InventoryFull:   return "Inventory full";
        case TradeResult::NotAllowed:      return "Not allowed";
    }
    return "?";
}

// ============================================================
// Buy
// ============================================================
TradeResult buy(ecs::Registry& reg,
                ecs::Entity playerEntity,
                ecs::Entity traderEntity,
                u16 itemId, u16 count)
{
    if (count == 0) return TradeResult::Ok;

    auto* inv  = reg.get<Inventory>(playerEntity);
    auto* wal  = reg.get<Wallet>(playerEntity);
    auto* tinv = reg.get<TradeInventory>(traderEntity);
    auto* tWal = reg.get<Wallet>(traderEntity);

    if (!inv || !wal || !tinv) return TradeResult::NoEntry;

    auto* entry = tinv->find(itemId);
    if (!entry || !entry->isBuyable) return TradeResult::NoEntry;
    if (entry->stock < count) return TradeResult::NotEnoughStock;

    // ---- Цена ----
    factions::ReputationTier tier = factions::ReputationTier::Neutral;
    if (auto* rep = reg.get<factions::Reputation>(playerEntity)) {
        // Для простоты берём Villagers — реальная логика требует
        // faction от NPC. Phase 13 уточнит.
        tier = rep->tier(factions::FactionId::Villagers);
    }

    auto price = priceFor(itemId, entry->basePrice, tier,
                          entry->isBuyable, entry->isSellable);
    u64 totalCost = (u64)price.buyPrice * count;

    if (wal->gold < totalCost) return TradeResult::NotEnoughGold;

    // ---- Место в инвентаре ----
    if (!inv->hasSpaceFor(itemId, count)) return TradeResult::InventoryFull;

    // ---- Транзакция ----
    wal->spend(totalCost);
    if (tWal) tWal->receive(totalCost);

    ItemStack s;
    s.itemId = itemId;
    s.count  = count;
    auto addRes = inv->addStack(s);
    if (addRes.leftover > 0) {
        // Возврат денег за leftover
        u64 refund = (u64)price.buyPrice * addRes.leftover;
        wal->receive(refund);
        if (tWal) tWal->spend(refund);
    }

    entry->stock -= (u16)addRes.added;

    LOGI("Trade: куплено %u x itemId=%u за %llu g",
         (unsigned)count, (unsigned)itemId,
         (unsigned long long)totalCost);

    return TradeResult::Ok;
}

// ============================================================
// Sell
// ============================================================
TradeResult sell(ecs::Registry& reg,
                 ecs::Entity playerEntity,
                 ecs::Entity traderEntity,
                 u16 itemId, u16 count)
{
    if (count == 0) return TradeResult::Ok;

    auto* inv  = reg.get<Inventory>(playerEntity);
    auto* wal  = reg.get<Wallet>(playerEntity);
    auto* tinv = reg.get<TradeInventory>(traderEntity);
    auto* tWal = reg.get<Wallet>(traderEntity);

    if (!inv || !wal || !tinv) return TradeResult::NoEntry;

    auto* entry = tinv->find(itemId);
    if (!entry || !entry->isSellable) return TradeResult::NotAllowed;

    if (inv->countOf(itemId) < count) return TradeResult::NotEnoughItems;

    factions::ReputationTier tier = factions::ReputationTier::Neutral;
    if (auto* rep = reg.get<factions::Reputation>(playerEntity)) {
        tier = rep->tier(factions::FactionId::Villagers);
    }

    auto price = priceFor(itemId, entry->basePrice, tier,
                          entry->isBuyable, entry->isSellable);
    u64 totalGain = (u64)price.sellPrice * count;

    if (tWal && tWal->gold < totalGain) {
        return TradeResult::NotEnoughGold;
    }

    // ---- Транзакция ----
    u16 removed = inv->removeItem(itemId, count);
    if (removed < count) {
        // Не смогли удалить всё — вернём в инвентарь то, что удалили
        // (это не должно произойти после проверки, но защищаемся).
        ItemStack back;
        back.itemId = itemId;
        back.count  = removed;
        inv->addStack(back);
        return TradeResult::NotEnoughItems;
    }

    wal->receive(totalGain);
    if (tWal) tWal->spend(totalGain);

    entry->stock += removed;
    if (entry->stock > entry->maxStock) entry->maxStock = entry->stock;

    LOGI("Trade: продано %u x itemId=%u за %llu g",
         (unsigned)count, (unsigned)itemId,
         (unsigned long long)totalGain);

    return TradeResult::Ok;
}

// ============================================================
// Ассортимент для торговца
// ============================================================
void generateTraderInventory(TradeInventory& out, u64 seed) {
    std::mt19937 local((u32)seed);
    auto r = [&](i32 lo, i32 hi) -> i32 {
        if (hi <= lo) return lo;
        return std::uniform_int_distribution<i32>(lo, hi)(local);
    };

    out.entries.clear();
    out.gold = 100 + r(0, 200);

    auto add = [&](u16 itemId, u32 basePrice,
                   u16 stock, u16 maxStock,
                   bool buyable, bool sellable)
    {
        TradeEntry e{};
        e.itemId     = itemId;
        e.basePrice  = basePrice;
        e.stock      = stock;
        e.maxStock   = maxStock;
        e.isBuyable  = buyable;
        e.isSellable = sellable;
        out.entries.push_back(e);
    };

    // Базовый ассортимент (детерминированный по seed)
    add(ITEM_BREAD,         5,  (u16)r(3, 8),   8,  true,  true);
    add(ITEM_APPLE,         3,  (u16)r(5, 12), 12,  true,  true);
    add(ITEM_MEAT_RAW,      4,  (u16)r(2, 6),   6,  true,  true);
    add(ITEM_LEATHER,       8,  (u16)r(2, 5),   5,  true,  true);
    add(ITEM_IRON_INGOT,   30,  (u16)r(2, 5),   5,  true,  true);

    // Случайный бонусный предмет
    u32 roll = (u32)r(0, 100);
    if (roll < 30) {
        add(ITEM_IRON_SWORD, 45, 1, 1, true, false);
    } else if (roll < 55) {
        add(ITEM_HUNTING_BOW, 65, 1, 1, true, false);
    } else if (roll < 75) {
        add(ITEM_POTION_HEALTH_SMALL, 35, 3, 3, true, false);
    } else if (roll < 90) {
        add(ITEM_POTION_MANA_SMALL, 35, 3, 3, true, false);
    } else {
        add(ITEM_GOLD_INGOT, 60, 2, 2, true, true);
    }

    LOGI("Trader inventory: %zu позиций, gold=%llu",
         out.entries.size(), (unsigned long long)out.gold);
}

} // namespace trade
