#pragma once
#include "../core/types.h"

namespace factions {

// ============================================================
// Фракции мира. Репутация накапливается за квесты, убийства
// врагов фракции и торговлю. Враждебность и дружелюбие NPC
// зависит от отношения игрока к их фракции.
// ============================================================
enum class FactionId : u8 {
    None = 0,
    Villagers,      // мирные жители, стражи, кузнецы
    Traders,        // гильдия торговцев
    Mages,          // академия магии
    Bandits,        // разбойники, враги всех
    Wildlings,      // дикие существа
    Count
};

// ============================================================
// Уровни отношений. Определяют поведение NPC и доступность
// контента.
// ============================================================
enum class ReputationTier : i8 {
    Hated     = -3,   // <= -1000: NPC атакуют
    Hostile   = -2,   // -999..-500: отказ в диалоге
    Unfriendly= -1,   // -499..-100: недоверие, плохие цены
    Neutral   =  0,   // -99..99: базовое отношение
    Friendly  =  1,   // 100..499: скидки, доп. квесты
    Honored   =  2,   // 500..999: эксклюзивные товары
    Exalted   =  3,   // >= 1000: легендарные предложения
};

const char* factionName(FactionId id);
const char* tierName(ReputationTier t);

// ============================================================
// Состояние репутации — компонент ECS, один на игрока.
// ============================================================
struct Reputation {
    // Индексируется по (u8)FactionId
    i32 values[(u8)FactionId::Count] = {};

    i32 get(FactionId id) const {
        u8 i = (u8)id;
        if (i >= (u8)FactionId::Count) return 0;
        return values[i];
    }

    // Возвращает true, если тир изменился — для уведомления UI.
    bool add(FactionId id, i32 amount);

    ReputationTier tier(FactionId id) const {
        return tierFromValue(get(id));
    }

    static ReputationTier tierFromValue(i32 v);
};

// ============================================================
// Пороги (используются в add() и в UI)
// ============================================================
constexpr i32 REP_HATED_MAX     = -1000;
constexpr i32 REP_HOSTILE_MAX   = -500;
constexpr i32 REP_UNFRIENDLY_MAX= -100;
constexpr i32 REP_NEUTRAL_MAX   = 99;
constexpr i32 REP_FRIENDLY_MAX  = 499;
constexpr i32 REP_HONORED_MAX   = 999;

// ============================================================
// Модификаторы отношения. Используются торговлей (Phase 12)
// и генерацией квестов.
// ============================================================
struct RelationModifiers {
    f32 priceMult;      // множитель цены у торговца
    f32 rewardMult;     // множитель награды за квест
    bool hostile;       // атакуют ли NPC игрока
    bool talksToPlayer; // вступают ли в диалог
};

RelationModifiers modifiersFor(ReputationTier t);

} // namespace factions
