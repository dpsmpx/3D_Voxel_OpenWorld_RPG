#pragma once
#include "../core/types.h"
#include "../ecs/registry.h"
#include "../world/chunk_manager.h"
#include "save_format.h"
#include "save_slot.h"
#include "world_delta.h"

namespace save {

// ============================================================
// Результат операции сохранения/загрузки.
// ============================================================
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

const char* statusString(SaveStatus s);

// ============================================================
// SaveManager — высокоуровневый API.
//
//   SaveManager mgr;
//   mgr.init(internalDataPath);
//   mgr.save(slot, world, registry, playerEntity, deltas, seed, playtime);
//   mgr.load(slot, world, registry, playerEntity, deltas, &seed, &playtime);
// ============================================================
class SaveManager {
public:
    void init(const char* baseDir);

    SaveSlotManager&       slots()       { return slotMgr_; }
    const SaveSlotManager& slots() const { return slotMgr_; }

    // Сохранить всё состояние в слот.
    SaveStatus save(const SaveSlot& slot,
                    world::ChunkManager& world,
                    ecs::Registry& registry,
                    ecs::Entity playerEntity,
                    const WorldDeltaStore& deltas,
                    u64 worldSeed,
                    u32 playtimeSec);

    // Загрузить состояние. world должен быть уже создан с тем же seed.
    SaveStatus load(const SaveSlot& slot,
                    world::ChunkManager& world,
                    ecs::Registry& registry,
                    ecs::Entity playerEntity,
                    WorldDeltaStore& deltas,
                    u64* outSeed,
                    u32* outPlaytimeSec);

    // ---- Автосейв — в служебный слот (profile=2, slot=2). ----
    static constexpr u32 AUTOSAVE_PROFILE = 2;
    static constexpr u32 AUTOSAVE_SLOT    = 2;

    SaveSlot autosaveSlot() const {
        return slotMgr_.slot(AUTOSAVE_PROFILE, AUTOSAVE_SLOT);
    }

    // Прочитать только метаданные слота.
    SaveStatus peekMeta(const SaveSlot& slot, SlotMeta& out) const;

private:
    SaveSlotManager slotMgr_;
    bool            initialized_ = false;
};

} // namespace save
