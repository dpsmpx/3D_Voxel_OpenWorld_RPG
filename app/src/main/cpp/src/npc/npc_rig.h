/**
 * @file npc_rig.h
 * @brief Оснастка NPC: двуногий по определению вида.
 */
#pragma once
#include "../entity/humanoid_rig.h"
#include "npc_def.h"

namespace npc {

/// Оснастка вида. Строится один раз, дальше возвращается ссылка.
///
/// Жила в npc_renderer.cpp, пока нужна была только рендеру. Теперь её
/// спрашивает и спавнер — за длиной шага, которая выводится из длины
/// ног, — а значит место ей рядом с определением вида, а не в рендере.
const entity::Rig& rigFor(u16 npcId);

} // namespace npc
