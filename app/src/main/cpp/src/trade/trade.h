#pragma once
#include "../core/types.h"
#include "../items/inventory.h"
#include "../items/currency.h"
#include "../factions/faction.h"
#include "../ecs/registry.h"

namespace trade {

// ============================================================
// Запись ассортимента.
//   itemId          — что продаётся/покупается
//   basePrice       — базовая цена в золоте
//   stock           — сколько штук в наличии
//   maxStock        — максимальное (обновление восстанавливает до max)
//   restockInterval — не реализовано, но фиксируется
//   isBuyable       — продаёт ли NPC этот предмет игроку
//   isSellable      — скупает ли NPC этот предмет у игрока
// ============================================================
struct TradeEntry {
    u16 itemId         = 0;
    u32 basePrice      = 1;
    u16 stock          = 0;
    u16 maxStock       = 0;
    bool isBuyable     = true;   // NPC → игрок
    bool isSellable    = true;   // игрок → NPC
};

// ============================================================
// Компонент «ассортимент». Живёт на NPC-торговце.
// ============================================================
struct TradeInventory {
    std::vector<TradeEntry> entries;
    u64 gold = 0;

    // Обновление ассортимента (при открытии диалога, раз в игровой день).
    f32 restockTimer = 0.f;
    static constexpr f32 RESTOCK_INTERVAL_SEC = 300.f;  // 5 минут игровых

    void tick(f32 dt);
    void restock();

    TradeEntry* find(u16 itemId);
    const TradeEntry* find(u16 itemId) const;
};

// ============================================================
// Один запрос покупки/продажи.
// ============================================================
enum class TradeResult : u8 {
    Ok = 0,
    NoEntry,
    NotEnoughStock,
    NotEnoughGold,
    NotEnoughItems,
    InventoryFull,
    NotAllowed,
};

const char* statusString(TradeResult r);

// ============================================================
// Купить: игрок покупает у торговца. Списывает золото игрока,
// добавляет предметы, увеличивает золото торговца, уменьшает stock.
// ============================================================
TradeResult buy(ecs::Registry& reg,
                ecs::Entity playerEntity,
                ecs::Entity traderEntity,
                u16 itemId, u16 count);

// ============================================================
// Продать: игрок продаёт торговцу. Уменьшает золото торговца,
// добавляет предметы в inventory торговца (или удаляет из мира),
// увеличивает золото игрока, увеличивает stock.
// ============================================================
TradeResult sell(ecs::Registry& reg,
                 ecs::Entity playerEntity,
                 ecs::Entity traderEntity,
                 u16 itemId, u16 count);

// ============================================================
// Цена с учётом репутации и типа операции.
//   buyPrice: сколько платит игрок.
//   sellPrice: сколько получает игрок.
// ============================================================
struct PriceInfo {
    u32 buyPrice   = 0;   // за 1 штуку
    u32 sellPrice  = 0;   // за 1 штуку
};

PriceInfo priceFor(u16 itemId, u32 basePrice,
                   factions::ReputationTier tier,
                   bool buyable, bool sellable);

// ============================================================
// Вспомогательное: подготовить ассортимент для NPC-роли.
// Используется в npc_spawner при создании торговца.
// ============================================================
void generateTraderInventory(TradeInventory& out, u64 seed);

} // namespace trade