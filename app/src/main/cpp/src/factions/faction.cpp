#include "faction.h"

namespace factions {

const char* factionName(FactionId id) {
    switch (id) {
        case FactionId::None:      return "-";
        case FactionId::Villagers: return "Villagers";
        case FactionId::Traders:   return "Traders";
        case FactionId::Mages:     return "Mages";
        case FactionId::Bandits:   return "Bandits";
        case FactionId::Wildlings: return "Wildlings";
        default:                   return "?";
    }
}

const char* tierName(ReputationTier t) {
    switch (t) {
        case ReputationTier::Hated:      return "Hated";
        case ReputationTier::Hostile:    return "Hostile";
        case ReputationTier::Unfriendly: return "Unfriendly";
        case ReputationTier::Neutral:    return "Neutral";
        case ReputationTier::Friendly:   return "Friendly";
        case ReputationTier::Honored:    return "Honored";
        case ReputationTier::Exalted:    return "Exalted";
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