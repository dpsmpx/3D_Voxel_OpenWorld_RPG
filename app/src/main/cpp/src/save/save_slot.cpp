/**
 * @file save_slot.cpp
 * @brief Сохранения: бинарный формат, сжатие, дельты мира, слоты.
 */
#include "save_slot.h"
#include "save_format.h"
#include "../core/log.h"
#include <cstdio>
#include <cstring>
#include <sys/stat.h>
#include <sys/types.h>
#include <string>
#include <vector>

namespace save {

// ============================================================
// SaveSlot
// ============================================================
std::string SaveSlot::dataPath() const {
    char buf[256];
    std::snprintf(buf, sizeof(buf), "%s/p%u_s%u.vxs",
                  dir_.c_str(), profile_, slot_);
    return buf;
}

std::string SaveSlot::metaPath() const {
    char buf[256];
    std::snprintf(buf, sizeof(buf), "%s/p%u_s%u.meta",
                  dir_.c_str(), profile_, slot_);
    return buf;
}

bool SaveSlot::dataExists() const {
    FILE* f = std::fopen(dataPath().c_str(), "rb");
    if (!f) return false;
    std::fclose(f);
    return true;
}

bool SaveSlot::metaExists() const {
    FILE* f = std::fopen(metaPath().c_str(), "rb");
    if (!f) return false;
    std::fclose(f);
    return true;
}

bool SaveSlot::writeMeta(const SlotMeta& meta) const {
    ByteWriter w;

    w.writeU32(meta.version);
    w.writeU32(meta.profileId);
    w.writeU32(meta.slotId);
    w.writeU64(meta.seed);
    w.writeU64(meta.timestampMs);
    w.writeU32(meta.playtimeSec);
    w.writeU32(meta.playerLevel);
    w.writeU32(meta.slotChecksum);

    char pname[32] = {};
    std::memcpy(pname, meta.playerName, 31);
    w.raw(pname, 32);

    char wname[32] = {};
    std::memcpy(wname, meta.worldName, 31);
    w.raw(wname, 32);

    FILE* f = std::fopen(metaPath().c_str(), "wb");
    if (!f) {
        LOGE("writeMeta: не удалось открыть %s", metaPath().c_str());
        return false;
    }
    usize written = std::fwrite(w.data().data(), 1, w.size(), f);
    std::fclose(f);
    return written == w.size();
}

bool SaveSlot::readMeta(SlotMeta& out) const {
    FILE* f = std::fopen(metaPath().c_str(), "rb");
    if (!f) return false;

    std::fseek(f, 0, SEEK_END);
    long sz = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);

    if (sz <= 0 || sz > 4096) {
        std::fclose(f);
        return false;
    }

    std::vector<u8> buf((usize)sz);
    std::fread(buf.data(), 1, (usize)sz, f);
    std::fclose(f);

    ByteReader r(buf);
    if (!r.u32v(out.version))      return false;
    if (!r.u32v(out.profileId))    return false;
    if (!r.u32v(out.slotId))       return false;
    if (!r.u64v(out.seed))         return false;
    if (!r.u64v(out.timestampMs))  return false;
    if (!r.u32v(out.playtimeSec))  return false;
    if (!r.u32v(out.playerLevel))  return false;
    if (!r.u32v(out.slotChecksum)) return false;

    if (!r.rawv(out.playerName, 32)) return false;
    if (!r.rawv(out.worldName, 32))  return false;

    out.playerName[31] = '\0';
    out.worldName[31]  = '\0';
    out.exists = true;
    return true;
}

void SaveSlot::removeFiles() const {
    std::remove(dataPath().c_str());
    std::remove(metaPath().c_str());
}

// ============================================================
// SaveSlotManager
// ============================================================
void SaveSlotManager::init(const char* baseDir) {
    savesDir_ = baseDir ? baseDir : "/tmp";
    savesDir_ += "/saves";

    ::mkdir(savesDir_.c_str(), 0755);

    initialized_ = true;
    LOGI("SaveSlotManager: saves dir = %s", savesDir_.c_str());
}

SaveSlot SaveSlotManager::slot(u32 profile, u32 slot) const {
    return SaveSlot(savesDir_, profile, slot);
}

void SaveSlotManager::scanAll(SlotMeta outMeta[NUM_PROFILES][NUM_SLOTS]) const {
    for (u32 p = 0; p < NUM_PROFILES; ++p) {
        for (u32 s = 0; s < NUM_SLOTS; ++s) {
            outMeta[p][s] = SlotMeta{};
            SaveSlot sl = slot(p, s);
            if (!sl.dataExists() || !sl.metaExists()) continue;

            SlotMeta m;
            if (sl.readMeta(m)) {
                m.exists = true;
                outMeta[p][s] = m;
            }
        }
    }
}

bool continueWorld(const SaveSlotManager& slots,
                   i32 lastProfile, i32 lastSlot, u64& outSeed)
{
    outSeed = 0;
    if (lastProfile < 0 || lastSlot < 0) return false;
    if ((u32)lastProfile >= SaveSlotManager::NUM_PROFILES) return false;
    if ((u32)lastSlot    >= SaveSlotManager::NUM_SLOTS)    return false;

    const SaveSlot slot = slots.slot((u32)lastProfile, (u32)lastSlot);
    // И сейв, и метаданные: по одним метаданным мир не открыть, а
    // сейв без них не назвать.
    if (!slot.dataExists() || !slot.metaExists()) return false;

    SlotMeta meta{};
    if (!slot.readMeta(meta)) return false;

    // Зерно ноль — это не мир, это пустая запись: у настоящего мира
    // зерно берётся у системы и нулём не бывает.
    if (meta.seed == 0) return false;

    outSeed = meta.seed;
    return true;
}

} // namespace save
