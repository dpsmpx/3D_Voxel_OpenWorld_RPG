/**
 * @file save_manager.h
 * @brief Сохранения: бинарный формат, сжатие, дельты мира, слоты.
 */
#pragma once
#include "../core/types.h"
#include "../ecs/registry.h"
#include "../world/chunk_manager.h"
#include "../world/day_cycle.h"
#include "../npc/npc_spawner.h"
#include "../world/hazards.h"
#include "save_format.h"
#include "save_slot.h"
#include "world_delta.h"

namespace save {

/// Результат операции сохранения/загрузки.
enum class SaveStatus : u8 {
    Ok = 0,
    FileNotFound,
    ReadError,
    WriteError,
    BadMagic,
    UnsupportedVersion,
    CorruptedData,
    ChecksumMismatch,
};

/// Человекочитаемое описание результата сохранения или загрузки.
const char* statusString(SaveStatus s);

/// SaveManager — высокоуровневый API.
///
///   SaveManager mgr;
///   mgr.init(internalDataPath);
///   mgr.save(slot, world, registry, playerEntity, deltas, seed, playtime);
///   mgr.load(slot, world, registry, playerEntity, deltas, &seed, &playtime);
class SaveManager {
public:
    void init(const char* baseDir);

    SaveSlotManager&       slots()       { return slotMgr_; }
    const SaveSlotManager& slots() const { return slotMgr_; }

    // Сохранить всё состояние в слот.
    /// day — игровые сутки: сохраняются, чтобы загруженный мир
    /// продолжился в то же время суток, а не с рассвета.
    SaveStatus save(const SaveSlot& slot,
                    world::ChunkManager& world,
                    ecs::Registry& registry,
                    ecs::Entity playerEntity,
                    const WorldDeltaStore& deltas,
                    u64 worldSeed,
                    u32 playtimeSec,
                    const world::DayCycle& day,
                    const npc::NpcSpawner& npcSpawner,
                    const hazards::TreasureKeeper& treasures,
                    const char* worldName = nullptr);

    /// Загрузить состояние. world должен быть уже создан с тем же seed.
    SaveStatus load(const SaveSlot& slot,
                    world::ChunkManager& world,
                    ecs::Registry& registry,
                    ecs::Entity playerEntity,
                    WorldDeltaStore& deltas,
                    u64* outSeed,
                    u32* outPlaytimeSec,
                    world::DayCycle* outDay,
                    npc::NpcSpawner& npcSpawner,
                    hazards::TreasureKeeper& treasures);

    /// ---- Автосейв — в служебный слот (profile=2, slot=2). ----
    static constexpr u32 AUTOSAVE_PROFILE = 2;
    static constexpr u32 AUTOSAVE_SLOT    = 2;

    SaveSlot autosaveSlot() const {
        return slotMgr_.slot(AUTOSAVE_PROFILE, AUTOSAVE_SLOT);
    }

    /// Прочитать только метаданные слота.
    SaveStatus peekMeta(const SaveSlot& slot, SlotMeta& out) const;

private:
    SaveSlotManager slotMgr_;
    bool            initialized_ = false;
};

} // namespace save
