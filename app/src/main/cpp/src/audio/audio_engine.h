/**
 * @file audio_engine.h
 * @brief Звук: движок AAudio, процедурные эффекты, динамическая музыка.
 */
#pragma once
#include "../core/types.h"
#include "audio_types.h"
#include "sound.h"
#include <atomic>
#include <glm/glm.hpp>
#include <algorithm>

struct AAudioStreamStruct;
typedef struct AAudioStreamStruct AAudioStream;

namespace audio {

enum VoiceState : u32 {
    VoiceState_Free    = 0,
    VoiceState_Active  = 1,
    VoiceState_Freeing = 2,
    VoiceState_Claimed = 3,
};

struct Voice {
    std::atomic<u32> state{ VoiceState_Free };

    SoundId       soundId      = SOUND_NONE;
    const Sound*  sound        = nullptr;
    u64           cursor       = 0;
    f32           baseGain     = 1.f;
    std::atomic<f32> liveGain{ 1.f };
    std::atomic<f32> atten{ 1.f };
    f32           panL         = 1.f;
    f32           panR         = 1.f;
    bool          looping      = false;
    bool          spatialized  = false;
    bool          randomizedStart = false;

    glm::vec3     position{0.f};

    f32           fadeStart    = 1.f;
    f32           fadeTime     = 0.f;
    f32           fadeDur      = 0.f;

    u32           generation   = 0;
};

class AudioEngine {
public:
    static constexpr i32 MAX_VOICES = 64;

    AudioEngine();
    ~AudioEngine();

    bool init(u32 sampleRate = 48000);
    void shutdown();

    bool isRunning() const { return stream_ != nullptr; }

    void update(f32 dt, const AudioListener& listener);

    VoiceHandle play(SoundId id, f32 gain = 1.f, bool looping = false);
    VoiceHandle play3D(SoundId id, const glm::vec3& pos,
                       f32 gain = 1.f, bool looping = false);

    void stop(VoiceHandle v, f32 fadeSec = 0.f);
    void setVoiceGain(VoiceHandle v, f32 gain);
    void setVoicePosition(VoiceHandle v, const glm::vec3& pos);
    void setVoiceIsMusic(VoiceHandle v, bool isMusic);
    bool isMusicVoice(VoiceHandle v) const;

    void setMasterVolume(f32 v) {
        masterVolume_.store(std::clamp(v, 0.f, 1.f));
    }
    f32 masterVolume() const { return masterVolume_.load(); }

    void setMusicVolume(f32 v) {
        musicVolume_.store(std::clamp(v, 0.f, 1.f));
    }
    f32 musicVolume() const { return musicVolume_.load(); }

    void setSfxVolume(f32 v) {
        sfxVolume_.store(std::clamp(v, 0.f, 1.f));
    }
    f32 sfxVolume() const { return sfxVolume_.load(); }

    void mixInto(f32* out, u32 frames);

    u32 activeVoiceCount() const;
    u32 sampleRate() const { return sampleRate_; }

private:
    i32 claimVoice();
    void releaseVoice(i32 idx);
    void applySpatial(Voice& v, const AudioListener& L);

    AAudioStream* stream_ = nullptr;
    u32 sampleRate_ = 48000;

    Voice voices_[MAX_VOICES];

    std::atomic<f32> masterVolume_{ 1.0f };
    std::atomic<f32> musicVolume_{ 0.7f };
    std::atomic<f32> sfxVolume_{ 0.9f };

    std::atomic<bool> isMusic_[MAX_VOICES];

    AudioListener listenerCache_;
};

/// Глобальный экземпляр движка. Создаётся при первом обращении,
/// живёт до конца процесса.
AudioEngine& engine();

} // namespace audio
