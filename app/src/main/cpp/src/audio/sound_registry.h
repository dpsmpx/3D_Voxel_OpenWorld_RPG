/**
 * @file sound_registry.h
 * @brief Звук: движок AAudio, процедурные эффекты, динамическая музыка.
 */
#pragma once
#include "../core/types.h"
#include "audio_types.h"
#include "sound.h"

namespace audio {

/// Реестр звуков. При первом обращении генерирует все звуки
/// процедурно. Потокобезопасен для чтения после init().
class SoundRegistry {
public:
    static SoundRegistry& instance();

    /// Генерация всех звуков. Безопасно вызывать один раз.
    void init(u32 sampleRate = 48000);

    const Sound& get(SoundId id) const;

    bool isInitialized() const { return initialized_; }

private:
    SoundRegistry();
    Sound sounds_[SOUND_COUNT];
    u32   sampleRate_ = 48000;
    bool  initialized_ = false;
};

inline SoundRegistry& sounds() { return SoundRegistry::instance(); }

} // namespace audio
