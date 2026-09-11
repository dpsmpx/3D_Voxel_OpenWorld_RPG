#pragma once
#include "../core/types.h"
#include "audio_engine.h"

namespace audio {

// ============================================================
// Контекст, влияющий на музыку. Обновляется из main каждый кадр.
// ============================================================
struct MusicContext {
    bool inCombat          = false;
    i32  nearbyHostiles    = 0;
    f32  playerHealthPct   = 1.f;
    bool underground       = false;
    bool inVillage         = false;
    bool paused            = false;
};

// ============================================================
// MusicDirector — управляет музыкой.
//
// Две петли: "explore" и "combat". Обе играются одновременно,
// но громкость каждой модулируется tension (0..1).
//   - tension = 0 → слышна только explore
//   - tension = 1 → слышна только combat
//
// tension плавно двигается к target с разной скоростью
// атаки/релаксации.
// ============================================================
class MusicDirector {
public:
    void init(AudioEngine& engine);
    void shutdown();

    void update(f32 dt, const MusicContext& ctx);

    // Принудительно установить tension (для специальных сцен).
    void setTension(f32 t) { targetTension_ = glm::clamp(t, 0.f, 1.f); }

    // Приглушить музыку (например, при выходе в меню).
    void setPaused(bool p) { paused_ = p; }

    f32 tension() const { return tension_; }

private:
    AudioEngine* engine_ = nullptr;
    VoiceHandle exploreVoice_;
    VoiceHandle combatVoice_;

    f32 tension_       = 0.f;
    f32 targetTension_ = 0.f;
    f32 pauseGain_     = 1.f;

    bool paused_ = false;

    // Время атаки/релаксации.
    static constexpr f32 ATTACK_TIME  = 0.8f;
    static constexpr f32 RELEASE_TIME = 2.5f;
};

} // namespace audio
