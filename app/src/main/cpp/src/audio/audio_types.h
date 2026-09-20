/**
 * @file audio_types.h
 * @brief Звук: движок AAudio, процедурные эффекты, динамическая музыка.
 */
#pragma once
#include "../core/types.h"
#include <glm/glm.hpp>

namespace audio {

/// Идентификатор звука. Все звуки процедурно генерируются
/// в SoundRegistry при старте.
enum SoundId : u16 {
    SOUND_NONE = 0,

    // Шаги по разным поверхностям
    SOUND_FOOTSTEP_DIRT,
    SOUND_FOOTSTEP_GRASS,
    SOUND_FOOTSTEP_STONE,
    SOUND_FOOTSTEP_WOOD,
    SOUND_FOOTSTEP_SAND,
    SOUND_FOOTSTEP_WATER,

    // Движение
    SOUND_JUMP,
    SOUND_LAND,

    // Ближний бой
    SOUND_SWING_LIGHT,
    SOUND_SWING_HEAVY,
    SOUND_HIT_FLESH,
    SOUND_HIT_STONE,
    SOUND_HIT_METAL,
    SOUND_HIT_WOOD,

    // Дальний бой и магия
    SOUND_ARROW_SHOOT,
    SOUND_ARROW_HIT,
    SOUND_SPELL_CAST,
    SOUND_SPELL_HIT,

    // Инвентарь
    SOUND_PICKUP_ITEM,
    SOUND_PICKUP_COIN,
    SOUND_DROP_ITEM,

    // UI
    SOUND_UI_CLICK,
    SOUND_UI_BACK,
    SOUND_UI_ERROR,

    // Игрок
    SOUND_PLAYER_HURT,
    SOUND_PLAYER_DEATH,
    SOUND_LEVEL_UP,

    // Мобы
    SOUND_MOB_HURT,
    SOUND_MOB_DEATH,
    SOUND_MOB_ATTACK,

    // ---- Защита ----
    // Три исхода удара по защите обязаны звучать по-разному: по
    // звуку игрок и узнаёт, что именно у него получилось.
    SOUND_GUARD_HIT,     ///< принял на оружие: глухой металл
    SOUND_GUARD_BREAK,   ///< пробили: низкий надтреснутый удар
    SOUND_PARRY,         ///< отбил: высокий чистый звон

    // Мир
    SOUND_BLOCK_BREAK,
    SOUND_BLOCK_PLACE,
    SOUND_DOOR_OPEN,
    SOUND_DOOR_CLOSE,
    SOUND_CRAFT,
    SOUND_ENCHANT,

    // --- Музыка (ТЗ 7: 3-4 трека с плавными переходами) ---
    /// Шум дождя: зацикленный, громкость идёт за силой осадков.
    SOUND_RAIN,

    SOUND_MUSIC_EXPLORE,   ///< мирное исследование, запасной трек
    SOUND_MUSIC_COMBAT,    ///< бой
    SOUND_MUSIC_DUNGEON,   ///< подземелье

    // --- Музыка мест: по треку на биом и на уклад деревни ---
    //
    // Порядок внутри блоков обязан совпадать с world::BiomeId и
    // world::VillageStyle. Звук не должен знать о мире, поэтому
    // связь проверяется static_assert'ом на стороне мира, а здесь
    // блоки просто идут подряд, чтобы номер считался сложением.
    SOUND_MUSIC_BIOME_FIRST,
    SOUND_MUSIC_BIOME_OCEAN = SOUND_MUSIC_BIOME_FIRST,
    SOUND_MUSIC_BIOME_BEACH,
    SOUND_MUSIC_BIOME_PLAINS,
    SOUND_MUSIC_BIOME_FOREST,
    SOUND_MUSIC_BIOME_TAIGA,
    SOUND_MUSIC_BIOME_DESERT,
    SOUND_MUSIC_BIOME_SAVANNA,
    SOUND_MUSIC_BIOME_TUNDRA,
    SOUND_MUSIC_BIOME_MOUNTAINS,
    SOUND_MUSIC_BIOME_SWAMP,
    SOUND_MUSIC_BIOME_VOLCANIC,
    SOUND_MUSIC_BIOME_BLIGHT,
    SOUND_MUSIC_BIOME_END,

    SOUND_MUSIC_VILLAGE_FIRST = SOUND_MUSIC_BIOME_END,
    SOUND_MUSIC_VILLAGE_FARMSTEAD = SOUND_MUSIC_VILLAGE_FIRST,
    SOUND_MUSIC_VILLAGE_STONEMASON,
    SOUND_MUSIC_VILLAGE_GARRISON,
    SOUND_MUSIC_VILLAGE_WOODLAND,
    SOUND_MUSIC_VILLAGE_END,

    SOUND_COUNT = SOUND_MUSIC_VILLAGE_END
};

/// Сколько треков в каждом блоке. Считается из самого enum, а не
/// повторяется числом: разойтись им тогда негде.
constexpr u32 MUSIC_BIOME_COUNT =
    (u32)SOUND_MUSIC_BIOME_END - (u32)SOUND_MUSIC_BIOME_FIRST;
constexpr u32 MUSIC_VILLAGE_COUNT =
    (u32)SOUND_MUSIC_VILLAGE_END - (u32)SOUND_MUSIC_VILLAGE_FIRST;

/// Частота синтеза музыки.
///
/// Вчетверо ниже потока: у синусного пэда выше шести килогерц нет
/// ничего, а шестнадцать шестнадцатисекундных дорожек на 48 кГц —
/// это пятьдесят мегабайт, которые слышно ровно так же. Микшер
/// доигрывает разницу интерполяцией.
constexpr u32 MUSIC_RATE = 12000;

/// Трек биома по его номеру. Вне диапазона — общий «исследование».
inline SoundId musicForBiome(u32 biome) {
    return biome < MUSIC_BIOME_COUNT
         ? (SoundId)((u32)SOUND_MUSIC_BIOME_FIRST + biome)
         : SOUND_MUSIC_EXPLORE;
}
/// Трек деревни по укладу. Вне диапазона — хлебная деревня.
inline SoundId musicForVillage(u32 style) {
    return style < MUSIC_VILLAGE_COUNT
         ? (SoundId)((u32)SOUND_MUSIC_VILLAGE_FIRST + style)
         : SOUND_MUSIC_VILLAGE_FARMSTEAD;
}

/// 3D-слушатель. Обновляется из main каждый кадр.
struct AudioListener {
    glm::vec3 position{0.f};
    glm::vec3 forward{0.f, 0.f, 1.f};
    glm::vec3 up{0.f, 1.f, 0.f};
    glm::vec3 right{1.f, 0.f, 0.f};

    void updateBasis() {
        // right = forward × up
        right = glm::cross(forward, up);
        f32 len = glm::length(right);
        if (len > 0.001f) right /= len;
    }
};

/// Хендл голоса. Индекс в пуле + генерация (для защиты от
/// повторного использования после освобождения).
struct VoiceHandle {
    i32 id = -1;   // -1 = invalid
    u32 gen = 0;

    bool valid() const { return id >= 0; }
    bool operator==(const VoiceHandle& o) const {
        return id == o.id && gen == o.gen;
    }
    bool operator!=(const VoiceHandle& o) const {
        return !(*this == o);
    }
};

} // namespace audio
