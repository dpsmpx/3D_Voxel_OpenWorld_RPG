/**
 * @file save_slot.h
 * @brief Сохранения: бинарный формат, сжатие, дельты мира, слоты.
 */
#pragma once
#include "../core/types.h"
#include <string>
#include <vector>
#include <utility>

namespace save {

/// Метаданные слота — маленький бинарный файл рядом с сейвом.
/// Позволяет показать список слотов без чтения полного файла.
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

/// SaveSlot — пути к файлам слота.
/// Директория: `internalDataPath/saves/`
/// Сейв:  `p<профиль>_s<слот>.vxs`
/// Мета:  `p<профиль>_s<слот>.meta`
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

/// SaveSlotManager — знает директорию, перечисляет слоты.
class SaveSlotManager {
public:
    static constexpr u32 NUM_PROFILES = 3;
    static constexpr u32 NUM_SLOTS    = 3;

    /// baseDir — ANativeActivity::internalDataPath.
    /// Создаёт `baseDir/saves/` при необходимости.
    void init(const char* baseDir);

    SaveSlot slot(u32 profile, u32 slot) const;

    /// Прочитать метаданные всех 9 слотов.
    void scanAll(SlotMeta outMeta[NUM_PROFILES][NUM_SLOTS]) const;

    const std::string& savesDir()   const { return savesDir_; }
    bool               initialized()const { return initialized_; }

private:
    std::string savesDir_;
    bool        initialized_ = false;
};

/// Какой мир открыть при запуске.
///
/// Игра начиналась новым случайным миром КАЖДЫЙ запуск: прошлый
/// оставался в списке, но добраться до него можно было только
/// вспомнив, в каком он слоте.
///
/// Отдельной функцией, а не веткой в main: решение «продолжать или
/// начинать заново» имеет ровно четыре исхода — слот не задан, слот
/// вне границ, файла нет, файл на месте, — и проверить их можно
/// только тогда, когда они не вплетены в инициализацию приложения.
///
/// @return true, если продолжать есть что; outSeed — зерно того мира.
bool continueWorld(const SaveSlotManager& slots,
                   i32 lastProfile, i32 lastSlot, u64& outSeed);

} // namespace save
