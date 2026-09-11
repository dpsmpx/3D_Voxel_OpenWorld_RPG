#pragma once
#include "../core/types.h"
#include <glm/glm.hpp>

namespace audio {

// ============================================================
// Идентификатор звука. Все звуки процедурно генерируются
// в SoundRegistry при старте.
// ============================================================
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

    // Мир
    SOUND_BLOCK_BREAK,
    SOUND_BLOCK_PLACE,
    SOUND_DOOR_OPEN,
    SOUND_DOOR_CLOSE,
    SOUND_CRAFT,
    SOUND_ENCHANT,

    // --- Музыка (ТЗ 7: 3-4 трека с плавными переходами) ---
    SOUND_MUSIC_EXPLORE,   ///< мирное исследование
    SOUND_MUSIC_COMBAT,    ///< бой
    SOUND_MUSIC_DUNGEON,   ///< подземелье
    SOUND_MUSIC_VILLAGE,   ///< деревня

    SOUND_COUNT
};

// ============================================================
// 3D-слушатель. Обновляется из main каждый кадр.
// ============================================================
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

// ============================================================
// Хендл голоса. Индекс в пуле + генерация (для защиты от
// повторного использования после освобождения).
// ============================================================
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
