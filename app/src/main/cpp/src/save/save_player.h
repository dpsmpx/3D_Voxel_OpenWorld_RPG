#pragma once
#include "../core/types.h"
#include "../ecs/registry.h"
#include "save_format.h"

namespace save {

// ============================================================
// Сериализация / десериализация игрока.
//
// Записывается:
//   - Transform (позиция, поворот)
//   - Health, Mana, Stamina
//   - Attributes (STR/AGI/INT/END)
//   - Progression (XP, level, очки)
//   - SkillTree (ранги узлов, очки)
//   - EquippedWeapon (ID оружия, зачарование)
//   - Combatant (faction, сопротивления, кэш)
//   - Resonance (value, stack)
//   - QuestLog (активные квесты + история)
//   - Reputation (значения по фракциям)
//   - Camera mode, selectedBlock
//
// НЕ записывается:
//   - WeaponState (текущая фаза атаки — сбрасываем в Idle)
//   - StatusEffects (сгорают при сохранении)
//   - ActiveDialogue (диалог закрывается)
// ============================================================

void serializePlayer(ByteWriter& w, ecs::Registry& reg, ecs::Entity player);
bool deserializePlayer(ByteReader& r, ecs::Registry& reg, ecs::Entity player);

} // namespace save
