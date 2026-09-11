#include "sound_registry.h"
#include "../core/log.h"
#include <cmath>
#include <cstring>
#include <algorithm>

namespace audio {

namespace {

// ============================================================
// Детерминированный PRNG для генерации.
// ============================================================
struct Rng {
    u32 s;
    explicit Rng(u32 seed) : s(seed ? seed : 1) {}
    u32 next() {
        s = s * 1103515245u + 12345u;
        return s;
    }
    // [-1, 1]
    f32 noise() {
        u32 v = next() >> 16;
        return ((f32)v / 32767.5f) - 1.f;
    }
};

// ============================================================
// Огибающая. attack/decay в секундах.
// ============================================================
inline f32 envAD(f32 t, f32 dur, f32 attack, f32 decay, f32 sustainLevel) {
    if (t < 0.f || t > dur) return 0.f;
    if (t < attack) return t / std::max(0.001f, attack);
    f32 d = (t - attack) / std::max(0.001f, decay);
    if (d >= 1.f) return 0.f;
    return (1.f - d) * (1.f - sustainLevel) + sustainLevel * (1.f - d);
}

inline f32 expoDecay(f32 t, f32 rate) {
    return std::exp(-t * rate);
}

// ============================================================
// Базовые генераторы.
// ============================================================

// Шум с экспоненциальным затуханием. cutoff 0..1 (грубый low-pass).
void genNoiseBurst(Sound& s, u32 sr, f32 dur, f32 amp,
                   f32 decayRate, f32 lowpass)
{
    u32 n = (u32)(sr * dur);
    s.samples.resize(n);
    s.frames = n;
    s.sampleRate = sr;

    Rng r(0xDEADBEEFu);
    f32 prev = 0.f;
    for (u32 i = 0; i < n; ++i) {
        f32 t = (f32)i / (f32)sr;
        f32 raw = r.noise();
        // Simple one-pole low-pass: y = y + alpha*(x - y)
        prev = prev + lowpass * (raw - prev);
        f32 env = expoDecay(t, decayRate);
        s.samples[i] = prev * env * amp;
    }
}

// Синус с ADSR-огибающей.
void genSineTone(Sound& s, u32 sr, f32 dur, f32 freq, f32 amp,
                 f32 attack, f32 decay, f32 sustainLevel)
{
    u32 n = (u32)(sr * dur);
    s.samples.resize(n);
    s.frames = n;
    s.sampleRate = sr;

    for (u32 i = 0; i < n; ++i) {
        f32 t = (f32)i / (f32)sr;
        f32 phase = t * freq * 6.2831853f;
        f32 env = envAD(t, dur, attack, decay, sustainLevel);
        s.samples[i] = std::sin(phase) * env * amp;
    }
}

// Sweep — меняет частоту от f0 к f1.
void genSweepTone(Sound& s, u32 sr, f32 dur, f32 f0, f32 f1, f32 amp,
                  f32 decayRate)
{
    u32 n = (u32)(sr * dur);
    s.samples.resize(n);
    s.frames = n;
    s.sampleRate = sr;

    f32 phase = 0.f;
    for (u32 i = 0; i < n; ++i) {
        f32 t = (f32)i / (f32)sr;
        f32 tt = t / dur;
        f32 freq = f0 + (f1 - f0) * tt;
        phase += freq * 6.2831853f / (f32)sr;
        f32 env = expoDecay(t, decayRate);
        s.samples[i] = std::sin(phase) * env * amp;
    }
}

// Комбинация двух тонов (быстрый "blip").
void genTwoToneBlip(Sound& s, u32 sr, f32 dur,
                    f32 f1, f32 f2, f32 amp)
{
    u32 n = (u32)(sr * dur);
    s.samples.resize(n);
    s.frames = n;
    s.sampleRate = sr;

    f32 half = dur * 0.5f;
    f32 phase = 0.f;
    for (u32 i = 0; i < n; ++i) {
        f32 t = (f32)i / (f32)sr;
        f32 freq = (t < half) ? f1 : f2;
        phase += freq * 6.2831853f / (f32)sr;
        f32 env = (t < half)
            ? expoDecay(t, 30.f)
            : expoDecay(t - half, 40.f);
        s.samples[i] = std::sin(phase) * env * amp;
    }
}

// Резонансный удар (металл): смесь синуса с узкой полосой.
void genMetallicHit(Sound& s, u32 sr, f32 dur, f32 baseFreq, f32 amp)
{
    u32 n = (u32)(sr * dur);
    s.samples.resize(n);
    s.frames = n;
    s.sampleRate = sr;

    Rng r(0xACE1u);
    for (u32 i = 0; i < n; ++i) {
        f32 t = (f32)i / (f32)sr;
        f32 env = expoDecay(t, 22.f);
        // Несколько обертонов
        f32 v = 0.f;
        v += std::sin(t * baseFreq * 6.2831853f) * 0.5f;
        v += std::sin(t * baseFreq * 2.01f * 6.2831853f) * 0.3f;
        v += std::sin(t * baseFreq * 3.97f * 6.2831853f) * 0.2f;
        v += r.noise() * 0.15f * expoDecay(t, 60.f);
        s.samples[i] = v * env * amp;
    }
}

// Клац (удар по камню/дереву).
void genClick(Sound& s, u32 sr, f32 dur, f32 amp, f32 lowpass)
{
    u32 n = (u32)(sr * dur);
    s.samples.resize(n);
    s.frames = n;
    s.sampleRate = sr;

    Rng r(0xBEEFu);
    f32 prev = 0.f;
    for (u32 i = 0; i < n; ++i) {
        f32 t = (f32)i / (f32)sr;
        f32 raw = r.noise();
        prev = prev + lowpass * (raw - prev);
        f32 env = expoDecay(t, 80.f);
        s.samples[i] = prev * env * amp;
    }
}

// Свист (замах оружия): полосовой шум со sweep по частоте.
void genWhoosh(Sound& s, u32 sr, f32 dur, f32 amp, f32 startAmp)
{
    u32 n = (u32)(sr * dur);
    s.samples.resize(n);
    s.frames = n;
    s.sampleRate = sr;

    Rng r(0x1234u);
    // Один фильтр с меняющейся cutoff — эмуляция свиста
    f32 lp = 0.f;
    for (u32 i = 0; i < n; ++i) {
        f32 t = (f32)i / (f32)sr;
        f32 tt = t / dur;
        // Cutoff растёт в середине, падает в конце
        f32 cutoff = startAmp + (1.f - startAmp) * std::sin(tt * 3.14159f);
        cutoff = std::clamp(cutoff, 0.05f, 0.95f);
        lp += cutoff * (r.noise() - lp);
        // Envelope колоколом
        f32 env = std::sin(tt * 3.14159f);
        s.samples[i] = lp * env * amp;
    }
}

// Аккорд из нескольких синусов с медленной огибающей (для музыкальных пэдов).
void addChordTo(Sound& s, u32 sr, f32 startTime, f32 duration,
                f32 f1, f32 f2, f32 f3, f32 amp)
{
    u32 startFrame = (u32)(startTime * sr);
    u32 durFrames  = (u32)(duration * sr);

    for (u32 i = 0; i < durFrames; ++i) {
        u32 idx = startFrame + i;
        if (idx >= s.samples.size()) break;
        f32 t = (f32)i / (f32)sr;
        f32 tt = t / duration;
        // Bell envelope
        f32 env = std::sin(tt * 3.14159f);
        env *= env;   // soft attack/release
        f32 v = 0.f;
        v += std::sin(t * f1 * 6.2831853f) * 0.45f;
        v += std::sin(t * f2 * 6.2831853f) * 0.35f;
        v += std::sin(t * f3 * 6.2831853f) * 0.20f;
        s.samples[idx] += v * env * amp;
    }
}

// Простой kick (для боевой музыки).
void addKick(Sound& s, u32 sr, f32 startTime, f32 amp)
{
    f32 dur = 0.20f;
    u32 startFrame = (u32)(startTime * sr);
    u32 durFrames  = (u32)(dur * sr);
    f32 phase = 0.f;

    for (u32 i = 0; i < durFrames; ++i) {
        u32 idx = startFrame + i;
        if (idx >= s.samples.size()) break;
        f32 t = (f32)i / (f32)sr;
        // Частота падает с 180 до 60
        f32 freq = 60.f + 120.f * expoDecay(t, 25.f);
        phase += freq * 6.2831853f / (f32)sr;
        f32 env = expoDecay(t, 12.f);
        s.samples[idx] += std::sin(phase) * env * amp;
    }
}

// ============================================================
// Генерация каждой звуковой дорожки.
// ============================================================

void genFootstep(Sound& s, u32 sr, f32 base, f32 lowpass, u32 seed) {
    f32 dur = 0.09f;
    u32 n = (u32)(sr * dur);
    s.samples.assign(n, 0.f);
    s.frames = n;
    s.sampleRate = sr;

    Rng r(seed);
    f32 prev = 0.f;
    for (u32 i = 0; i < n; ++i) {
        f32 t = (f32)i / (f32)sr;
        prev = prev + lowpass * (r.noise() - prev);
        f32 env = expoDecay(t, 50.f);
        s.samples[i] = prev * env * base;
    }
}

void genFootstepSet(Sound& s, u32 sr, f32 lowpass, u32 seed, f32 amp) {
    // 4 варианта шагов, слегка отличающихся
    f32 dur = 0.08f;
    u32 oneN = (u32)(sr * dur);
    u32 totalN = oneN * 4;
    s.samples.assign(totalN, 0.f);
    s.frames = totalN;
    s.sampleRate = sr;

    Rng r(seed);
    for (int v = 0; v < 4; ++v) {
        f32 prev = 0.f;
        f32 decay = 45.f + (f32)(r.next() % 30);
        f32 gain = amp * (0.85f + 0.15f * ((r.next() % 100) / 100.f));
        for (u32 i = 0; i < oneN; ++i) {
            f32 t = (f32)i / (f32)sr;
            prev = prev + lowpass * (r.noise() - prev);
            f32 env = expoDecay(t, decay);
            s.samples[v * oneN + i] = prev * env * gain;
        }
    }
}

// ============================================================
// Генерация музыки — отдельная функция.
// ============================================================

void genExploreMusic(Sound& s, u32 sr) {
    f32 dur = 16.f;
    u32 n = (u32)(sr * dur);
    s.samples.assign(n, 0.f);
    s.frames = n;
    s.sampleRate = sr;
    s.looping = true;
    s.defaultGain = 0.55f;

    // Пентатоника C (Hz)
    const f32 C4  = 261.63f;
    const f32 D4  = 293.66f;
    const f32 E4  = 329.63f;
    const f32 G4  = 392.00f;
    const f32 A4  = 440.00f;
    const f32 C5  = 523.25f;

    // 4 аккорда по 4 секунды
    // Am - F - C - G (по низам)
    // Используем низкие тоны как дрone и высокие как пэд

    addChordTo(s, sr, 0.0f,  4.0f, 110.00f, 164.81f, 220.00f, 0.35f); // Am
    addChordTo(s, sr, 4.0f,  4.0f,  87.31f, 130.81f, 174.61f, 0.35f); // F
    addChordTo(s, sr, 8.0f,  4.0f, 130.81f, 196.00f, 261.63f, 0.35f); // C
    addChordTo(s, sr, 12.0f, 4.0f,  98.00f, 146.83f, 196.00f, 0.35f); // G

    // Мелодия — разреженные ноты пентатоники
    Rng r(0xF00Du);
    f32 melodyNotes[6] = { C4, D4, E4, G4, A4, C5 };
    for (int i = 0; i < 8; ++i) {
        f32 t0 = (f32)i * 2.0f + (f32)(r.next() % 100) * 0.005f;
        if (t0 >= dur - 1.f) break;
        f32 f = melodyNotes[r.next() % 6];
        f32 amp = 0.12f + (f32)(r.next() % 50) * 0.002f;

        // Простой тон с огибающей
        f32 noteDur = 1.6f;
        u32 nf = (u32)(noteDur * sr);
        u32 start = (u32)(t0 * sr);
        for (u32 j = 0; j < nf; ++j) {
            u32 idx = start + j;
            if (idx >= n) break;
            f32 t = (f32)j / (f32)sr;
            f32 tt = t / noteDur;
            f32 env = std::sin(tt * 3.14159f);
            env *= env;
            f32 v = std::sin(t * f * 6.2831853f) * env * amp;
            s.samples[idx] += v;
        }
    }

    // Нормализация
    f32 maxAmp = 0.f;
    for (f32 v : s.samples) maxAmp = std::max(maxAmp, std::abs(v));
    if (maxAmp > 0.f) {
        f32 k = 0.85f / maxAmp;
        for (f32& v : s.samples) v *= k;
    }
}

void genCombatMusic(Sound& s, u32 sr) {
    f32 dur = 8.f;
    u32 n = (u32)(sr * dur);
    s.samples.assign(n, 0.f);
    s.frames = n;
    s.sampleRate = sr;
    s.looping = true;
    s.defaultGain = 0.55f;

    // Низкий drone
    for (u32 i = 0; i < n; ++i) {
        f32 t = (f32)i / (f32)sr;
        f32 v = 0.f;
        v += std::sin(t * 55.00f * 6.2831853f) * 0.30f;
        v += std::sin(t * 82.41f * 6.2831853f) * 0.20f;
        // Медленная пульсация
        f32 pulse = 0.7f + 0.3f * std::sin(t * 3.14159f);
        s.samples[i] += v * pulse;
    }

    // Быстрый арпеджио
    f32 notes[8] = {
        220.00f, 261.63f, 329.63f, 392.00f,
        440.00f, 392.00f, 329.63f, 261.63f,
    };
    f32 noteDur = 0.25f;
    for (int bar = 0; bar < 32; ++bar) {
        f32 t0 = (f32)bar * noteDur;
        if (t0 >= dur) break;
        f32 f = notes[bar % 8];
        f32 amp = 0.08f;

        u32 nf = (u32)(noteDur * sr);
        u32 start = (u32)(t0 * sr);
        for (u32 j = 0; j < nf; ++j) {
            u32 idx = start + j;
            if (idx >= n) break;
            f32 t = (f32)j / (f32)sr;
            f32 tt = t / noteDur;
            f32 env = expoDecay(tt * 3.f, 3.f);
            f32 v = std::sin(t * f * 6.2831853f) * env * amp;
            s.samples[idx] += v;
        }
    }

    // Kick каждый bar (каждые 1 сек)
    for (int bar = 0; bar < 8; ++bar) {
        addKick(s, sr, (f32)bar * 1.0f, 0.7f);
    }

    // Нормализация
    f32 maxAmp = 0.f;
    for (f32 v : s.samples) maxAmp = std::max(maxAmp, std::abs(v));
    if (maxAmp > 0.f) {
        f32 k = 0.90f / maxAmp;
        for (f32& v : s.samples) v *= k;
    }
}

} // namespace

// ============================================================
// SoundRegistry
// ============================================================
SoundRegistry::SoundRegistry() = default;

SoundRegistry& SoundRegistry::instance() {
    static SoundRegistry inst;
    return inst;
}

void SoundRegistry::init(u32 sampleRate) {
    if (initialized_) return;

    sampleRate_ = sampleRate;
    LOGI("SoundRegistry: генерация %u звуков при %u Hz",
         (unsigned)SOUND_COUNT - 1, sampleRate);

    // ---- Footsteps ----
    genFootstepSet(sounds_[SOUND_FOOTSTEP_DIRT],  sampleRate, 0.25f, 0x1001, 0.6f);
    genFootstepSet(sounds_[SOUND_FOOTSTEP_GRASS], sampleRate, 0.15f, 0x1002, 0.5f);
    genFootstepSet(sounds_[SOUND_FOOTSTEP_STONE], sampleRate, 0.55f, 0x1003, 0.7f);
    genFootstepSet(sounds_[SOUND_FOOTSTEP_WOOD],  sampleRate, 0.35f, 0x1004, 0.6f);
    genFootstepSet(sounds_[SOUND_FOOTSTEP_SAND],  sampleRate, 0.10f, 0x1005, 0.4f);
    genFootstepSet(sounds_[SOUND_FOOTSTEP_WATER], sampleRate, 0.20f, 0x1006, 0.55f);

    // ---- Jump / Land ----
    genSweepTone(sounds_[SOUND_JUMP], sampleRate, 0.12f, 240.f, 400.f, 0.35f, 25.f);
    sounds_[SOUND_JUMP].defaultGain = 0.7f;

    genNoiseBurst(sounds_[SOUND_LAND], sampleRate, 0.14f, 0.55f, 30.f, 0.4f);
    sounds_[SOUND_LAND].defaultGain = 0.85f;

    // ---- Melee ----
    genWhoosh(sounds_[SOUND_SWING_LIGHT], sampleRate, 0.12f, 0.45f, 0.10f);
    genWhoosh(sounds_[SOUND_SWING_HEAVY], sampleRate, 0.22f, 0.60f, 0.05f);

    // Hit flesh — низкий "плевок"
    genNoiseBurst(sounds_[SOUND_HIT_FLESH], sampleRate, 0.08f, 0.7f, 45.f, 0.25f);

    // Hit stone — высокий клик
    genClick(sounds_[SOUND_HIT_STONE], sampleRate, 0.06f, 0.65f, 0.55f);

    // Hit metal — резонансный
    genMetallicHit(sounds_[SOUND_HIT_METAL], sampleRate, 0.20f, 900.f, 0.55f);

    // Hit wood — глухой
    genNoiseBurst(sounds_[SOUND_HIT_WOOD], sampleRate, 0.10f, 0.55f, 60.f, 0.30f);

    // ---- Ranged ----
    genWhoosh(sounds_[SOUND_ARROW_SHOOT], sampleRate, 0.20f, 0.35f, 0.15f);
    genClick(sounds_[SOUND_ARROW_HIT], sampleRate, 0.08f, 0.55f, 0.45f);
    genSweepTone(sounds_[SOUND_SPELL_CAST], sampleRate, 0.28f, 200.f, 800.f, 0.35f, 12.f);
    genNoiseBurst(sounds_[SOUND_SPELL_HIT], sampleRate, 0.20f, 0.55f, 18.f, 0.35f);

    // ---- Pickup ----
    genTwoToneBlip(sounds_[SOUND_PICKUP_ITEM], sampleRate, 0.12f, 660.f, 880.f, 0.35f);
    genTwoToneBlip(sounds_[SOUND_PICKUP_COIN], sampleRate, 0.10f, 1100.f, 1500.f, 0.35f);
    genTwoToneBlip(sounds_[SOUND_DROP_ITEM], sampleRate, 0.10f, 500.f, 350.f, 0.30f);

    // ---- UI ----
    genTwoToneBlip(sounds_[SOUND_UI_CLICK], sampleRate, 0.05f, 1400.f, 1400.f, 0.30f);
    genTwoToneBlip(sounds_[SOUND_UI_BACK], sampleRate, 0.07f, 800.f, 600.f, 0.30f);
    genTwoToneBlip(sounds_[SOUND_UI_ERROR], sampleRate, 0.15f, 220.f, 180.f, 0.40f);

    // ---- Player ----
    genNoiseBurst(sounds_[SOUND_PLAYER_HURT], sampleRate, 0.18f, 0.55f, 12.f, 0.25f);
    genSweepTone(sounds_[SOUND_PLAYER_DEATH], sampleRate, 0.9f, 400.f, 80.f, 0.45f, 3.5f);

    // Level up — восходящая арпеджио
    {
        Sound& s = sounds_[SOUND_LEVEL_UP];
        f32 dur = 0.9f;
        u32 n = (u32)(sampleRate * dur);
        s.samples.assign(n, 0.f);
        s.frames = n;
        s.sampleRate = sampleRate;

        f32 notes[4] = { 523.25f, 659.25f, 783.99f, 1046.50f };
        for (int k = 0; k < 4; ++k) {
            f32 t0 = (f32)k * 0.18f;
            f32 noteDur = 0.5f;
            u32 start = (u32)(t0 * sampleRate);
            u32 nf = (u32)(noteDur * sampleRate);
            for (u32 j = 0; j < nf; ++j) {
                u32 idx = start + j;
                if (idx >= n) break;
                f32 t = (f32)j / (f32)sampleRate;
                f32 env = expoDecay(t, 6.f);
                f32 v = std::sin(t * notes[k] * 6.2831853f) * 0.28f;
                v += std::sin(t * notes[k] * 2.f * 6.2831853f) * 0.10f;
                s.samples[idx] += v * env;
            }
        }
    }

    // ---- Mob ----
    genNoiseBurst(sounds_[SOUND_MOB_HURT], sampleRate, 0.15f, 0.55f, 18.f, 0.30f);
    genSweepTone(sounds_[SOUND_MOB_DEATH], sampleRate, 0.55f, 350.f, 90.f, 0.40f, 5.f);
    genNoiseBurst(sounds_[SOUND_MOB_ATTACK], sampleRate, 0.10f, 0.45f, 35.f, 0.40f);

    // ---- World ----
    genClick(sounds_[SOUND_BLOCK_BREAK], sampleRate, 0.14f, 0.55f, 0.55f);
    genClick(sounds_[SOUND_BLOCK_PLACE], sampleRate, 0.08f, 0.45f, 0.50f);
    genSweepTone(sounds_[SOUND_DOOR_OPEN], sampleRate, 0.30f, 400.f, 700.f, 0.28f, 10.f);
    genSweepTone(sounds_[SOUND_DOOR_CLOSE], sampleRate, 0.25f, 600.f, 300.f, 0.30f, 12.f);
    genTwoToneBlip(sounds_[SOUND_CRAFT], sampleRate, 0.20f, 700.f, 900.f, 0.35f);
    genSweepTone(sounds_[SOUND_ENCHANT], sampleRate, 0.60f, 300.f, 1200.f, 0.32f, 6.f);

    initialized_ = true;
    LOGI("SoundRegistry: готово");
}

const Sound& SoundRegistry::get(SoundId id) const {
    static Sound empty;
    if (id >= SOUND_COUNT) return empty;
    return sounds_[id];
}

} // namespace audio