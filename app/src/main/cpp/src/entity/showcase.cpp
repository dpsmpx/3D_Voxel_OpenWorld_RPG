/**
 * @file showcase.cpp
 * @brief Витрина: все сущности разом, детерминированно.
 */
#include "showcase.h"
#include "mob_rigs.h"
#include "../npc/npc_rig.h"
#include "../player/player_rig.h"
#include "../mobs/mob_def.h"

namespace entity {

namespace {

/// Шаг между моделями. Крупнее самого широкого зверя, чтобы соседи не
/// налезали друг на друга: витрина нужна ровно для того, чтобы
/// сравнивать силуэты, а не разбирать кучу.
constexpr f32 SPACING = 4.f;

void add(ShowcaseSlot* out, u8 outMax, u8& n,
         const char* name, const Rig& rig, f32 phase, f32 speedNorm)
{
    if (n >= outMax || rig.count == 0) return;
    ShowcaseSlot& s = out[n];
    s.name = name;
    s.rig  = &rig;
    // Ряд вдоль +X, лицом к зрителю (+Z): так видно и профиль, и фас.
    s.pos  = glm::vec3((f32)n * SPACING, 0.f, 0.f);
    s.yaw  = 0.f;
    s.state.phase     = phase;
    s.state.time      = 0.f;      // время на витрине стоит
    s.state.speedNorm = speedNorm;
    ++n;
}

} // namespace

u8 buildShowcase(ShowcaseSlot* out, u8 outMax, f32 phase, f32 speedNorm) {
    u8 n = 0;
    if (!out || outMax == 0) return 0;

    add(out, outMax, n, "Player", player::rig(), phase, speedNorm);

    for (u16 id = 1; id < npc::NPC_COUNT; ++id)
        add(out, outMax, n, npc::npcRegistry().get(id).name,
            npc::rigFor(id, 0u), phase, speedNorm);

    for (u16 id = 1; id < mobs::MOB_COUNT; ++id)
        add(out, outMax, n, mobs::mobRegistry().get(id).name,
            mobs::rigFor(id), phase, speedNorm);

    return n;
}

u32 showcaseBoxCount(f32 phase, f32 speedNorm) {
    ShowcaseSlot slots[SHOWCASE_MAX];
    const u8 n = buildShowcase(slots, SHOWCASE_MAX, phase, speedNorm);

    u32 boxes = 0;
    for (u8 i = 0; i < n; ++i) {
        Pose pose;
        anim::poseFor(*slots[i].rig, pose, slots[i].state);
        ResolvedPart parts[MAX_PARTS];
        boxes += resolve(*slots[i].rig, pose, slots[i].pos, slots[i].yaw,
                         parts, MAX_PARTS);
    }
    return boxes;
}

} // namespace entity
