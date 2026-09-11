#pragma once
#include "../core/types.h"
#include <string>
#include <vector>

namespace save {

// ============================================================
// Метаданные слота — маленький бинарный файл рядом с сейвом.
// Позволяет показать список слотов без чтения полного файла.
// ============================================================
struct SlotMeta {
    bool exists       = false;
    u32  version      = 0;
    u32  profileId    = 0;
    u32  slotId       = 0;
    u64  seed         = 0;
    u64  timestampMs  = 0;
    u32  playtimeSec  = 0;
    u32  playerLevel  = 1;
    u32  slotChecksum = 0;

    char playerName[32] = {};
    char worldName[32]  = {};
};

// ============================================================
// SaveSlot — пути к файлам слота.
// Директория: <internalDataPath>/saves/
// Сейв:  p<profile>_s<slot>.vxs
// Мета:  p<profile>_s<slot>.meta
// ============================================================
class SaveSlot {
public:
    SaveSlot() = default;
    SaveSlot(std::string dir, u32 profile, u32 slot)
        : dir_(std::move(dir)), profile_(profile), slot_(slot) {}

    const std::string& directory() const { return dir_; }
    u32 profile() const { return profile_; }
    u32 slot()    const { return slot_; }

    std::string dataPath() const;
    std::string metaPath() const;

    bool dataExists() const;
    bool metaExists() const;

    bool readMeta (SlotMeta& out)   const;
    bool writeMeta(const SlotMeta& m) const;
    void removeFiles() const;

private:
    std::string dir_;
    u32 profile_ = 0;
    u32 slot_    = 0;
};

// ============================================================
// SaveSlotManager — знает директорию, перечисляет слоты.
// ============================================================
class SaveSlotManager {
public:
    static constexpr u32 NUM_PROFILES = 3;
    static constexpr u32 NUM_SLOTS    = 3;

    // baseDir — ANativeActivity::internalDataPath.
    // Создаёт <baseDir>/saves/ при необходимости.
    void init(const char* baseDir);

    SaveSlot slot(u32 profile, u32 slot) const;

    // Прочитать метаданные всех 9 слотов.
    void scanAll(SlotMeta outMeta[NUM_PROFILES][NUM_SLOTS]) const;

    const std::string& savesDir()   const { return savesDir_; }
    bool               initialized()const { return initialized_; }

private:
    std::string savesDir_;
    bool        initialized_ = false;
};

} // namespace save