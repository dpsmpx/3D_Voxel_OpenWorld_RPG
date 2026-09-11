/**
 * @file music.h
 * @brief Звук: движок AAudio, процедурные эффекты, динамическая музыка.
 */
#pragma once
#include "../core/types.h"
#include "audio_engine.h"

namespace audio {

/// Контекст, влияющий на музыку. Обновляется из main каждый кадр.
struct MusicContext {
    bool inCombat          = false;
    i32  nearbyHostiles    = 0;
    f32  playerHealthPct   = 1.f;
    bool underground       = false;
    bool inVillage         = false;
    bool paused            = false;
};

/// MusicDirector — управляет музыкой (ТЗ 7).
///
/// Четыре петли играют одновременно с нулевой громкостью, а
/// переключение — это кроссфейд их уровней. Так переходы всегда
/// плавные и без щелчков: ни один трек не стартует и не
/// останавливается посреди игры.
///
///   explore  — мирное исследование, база
///   combat   — бой, вытесняет остальные
///   dungeon  — под землёй
///   village  — рядом со станциями крафта и жителями
class MusicDirector {
public:
    void init(AudioEngine& engine);
    void shutdown();

    void update(f32 dt, const MusicContext& ctx);

    /// Принудительно установить tension (для специальных сцен).
    void setTension(f32 t) { targetTension_ = glm::clamp(t, 0.f, 1.f); }

    /// Приглушить музыку (например, при выходе в меню).
    void setPaused(bool p) { paused_ = p; }

    f32 tension() const { return tension_; }

private:
    /// Сколько треков крутится одновременно.
    static constexpr u32 TRACK_COUNT = 4;
    enum Track : u32 { Explore = 0, Combat, Dungeon, Village };

    /// Плавно ведёт текущий уровень к целевому.
    static void approach(f32& value, f32 target, f32 rate, f32 dt);

    AudioEngine* engine_ = nullptr;
    VoiceHandle  voices_[TRACK_COUNT];
    f32          levels_[TRACK_COUNT] = { 1.f, 0.f, 0.f, 0.f };

    f32 tension_       = 0.f;
    f32 targetTension_ = 0.f;
    f32 pauseGain_     = 1.f;

    bool paused_ = false;

    /// Время атаки/релаксации кроссфейда.
    static constexpr f32 ATTACK_TIME  = 0.8f;
    static constexpr f32 RELEASE_TIME = 2.5f;
};

} // namespace audio
