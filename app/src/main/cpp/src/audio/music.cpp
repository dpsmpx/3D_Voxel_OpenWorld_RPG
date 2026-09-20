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

SoundId MusicDirector::trackFor(const MusicContext& ctx) {
    // Деревня важнее биома: внутри неё биом никуда не делся, но
    // слышать полагается деревню. Подземелье важнее обоих — там не
    // видно ни того, ни другого.
    if (ctx.underground) return SOUND_MUSIC_DUNGEON;
    if (ctx.inVillage)   return musicForVillage(ctx.villageStyle);
    return musicForBiome(ctx.biome);
}

void MusicDirector::init(AudioEngine& engine) {
    engine_ = &engine;

    combat_ = engine.play(SOUND_MUSIC_COMBAT, 0.f, /*looping=*/true);
    if (combat_.valid()) {
        // Без этой пометки движок считает музыку обычным звуком и
        // применяет к ней громкость эффектов, а ползунок громкости
        // музыки не делает вообще ничего.
        engine.setVoiceIsMusic(combat_, true);
    } else {
        LOGW("MusicDirector: боевой слой не запустился");
    }
    combatLevel_ = 0.f;

    for (auto& p : place_) { p = PlaceSlot{}; }
    cur_ = 0;
    pending_ = SOUND_NONE;
    pendingTime_ = 0.f;
}

void MusicDirector::shutdown() {
    if (!engine_) return;
    if (combat_.valid()) engine_->stop(combat_, 1.f);
    combat_ = {};
    for (auto& p : place_) {
        if (p.voice.valid()) engine_->stop(p.voice, 1.f);
        p = PlaceSlot{};
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

    // ---- Какое место полагается, и выдержало ли оно срок ----
    const SoundId want = trackFor(ctx);
    if (want != pending_) {
        pending_ = want;
        pendingTime_ = 0.f;
    } else {
        pendingTime_ += dt;
    }

    PlaceSlot& cur  = place_[cur_];
    PlaceSlot& prev = place_[1 - cur_];

    const bool first = (cur.id == SOUND_NONE);
    if (want != cur.id && (first || pendingTime_ >= PLACE_HOLD_SEC)) {
        // Предыдущий слот освобождаем: два перехода подряд не
        // складываются в три одновременно звучащих трека.
        if (prev.voice.valid()) engine_->stop(prev.voice, 0.5f);
        prev = PlaceSlot{};

        cur_ = 1 - cur_;
        PlaceSlot& next = place_[cur_];
        next.id   = want;
        next.gain = 0.f;
        next.voice = engine_->play(want, 0.f, /*looping=*/true);
        if (next.voice.valid()) engine_->setVoiceIsMusic(next.voice, true);
        else                    LOGW("MusicDirector: трек места не запустился");

        // Первый трек не выцветает из тишины полминуты: игра только
        // началась, ждать нечего.
        if (first) next.gain = 1.f;
        pendingTime_ = 0.f;
    }

    // ---- Пауза приглушает всё разом ----
    approach(pauseGain_, (ctx.paused || paused_) ? 0.15f : 1.f, 1.2f, dt);

    // ---- Уровни ----
    //
    // Бой вытесняет место, а не заглушает: сумма всегда единица,
    // иначе в бою музыки становится вдвое больше.
    const f32 fadeRate = 1.f / PLACE_FADE_SEC;
    approach(combatLevel_, tension_, 1.f / ATTACK_TIME, dt);

    const f32 rest = 1.f - combatLevel_;
    for (u32 i = 0; i < 2; ++i) {
        PlaceSlot& p = place_[i];
        approach(p.gain, (i == cur_) ? 1.f : 0.f, fadeRate, dt);
        if (!p.voice.valid()) continue;
        engine_->setVoiceGain(p.voice, p.gain * rest * pauseGain_ * MUSIC_LEVEL);
        // Отзвучавший слот отпускаем: голос, вечно висящий на нуле,
        // занимает место в микшере и крутит курсор впустую.
        if (i != cur_ && p.gain <= 0.f) {
            engine_->stop(p.voice, 0.2f);
            p = PlaceSlot{};
        }
    }

    if (combat_.valid())
        engine_->setVoiceGain(combat_, combatLevel_ * pauseGain_ * MUSIC_LEVEL);
}

} // namespace audio
