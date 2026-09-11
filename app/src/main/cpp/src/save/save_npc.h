#pragma once
#include "../core/types.h"
#include "../ecs/registry.h"
#include "save_format.h"

namespace save {

// ============================================================
// Сохранение состояния NPC.
//
// В Phase 11 сохраняем только глобальные счётчики и статичные
// метаданные. Полное состояние NPC (позиция, здоровье, FSM)
// сохранять не нужно, поскольку NPC детерминированно
// респавнятся при загрузке чанка рядом с деревней.
//
// Исключение — убитые NPC: сохраняем список (superX, superZ, idx)
// убитых в деревнях, чтобы не появлялись заново.
// ============================================================
void serializeNpcState(ByteWriter& w, ecs::Registry& reg);
bool deserializeNpcState(ByteReader& r, ecs::Registry& reg);

} // namespace save