/**
 * @file npc_rig.cpp
 * @brief Оснастка NPC: двуногий по определению вида.
 */
#include "npc_rig.h"
#include <array>

namespace npc {

const entity::Rig& rigFor(u16 npcId) {
    static std::array<entity::Rig, NPC_COUNT> cache{};
    static std::array<bool, NPC_COUNT> built{};

    const u16 id = (npcId < NPC_COUNT) ? npcId : (u16)NPC_NONE;
    if (!built[id]) {
        const NpcDef& def = npcRegistry().get(id);

        // Двуногий один на всех: селянин, страж, кузнец, игрок.
        // Отличают виды пропорции и цвета из определения, а не
        // отдельный код на каждого.
        entity::HumanoidSpec spec;
        spec.height      = def.bodyHeight;
        spec.bodyColor   = def.bodyColor;
        spec.headColor   = def.headColor;
        spec.accentColor = def.accentColor;

        cache[id] = entity::humanoidRig(spec);
        built[id] = true;
    }
    return cache[id];
}

} // namespace npc
