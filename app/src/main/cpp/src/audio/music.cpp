/**
 * @file music.cpp
 * @brief Звук: движок AAudio, процедурные эффекты, динамическая музыка.
 */
#include "music.h"
#include "sound_registry.h"
#include "../core/log.h"
#include <algorithm>
#include <cmath>

namespace audio {

namespace {
/// Идентификаторы треков в том же порядке, что MusicDirector::Track.
constexpr SoundId TRACK_SOUNDS[4] = {
    SOUND_MUSIC_EXPLORE,
    SOUND_MUSIC_COMBAT,
    SOUND_MUSIC_DUNGEON,
    SOUND_MUSIC_VILLAGE,
};
} // namespace

void MusicDirector::init(AudioEngine& engine) {
    engine_ = &engine;

    // Все четыре петли запускаются сразу и играют до конца сессии.
    // Переключение треков — это изменение их громкостей, поэтому
    // переход никогда не рвётся на границе буфера.
    for (u32 i = 0; i < TRACK_COUNT; ++i) {
        voices_[i] = engine.play(TRACK_SOUNDS[i], 0.f, /*looping=*/true);
        if (!voices_[i].valid())
            LOGW("MusicDirector: не удалось запустить трек %u", i);
    }

    levels_[Explore] = 1.f;
    for (u32 i = 1; i < TRACK_COUNT; ++i) levels_[i] = 0.f;
    if (voices_[Explore].valid())
        engine_->setVoiceGain(voices_[Explore], 0.8f);
}

void MusicDirector::shutdown() {
    if (!engine_) return;
    for (auto& v : voices_) {
        if (v.valid()) engine_->stop(v, 1.f);
        v = {};
    }
    engine_ = nullptr;
}

void MusicDirector::approach(f32& value, f32 target, f32 rate, f32 dt) {
    const f32 step = rate * dt;
    const f32 diff = target - value;
    if (std::fabs(diff) <= step) value = target;
    else                         value += (diff > 0.f ? step : -step);
}

void MusicDirector::update(f32 dt, const MusicContext& ctx) {
    if (!engine_) return;

    // ---- Напряжение: насколько ситуация боевая ----
    f32 target = 0.f;
    if (ctx.inCombat) {
        target = 1.f;
    } else {
        if (ctx.nearbyHostiles > 0)
            target = std::min(1.f, 0.4f + (f32)ctx.nearbyHostiles * 0.15f);
        if (ctx.playerHealthPct < 0.35f) target = std::max(target, 0.5f);
        if (ctx.underground)             target = std::max(target, 0.3f);
        if (ctx.inVillage)               target = std::min(target, 0.1f);
    }
    targetTension_ = std::clamp(target, 0.f, 1.f);

    const f32 rate = (targetTension_ > tension_) ? (1.f / ATTACK_TIME)
                                                 : (1.f / RELEASE_TIME);
    approach(tension_, targetTension_, rate, dt);

    // ---- Целевые уровни треков ----
    // Бой перекрывает всё; иначе выбирается по месту: деревня,
    // подземелье или открытый мир.
    f32 want[TRACK_COUNT] = { 0.f, 0.f, 0.f, 0.f };
    want[Combat] = tension_;

    const f32 rest = 1.f - tension_;
    if (ctx.inVillage)          want[Village] = rest;
    else if (ctx.underground)   want[Dungeon] = rest;
    else                        want[Explore] = rest;

    // ---- Пауза приглушает всё разом ----
    approach(pauseGain_, (ctx.paused || paused_) ? 0.15f : 1.f, 1.2f, dt);

    // ---- Применяем ----
    const f32 crossRate = 1.f / ATTACK_TIME;
    for (u32 i = 0; i < TRACK_COUNT; ++i) {
        approach(levels_[i], want[i], crossRate, dt);
        if (voices_[i].valid())
            engine_->setVoiceGain(voices_[i], levels_[i] * pauseGain_ * 0.8f);
    }
}

} // namespace audio
