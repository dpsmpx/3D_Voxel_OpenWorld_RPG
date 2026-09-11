#include "components.h"
#include "../ecs/components.h"

namespace combat {

u32 Faction::of(ecs::Registry& reg, ecs::Entity e) {
    if (reg.has<ecs::PlayerTag>(e)) return Player;
    if (reg.has<ecs::EnemyTag>(e))  return Hostile;
    if (reg.has<ecs::NPCTag>(e))    return NPC;

    auto* k = reg.get<ecs::Kind>(e);
    if (k) {
        switch (k->value) {
            case ecs::EntityKind::Player: return Player;
            case ecs::EntityKind::Mob:    return Passive;
            case ecs::EntityKind::NPC:    return NPC;
            default: break;
        }
    }
    return 0;
}

} // namespace combat
