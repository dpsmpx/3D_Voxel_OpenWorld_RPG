/**
 * @file faction.cpp
 * @brief Фракции и репутация.
 */
#include "faction.h"
#include "../config/localization.h"

namespace factions {

const char* factionName(FactionId id) {
    switch (id) {
        case FactionId::None:      return "-";
        case FactionId::Villagers: return config::tr("Villagers");
        case FactionId::Traders:   return config::tr("Traders");
        case FactionId::Mages:     return config::tr("Mages");
        case FactionId::Bandits:   return config::tr("Bandits");
        case FactionId::Wildlings: return config::tr("Wildlings");
        default:                   return "?";
    }
}

const char* tierName(ReputationTier t) {
    switch (t) {
        case ReputationTier::Hated:      return config::tr("Hated");
        case ReputationTier::Hostile:    return config::tr("Hostile");
        case ReputationTier::Unfriendly: return config::tr("Unfriendly");
        case ReputationTier::Neutral:    return config::tr("Neutral");
        case ReputationTier::Friendly:   return config::tr("Friendly");
        case ReputationTier::Honored:    return config::tr("Honored");
        case ReputationTier::Exalted:    return config::tr("Exalted");
        default:                         return "?";
    }
}

ReputationTier Reputation::tierFromValue(i32 v) {
    if (v <= REP_HATED_MAX)      return ReputationTier::Hated;
    if (v <= REP_HOSTILE_MAX)    return ReputationTier::Hostile;
    if (v <= REP_UNFRIENDLY_MAX) return ReputationTier::Unfriendly;
    if (v <= REP_NEUTRAL_MAX)    return ReputationTier::Neutral;
    if (v <= REP_FRIENDLY_MAX)   return ReputationTier::Friendly;
    if (v <= REP_HONORED_MAX)    return ReputationTier::Honored;
    return ReputationTier::Exalted;
}

bool Reputation::add(FactionId id, i32 amount) {
    u8 i = (u8)id;
    if (i >= (u8)FactionId::Count) return false;
    if (amount == 0) return false;

    ReputationTier before = tierFromValue(values[i]);
    values[i] += amount;
    ReputationTier after = tierFromValue(values[i]);
    return before != after;
}

RelationModifiers modifiersFor(ReputationTier t) {
    RelationModifiers m{};
    switch (t) {
        case ReputationTier::Hated:
            m.priceMult      = 2.00f;
            m.rewardMult     = 0.25f;
            m.hostile        = true;
            m.talksToPlayer  = false;
            break;
        case ReputationTier::Hostile:
            m.priceMult      = 1.80f;
            m.rewardMult     = 0.50f;
            m.hostile        = false;
            m.talksToPlayer  = false;
            break;
        case ReputationTier::Unfriendly:
            m.priceMult      = 1.40f;
            m.rewardMult     = 0.75f;
            m.hostile        = false;
            m.talksToPlayer  = true;
            break;
        case ReputationTier::Neutral:
            m.priceMult      = 1.00f;
            m.rewardMult     = 1.00f;
            m.hostile        = false;
            m.talksToPlayer  = true;
            break;
        case ReputationTier::Friendly:
            m.priceMult      = 0.90f;
            m.rewardMult     = 1.15f;
            m.hostile        = false;
            m.talksToPlayer  = true;
            break;
        case ReputationTier::Honored:
            m.priceMult      = 0.80f;
            m.rewardMult     = 1.30f;
            m.hostile        = false;
            m.talksToPlayer  = true;
            break;
        case ReputationTier::Exalted:
            m.priceMult      = 0.70f;
            m.rewardMult     = 1.50f;
            m.hostile        = false;
            m.talksToPlayer  = true;
            break;
    }
    return m;
}

} // namespace factions
