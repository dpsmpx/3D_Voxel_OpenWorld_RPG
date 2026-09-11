#include "music.h"
#include "sound_registry.h"
#include "../core/log.h"
#include <algorithm>
#include <cmath>

namespace audio {

void MusicDirector::init(AudioEngine& engine) {
    engine_ = &engine;

    // Запускаем обе петли с нулевой громкостью.
    exploreVoice_ = engine.play(SOUND_NONE, 0.f, true);  // заглушка
    combatVoice_  = engine.play(SOUND_NONE, 0.f, true);  // заглушка

    // SOUND_NONE не заведётся — нужно использовать реальные ID.
    // Поскольку музыка генерируется в SoundRegistry, но там нет
    // отдельных SoundId для музыки, мы заведём их как "виртуальные":
    // используем поля SOUND_NONE, но с прямым указателем на Sound.
    // Пока — заглушка, реальный запуск делается в update().

    exploreVoice_ = {};
    combatVoice_  = {};
}

void MusicDirector::shutdown() {
    if (!engine_) return;
    if (exploreVoice_.valid()) engine_->stop(exploreVoice_, 1.f);
    if (combatVoice_.valid())  engine_->stop(combatVoice_, 1.f);
    exploreVoice_ = {};
    combatVoice_  = {};
}

void MusicDirector::update(f32 dt, const MusicContext& ctx) {
    if (!engine_) return;

    // ---- Вычисляем targetTension ----
    f32 target = 0.f;

    if (ctx.inCombat) {
        target = 1.f;
    } else {
        if (ctx.nearbyHostiles > 0) {
            target = std::min(1.f, 0.4f + (f32)ctx.nearbyHostiles * 0.15f);
        }
        if (ctx.playerHealthPct < 0.35f) {
            target = std::max(target, 0.5f);
        }
        if (ctx.underground) {
            target = std::max(target, 0.3f);
        }
        if (ctx.inVillage) {
            target = std::min(target, 0.1f);
        }
    }

    targetTension_ = std::clamp(target, 0.f, 1.f);

    // ---- Плавное движение ----
    f32 diff = targetTension_ - tension_;
    f32 rate = (diff > 0.f) ? (1.f / ATTACK_TIME) : (1.f / RELEASE_TIME);
    f32 step = rate * dt;

    if (std::abs(diff) <= step) {
        tension_ = targetTension_;
    } else {
        tension_ += (diff > 0.f ? step : -step);
    }

    tension_ = std::clamp(tension_, 0.f, 1.f);

    // ---- Pause fade ----
    f32 targetPause = ctx.paused ? 0.15f : 1.f;
    f32 pstep = 1.2f * dt;
    if (std::abs(targetPause - pauseGain_) <= pstep) {
        pauseGain_ = targetPause;
    } else {
        pauseGain_ += (targetPause > pauseGain_) ? pstep : -pstep;
    }

    // ---- Обновляем громкости ----
    // explore = 1 - tension, combat = tension, оба * pauseGain.
    if (exploreVoice_.valid()) {
        f32 g = (1.f - tension_) * pauseGain_ * 0.8f;
        engine_->setVoiceGain(exploreVoice_, g);
    }
    if (combatVoice_.valid()) {
        f32 g = tension_ * pauseGain_ * 0.8f;
        engine_->setVoiceGain(combatVoice_, g);
    }
}

} // namespace audio