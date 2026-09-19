/**
 * @file audio_events.h
 * @brief Звук: движок AAudio, процедурные эффекты, динамическая музыка.
 */
#pragma once
#include "../core/types.h"
#include "audio_engine.h"
#include <glm/glm.hpp>

namespace audio {

/// Центральный диспетчер игровых событий → звуки.
/// Игровые системы (combat, mob, ui) вызывают методы этого
/// класса, не зная о низкоуровневом AudioEngine.
class AudioEvents {
public:
    void setEngine(AudioEngine* e) { engine_ = e; }

    /// ---- Шаги ----
    void footstep(u16 blockId, const glm::vec3& worldPos);

    /// ---- Движение ----
    void jump(const glm::vec3& worldPos);
    void land(const glm::vec3& worldPos, f32 impactVelocity);

    /// ---- Ближний бой ----
    void swingLight(const glm::vec3& worldPos);
    void swingHeavy(const glm::vec3& worldPos);
    void hitFlesh(const glm::vec3& worldPos);
    void hitBlock(u16 blockId, const glm::vec3& worldPos);

    /// ---- Дальний бой ----
    void arrowShoot(const glm::vec3& worldPos);
    void arrowHit(const glm::vec3& worldPos);
    void spellCast(const glm::vec3& worldPos, u8 damageType);
    void spellHit(const glm::vec3& worldPos, u8 damageType);

    /// ---- Инвентарь ----
    void pickupItem();
    void pickupCoin();
    void dropItem();

    /// ---- UI ----
    void uiClick();
    void uiBack();
    void uiError();

    /// ---- Игрок ----
    void playerHurt();
    void playerDeath();
    void levelUp();

    /// ---- Мобы ----
    void mobHurt(const glm::vec3& worldPos);
    void mobDeath(const glm::vec3& worldPos);
    void mobAttack(const glm::vec3& worldPos);

    /// ---- Погода ----
    ///
    /// Не событие, а СОСТОЯНИЕ: дождь не «случается», он идёт.
    /// Поэтому здесь одна петля, которой меняют громкость, а не
    /// череда одиночных звуков.
    ///
    /// intensity — 0..1 сила осадков. Ноль глушит петлю совсем.
    void setRain(f32 intensity);

    /// ---- Мир ----
    void blockBreak(u16 blockId, const glm::vec3& worldPos);
    void blockPlace(u16 blockId, const glm::vec3& worldPos);
    void craft();
    void enchant();

private:
    AudioEngine* engine_ = nullptr;

    /// Голос дождя. Живёт, пока идёт дождь, и гаснет вместе с ним.
    VoiceHandle rainVoice_{};
    f32 rainGain_ = 0.f;
};

inline AudioEvents& events() {
    static AudioEvents e;
    return e;
}

} // namespace audio
