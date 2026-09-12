/**
 * @file save_npc.cpp
 * @brief Сохранения: бинарный формат, сжатие, дельты мира, слоты.
 */
#include "save_npc.h"
#include "../ecs/components.h"
#include "../core/log.h"
#include "../npc/npc_def.h"
#include "../npc/npc_ai.h"

#include <cmath>
#include <vector>

namespace save {

using namespace ecs;

void serializeNpcState(ByteWriter& w, ecs::Registry& reg) {
    // В Phase 11 сериализуем только список "убитых" NPC.
    // Логика: проходим по всем NPC с NpcAI::Dead и записываем
    // super-chunk координаты homePos + индекс. Реальный
    // фильтр "не спавнить снова" требует интеграции с npc_spawner
    // (Phase 15).

    std::vector<u32> deadKeys;

    auto& pool = reg.pool<npc::NpcAI>();
    for (usize i = 0; i < pool.size(); ++i) {
        ecs::Entity e = pool.entityAt((u32)i);
        auto* ai  = pool.get(e);
        auto* tag = reg.get<npc::NpcTag>(e);
        auto* hp  = reg.get<Health>(e);
        if (!ai || !tag) continue;
        if (ai->state != npc::NpcAI::Dead && hp && hp->current > 0.f) continue;

        // Ключ: super-chunk X, super-chunk Z, tag id, округлённый idx
        i32 sx = (i32)std::floor(ai->homePos.x / 256.f);
        i32 sz = (i32)std::floor(ai->homePos.z / 256.f);
        u32 key = ((u32)(sx + 0x8000) << 16) ^
                  ((u32)(sz + 0x8000)      ) ^
                  ((u32)tag->id << 24);
        deadKeys.push_back(key);
    }

    w.varU32((u32)deadKeys.size());
    for (u32 k : deadKeys) w.writeU32(k);
}

bool deserializeNpcState(ByteReader& r, ecs::Registry& reg) {
    (void)reg;

    u32 count = 0;
    if (!r.varU32v(count)) return false;
    if (count > 10000) return false;

    // Пока просто читаем и игнорируем. При следующей загрузке
    // спавнер сам решит, каких NPC не создавать, когда мы
    // подключим к нему список "убитых". Сейчас — no-op.
    for (u32 i = 0; i < count; ++i) {
        u32 k = 0;
        if (!r.u32v(k)) return false;
    }
    return true;
}

} // namespace save
