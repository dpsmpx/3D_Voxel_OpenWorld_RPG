/**
 * @file save_manager.cpp
 * @brief Сохранения: бинарный формат, сжатие, дельты мира, слоты.
 */
#include "save_manager.h"
#include "../world/world_spec.h"
#include "zlib_util.h"
#include "save_player.h"
#include "save_npc.h"
#include "save_inventory.h"
#include "../core/log.h"
#include "../ecs/components.h"
#include "../progression/progression.h"
#include <cstdio>
#include <cstring>
#include <ctime>
#include <string>
#include <vector>
#include <unistd.h>

namespace save {

const char* statusString(SaveStatus s) {
    switch (s) {
        case SaveStatus::Ok:                 return "OK";
        case SaveStatus::FileNotFound:       return "File not found";
        case SaveStatus::ReadError:          return "Read error";
        case SaveStatus::WriteError:         return "Write error";
        case SaveStatus::BadMagic:           return "Bad magic";
        case SaveStatus::UnsupportedVersion: return "Unsupported version";
        case SaveStatus::CorruptedData:      return "Corrupted data";
        case SaveStatus::ChecksumMismatch:   return "Checksum mismatch";
    }
    return "?";
}

void SaveManager::init(const char* baseDir) {
    slotMgr_.init(baseDir);
    initialized_ = true;
}

SaveStatus SaveManager::save(const SaveSlot& slot,
                             world::ChunkManager& world,
                             ecs::Registry& registry,
                             ecs::Entity playerEntity,
                             const WorldDeltaStore& deltas,
                             u64 worldSeed,
                             u32 playtimeSec,
                             const world::DayCycle& day,
                             const npc::NpcSpawner& npcSpawner,
                             const hazards::TreasureKeeper& treasures,
                             const char* worldName)
{
    if (!initialized_) return SaveStatus::WriteError;
    // Воксели не сохраняются: мир восстанавливается из зерна, а
    // изменения игрока — из дельт. Ссылка нужна ради seed в шапке.
    (void)world;

    ByteWriter body;
    body.reserve(256 * 1024);

    // Время суток идёт первым: короткое поле фиксированного размера,
    // его удобно читать, не разбирая остальное тело.
    body.writeF32(day.rawTime());
    body.writeU32(day.day());

    serializePlayer(body, registry, playerEntity);
    deltas.write(body);
    serializeNpcState(body, npcSpawner);
    serializeTreasures(body, treasures);
    serializePickups(body, registry);

    std::vector<u8> compressed = zcompress(body.data(), 6);
    if (compressed.empty() && !body.empty()) {
        return SaveStatus::WriteError;
    }

    ByteWriter header;
    header.writeU32(SAVE_MAGIC);
    header.writeU32(SAVE_VERSION);
    header.writeU32(slot.profile());
    header.writeU32(slot.slot());
    header.writeU64(worldSeed);
    header.writeU64((u64)std::time(nullptr) * 1000ULL);
    header.writeU32(playtimeSec);
    header.writeU32((u32)body.size());
    header.writeU32((u32)compressed.size());
    header.writeU32(crc32_compute(compressed.data(), compressed.size()));

    std::string path = slot.dataPath();
    const std::string tmpPath = path + ".tmp";
    FILE* f = std::fopen(tmpPath.c_str(), "wb");
    if (!f) {
        LOGE("Save: не удалось открыть %s", tmpPath.c_str());
        return SaveStatus::WriteError;
    }

    // Заголовок обязан быть ровно той длины, которую ждёт загрузчик:
    // разойдясь на четыре байта, они уже однажды сделали все сейвы
    // нечитаемыми.
    if (header.size() != SAVE_HEADER_SIZE) {
        LOGE("Save: заголовок %zu байт вместо %u", header.size(),
             (unsigned)SAVE_HEADER_SIZE);
        std::fclose(f);
        std::remove(tmpPath.c_str());
        return SaveStatus::WriteError;
    }

    bool ok = true;
    ok = ok && std::fwrite(header.data().data(), 1, header.size(), f) == header.size();
    ok = ok && std::fwrite(compressed.data(), 1, compressed.size(), f) == compressed.size();
    if (std::fflush(f) != 0 || ::fsync(::fileno(f)) != 0) ok = false;
    std::fclose(f);
    if (!ok || std::rename(tmpPath.c_str(), path.c_str()) != 0) {
        std::remove(tmpPath.c_str());
        return SaveStatus::WriteError;
    }

    SlotMeta meta{};
    meta.exists       = true;
    meta.version      = SAVE_VERSION;
    meta.profileId    = slot.profile();
    meta.slotId       = slot.slot();
    meta.seed         = worldSeed;
    meta.timestampMs  = (u64)std::time(nullptr) * 1000ULL;
    meta.playtimeSec  = playtimeSec;
    meta.slotChecksum = crc32_compute(compressed.data(), compressed.size());

    if (auto* prog = registry.get<progression::Progression>(playerEntity)) {
        meta.playerLevel = prog->level;
    }
    // Имя мира даёт игрок. Своего здесь нет и быть не может: до
    // списка миров имя было одно на всех — «World» и хвост зерна, —
    // и девять слотов в меню различались только этим хвостом.
    if (worldName && worldName[0] != '\0')
        std::snprintf(meta.worldName, sizeof(meta.worldName), "%s", worldName);
    else
        world::defaultWorldName(meta.worldName, sizeof(meta.worldName), worldSeed);
    std::snprintf(meta.playerName, sizeof(meta.playerName), "Adventurer");

    if (!slot.writeMeta(meta)) return SaveStatus::WriteError;

    LOGI("Save OK: %s  body=%zu  comp=%zu  delta_chunks=%zu  mods=%zu",
         path.c_str(), body.size(), compressed.size(),
         deltas.chunkCount(), deltas.totalMods());

    return SaveStatus::Ok;
}

SaveStatus SaveManager::load(const SaveSlot& slot,
                             world::ChunkManager& world,
                             ecs::Registry& registry,
                             ecs::Entity playerEntity,
                             WorldDeltaStore& deltas,
                             u64* outSeed,
                             u32* outPlaytimeSec,
                             world::DayCycle* outDay,
                             npc::NpcSpawner& npcSpawner,
                             hazards::TreasureKeeper& treasures)
{
    if (!initialized_) return SaveStatus::ReadError;

    std::string path = slot.dataPath();
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) {
        LOGE("Load: не найден %s", path.c_str());
        return SaveStatus::FileNotFound;
    }

    u8 headBuf[SAVE_HEADER_SIZE];
    if (std::fread(headBuf, 1, SAVE_HEADER_SIZE, f) != SAVE_HEADER_SIZE) {
        std::fclose(f);
        return SaveStatus::ReadError;
    }

    ByteReader hr(headBuf, SAVE_HEADER_SIZE);

    u32 magic = 0, version = 0, profile = 0, slotId = 0;
    u64 seed = 0, ts = 0;
    u32 playtime = 0, origSize = 0, compSize = 0, checksum = 0;

    if (!hr.u32v(magic))         { std::fclose(f); return SaveStatus::ReadError; }
    if (magic != SAVE_MAGIC)     { std::fclose(f); return SaveStatus::BadMagic; }
    if (!hr.u32v(version))       { std::fclose(f); return SaveStatus::ReadError; }
    if (version != SAVE_VERSION) { std::fclose(f); return SaveStatus::UnsupportedVersion; }

    // Результат каждого чтения проверяем. Раньше он отбрасывался, и
    // не дочитанная контрольная сумма молча оставалась нулём —
    // ошибка выглядела как «файл битый», а бит был загрузчик.
    bool head = true;
    head = head && hr.u32v(profile);
    head = head && hr.u32v(slotId);
    head = head && hr.u64v(seed);
    head = head && hr.u64v(ts);
    head = head && hr.u32v(playtime);
    head = head && hr.u32v(origSize);
    head = head && hr.u32v(compSize);
    head = head && hr.u32v(checksum);
    if (!head) {
        LOGE("Load: заголовок не дочитан");
        std::fclose(f);
        return SaveStatus::ReadError;
    }

    (void)profile; (void)slotId; (void)ts;

    // ============================================================
    // Phase 15: проверка seed
    // Если seed из сейва не совпадает с seed мира — сейв непригоден.
    // ============================================================
    if (world.seed() != seed) {
        LOGE("Save: seed mismatch (file=%llX world=%llX)",
             (unsigned long long)seed,
             (unsigned long long)world.seed());
        std::fclose(f);
        return SaveStatus::CorruptedData;
    }

    std::vector<u8> compressed(compSize);
    usize read = std::fread(compressed.data(), 1, compSize, f);
    std::fclose(f);

    if (read != compSize) return SaveStatus::ReadError;

    u32 actualCrc = crc32_compute(compressed.data(), compressed.size());
    if (actualCrc != checksum) {
        LOGE("Save: CRC mismatch (expected=%u actual=%u)", checksum, actualCrc);
        return SaveStatus::ChecksumMismatch;
    }

    std::vector<u8> body = zdecompress(compressed, 64ull * 1024ull * 1024ull);
    if (body.size() != origSize) {
        LOGE("Save: decompressed size mismatch (%zu vs %u)",
             body.size(), origSize);
        return SaveStatus::CorruptedData;
    }

    ByteReader br(body);

    // Время суток — в том же порядке, что и при записи.
    f32 timeOfDay = 0.3f;
    u32 dayNumber = 0;
    if (!br.f32v(timeOfDay) || !br.u32v(dayNumber)) {
        return SaveStatus::CorruptedData;
    }
    if (outDay) outDay->setRaw(timeOfDay, dayNumber);

    if (!deserializePlayer(br, registry, playerEntity)) {
        LOGE("Save: player deserialize failed");
        return SaveStatus::CorruptedData;
    }

    if (!deltas.read(br)) {
        LOGE("Save: world delta deserialize failed");
        return SaveStatus::CorruptedData;
    }

    if (!deserializeNpcState(br, npcSpawner)) {
        LOGE("Save: npc deserialize failed");
        return SaveStatus::CorruptedData;
    }

    if (!deserializeTreasures(br, treasures)) {
        LOGE("Save: treasure deserialize failed");
        return SaveStatus::CorruptedData;
    }

    if (!deserializePickups(br, registry)) {
        LOGE("Save: pickups deserialize failed");
        return SaveStatus::CorruptedData;
    }

    if (!deltas.applyAll(world)) return SaveStatus::CorruptedData;

    if (outSeed)        *outSeed = seed;
    if (outPlaytimeSec) *outPlaytimeSec = playtime;

    LOGI("Load OK: %s  seed=%llX  playtime=%us  delta_chunks=%zu  mods=%zu",
         path.c_str(),
         (unsigned long long)seed, playtime,
         deltas.chunkCount(), deltas.totalMods());

    return SaveStatus::Ok;
}

SaveStatus SaveManager::peekMeta(const SaveSlot& slot, SlotMeta& out) const {
    if (slot.readMeta(out)) {
        out.exists = true;
        return SaveStatus::Ok;
    }
    return SaveStatus::FileNotFound;
}

} // namespace save