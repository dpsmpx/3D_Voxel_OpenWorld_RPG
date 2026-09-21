/**
 * @file creature_info.cpp
 * @brief Кто это существо: имя и цвет тела, как их видит игрок.
 */
#include "creature_info.h"
#include "mob_rigs.h"
#include "../npc/npc_rig.h"
#include "../npc/npc_def.h"
#include "../player/player_rig.h"
#include "../mobs/mob_ai.h"
#include "../mobs/mob_def.h"
#include "../ecs/components.h"

namespace entity {

u32 bodyColor(const Rig& rig) {
    constexpr u32 FALLBACK = 0xB0B0B0FFu;
    if (rig.count == 0) return FALLBACK;

    f32 bestVolume = -1.f;
    u32 bestColor = FALLBACK;

    for (u8 i = 0; i < rig.count; ++i) {
        const Part& p = rig.parts[i];
        if (!p.visible) continue;
        // Торс — ответ без вариантов: это то, что занимает середину
        // силуэта и что игрок называет «этот серый» или «этот бурый».
        if (p.role == PartRole::Torso) return p.color;

        const f32 v = p.size.x * p.size.y * p.size.z;
        if (v > bestVolume) { bestVolume = v; bestColor = p.color; }
    }
    return bestColor;
}

u32 creatureColor(ecs::Registry& reg, ecs::Entity e) {
    if (auto* tag = reg.get<mobs::MobTag>(e))
        return bodyColor(mobs::rigFor(tag->id));

    if (auto* tag = reg.get<npc::NpcTag>(e)) {
        const auto* look = reg.get<ecs::Appearance>(e);
        return bodyColor(npc::rigFor(tag->id, look ? look->seed : 0u));
    }

    if (reg.has<ecs::PlayerTag>(e))
        return bodyColor(player::rig());

    return 0xB0B0B0FFu;
}

const char* creatureName(ecs::Registry& reg, ecs::Entity e) {
    if (auto* tag = reg.get<mobs::MobTag>(e)) {
        const char* n = mobs::mobRegistry().get(tag->id).name;
        return n ? n : "";
    }
    if (auto* tag = reg.get<npc::NpcTag>(e)) {
        const char* n = npc::npcRegistry().get(tag->id).name;
        return n ? n : "";
    }
    return "";
}

} // namespace entity
