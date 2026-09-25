/**
 * @file spells.h
 * @brief Бой: заклинания Древа — ледяные иглы и струя огня.
 */
#pragma once
#include "../core/types.h"
#include "../ecs/registry.h"
#include "../world/chunk_manager.h"
#include "components.h"
#include "enchantment.h"
#include "spatial_hash.h"
#include "weapon.h"
#include <glm/glm.hpp>

namespace combat {

struct CombatAction;

/// Заклинания, которые даёт Древо Познания.
///
/// Оружия-предмета у них нет: их не куют и не находят. Навык кладёт
/// в сумку руну, руну надевают как оружие — держат заклинание в той
/// же руке и бьют той же кнопкой, — а платят за каждый бросок маной.
/// Ледышек в запасе нет: игла лепится из маны в момент броска.

/// Заклинание ли это Древа.
bool isTreeSpell(u16 weaponId);

/// Ранг навыка, открывающего заклинание; 0 — не выучено (или это не
/// заклинание Древа).
u8 spellRank(ecs::Registry& reg, ecs::Entity e, u16 weaponId);

/// Руны в сумке — по навыкам: выучил — руна появилась (в поясе, если
/// там есть место), навык сброшен — руна ушла и из рук тоже.
/// @return номер выданной сейчас руны, 0 — ничего не выдано
u16 syncSpellRunes(ecs::Registry& reg, ecs::Entity e);

/// Сколько игл в залпе на этом ранге.
u32 iceNeedleCount(u8 rank);

/// Залп ледяных игл веером из ладони.
/// `damageMult` — все множители урона бросающего (Древо, резонанс).
/// @return сколько игл выпущено
u32 castIceNeedles(ecs::Registry& reg, ecs::Entity caster, u16 weaponId,
                   const Enchantment& enchant, u8 rank,
                   const glm::vec3& origin, const glm::vec3& aim,
                   f32 damageMult, f32 critChance, f32 critMult);

/// Длина струи огня на этом ранге (без множителей досягаемости).
f32 flameReach(u8 rank);

/// Кадр струи огня.
///
/// Пока держат кнопку и хватает маны — ладонь выдыхает пламя: мана
/// уходит каждый кадр, всё в конусе раз в десятую секунды получает
/// урон и загорается, а то, во что струя упирается, загорается само
/// (world::fires()). Отпустили или кончилась мана — струя гаснет.
void updateFlameStream(world::ChunkManager& world, ecs::Registry& reg,
                       const SpatialHash* hash, ecs::Entity caster,
                       const WeaponDef& def, u8 rank,
                       const glm::vec3& origin, const glm::vec3& aim,
                       bool held, bool stunned,
                       f32 damageMult, f32 reachMult, f32 dt,
                       WeaponState& st, CombatAction& out);

/// Период тика урона струи, сек.
constexpr f32 FLAME_TICK = 0.1f;

} // namespace combat
