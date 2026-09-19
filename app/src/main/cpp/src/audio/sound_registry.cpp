/**
 * @file sound_registry.cpp
 * @brief Звук: движок AAudio, процедурные эффекты, динамическая музыка.
 */
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
// Огибающая. Затухание — экспоненциальное, rate в 1/с.
// ============================================================
/// Линейная смесь. Своя, чтобы не тащить glm в генератор звука.
inline f32 glm_mix(f32 a, f32 b, f32 k) { return a + (b - a) * k; }

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

/// Из чего сложен шаг.
///
/// Прежний шаг был одной вспышкой отфильтрованного шума в
/// восемьдесят миллисекунд — то есть ЩЕЛЧКОМ. Щелчок и слышался:
/// «тук-тук-тук» одинаковой высоты, по камню и по траве почти
/// одинаково.
///
/// Настоящий шаг состоит из двух частей, и слышно именно их: удар
/// пятки — короткий низкий толчок, и следом шорох, с каким нога
/// доезжает по поверхности. По соотношению этих двух и узнают, по
/// чему идут: по камню удар резкий и шороха почти нет, по траве
/// удара нет вовсе, а шорох длинный.
struct StepVoice {
    f32 dur;          ///< длительность, с
    f32 thumpFreq;    ///< частота удара пятки, Гц
    f32 thumpDecay;   ///< как быстро он гаснет
    f32 thumpAmp;     ///< и насколько он слышен
    f32 crunchLP;     ///< яркость шороха, 0..1
    f32 crunchDecay;
    f32 crunchAmp;
    f32 crunchDelay;  ///< на сколько шорох отстаёт от удара, с
    f32 grain;        ///< зернистость: 0 — ровное шипение, 1 — крупинки
    f32 ring;         ///< призвук: доска гудит, камень звенит
};

void genFootstepSet(Sound& s, u32 sr, const StepVoice& v, u32 seed, f32 amp) {
    // Четыре варианта: одинаковый шаг подряд слышен как запись.
    const u32 oneN = (u32)((f32)sr * v.dur);
    const u32 totalN = oneN * 4;
    s.samples.assign(totalN, 0.f);
    s.frames = totalN;
    s.sampleRate = sr;

    Rng r(seed);
    for (i32 k = 0; k < 4; ++k) {
        // Каждый шаг чуть иной: высота удара, скорость затухания и
        // громкость гуляют. Ровно повторённый звук ухо ловит сразу.
        const f32 pitch = 0.88f + 0.24f * ((f32)(r.next() % 100) / 100.f);
        const f32 gain  = amp * (0.82f + 0.18f * ((f32)(r.next() % 100) / 100.f));
        const f32 delay = v.crunchDelay *
                          (0.7f + 0.6f * ((f32)(r.next() % 100) / 100.f));

        f32 lp = 0.f;
        f32 grainHold = 0.f;
        i32 grainLeft = 0;

        for (u32 i = 0; i < oneN; ++i) {
            const f32 t = (f32)i / (f32)sr;

            // ---- Удар пятки ----
            f32 thump = 0.f;
            if (v.thumpAmp > 0.f) {
                const f32 f = v.thumpFreq * pitch;
                thump = std::sin(6.2831853f * f * t) *
                        expoDecay(t, v.thumpDecay) * v.thumpAmp;
                // Призвук: доска отзывается октавой выше и дольше.
                if (v.ring > 0.f)
                    thump += std::sin(6.2831853f * f * 2.02f * t) *
                             expoDecay(t, v.thumpDecay * 0.45f) *
                             v.thumpAmp * v.ring;
            }

            // ---- Шорох ----
            f32 crunch = 0.f;
            if (t >= delay) {
                f32 raw = r.noise();
                // Зернистость: шум не ровный, а рассыпанный на
                // крупинки. Трава и песок отличаются от камня именно
                // этим — не тембром, а тем, что шорох дробный.
                if (v.grain > 0.f) {
                    if (grainLeft <= 0) {
                        grainLeft = 1 + (i32)(r.next() % 6u);
                        grainHold = raw;
                    }
                    --grainLeft;
                    raw = glm_mix(raw, grainHold, v.grain);
                }
                lp += v.crunchLP * (raw - lp);
                crunch = lp * expoDecay(t - delay, v.crunchDecay) * v.crunchAmp;
            }

            s.samples[(u32)k * oneN + i] = (thump + crunch) * gain;
        }
    }
}

/// Шум дождя: сплошной, ровный, зацикленный.
///
/// Не набор капель, а именно ШУМ: отдельные капли на слух
/// складываются в треск, а дождь слышится как широкая полоса с
/// медленно гуляющей плотностью. Гуляет она оттого, что ветер
/// приносит заряды.
void genRain(Sound& s, u32 sr) {
    // Четыре секунды: короче — и петля слышна как повтор.
    const f32 dur = 4.f;
    const u32 n = (u32)((f32)sr * dur);
    s.samples.assign(n, 0.f);
    s.frames = n;
    s.sampleRate = sr;
    s.looping = true;
    s.defaultGain = 0.6f;

    Rng r(0x2A19u);
    f32 hi = 0.f, lo = 0.f;
    for (u32 i = 0; i < n; ++i) {
        const f32 t = (f32)i / (f32)sr;
        const f32 raw = r.noise();

        // Два слоя: шипение струй и гул ливня. Один только верх
        // звучит как радиопомеха, один низ — как ветер в трубе.
        hi += 0.45f * (raw - hi);
        lo += 0.06f * (raw - lo);

        // Плотность гуляет: заряд налетел, заряд ушёл.
        const f32 swell = 0.78f + 0.22f * std::sin(6.2831853f * 0.21f * t)
                                * std::sin(6.2831853f * 0.07f * t);

        f32 v = (hi * 0.75f + lo * 1.8f) * swell;

        // Стык петли сшивается: разрыв волны в точке склейки
        // слышится щелчком раз в четыре секунды.
        const f32 edge = 0.08f;
        if (t < edge)            v *= t / edge;
        if (t > dur - edge)      v *= (dur - t) / edge;

        s.samples[i] = v;
    }

    // Нормировка. Два слоя, сложенные с разным весом, дают пик выше
    // единицы — и это не «громко», это клиппинг: всё, что выше,
    // срезается в квадрат и хрипит.
    f32 peak = 0.f;
    for (f32 v : s.samples) peak = std::max(peak, std::fabs(v));
    if (peak > 0.f) {
        const f32 k = 0.70f / peak;
        for (f32& v : s.samples) v *= k;
    }
}

// ============================================================
// Генерация музыки — отдельная функция.
// ============================================================

// ============================================================
// Музыка места: один генератор, шестнадцать рецептов.
// ============================================================
//
// У каждого биома и каждого уклада деревни своя дорожка. Писать
// шестнадцать функций по сорок строк значило бы шестнадцать раз
// повторить нормировку, сшивку петли и огибающую — и разойтись в
// них. Отличается музыка мест не устройством, а ладом, темпом,
// плотностью мелодии и шумовым слоем: это и есть рецепт.

/// Лад: полутона ступеней от корня.
enum class Mode : u8 { Major = 0, Minor, PentaMajor, Phrygian, WholeTone,
                       Lydian, Mixolydian, Count };

/// Ступени лада. Длина — сколько их в октаве.
struct ScaleDef { const i8* steps; u8 count; };

inline ScaleDef scaleOf(Mode m) {
    static const i8 MAJOR[]      = { 0, 2, 4, 5, 7, 9, 11 };
    static const i8 MINOR[]      = { 0, 2, 3, 5, 7, 8, 10 };
    static const i8 PENTA[]      = { 0, 2, 4, 7, 9 };
    static const i8 PHRYGIAN[]   = { 0, 1, 3, 5, 7, 8, 10 };
    static const i8 WHOLE[]      = { 0, 2, 4, 6, 8, 10 };
    static const i8 LYDIAN[]     = { 0, 2, 4, 6, 7, 9, 11 };
    static const i8 MIXO[]       = { 0, 2, 4, 5, 7, 9, 10 };
    switch (m) {
        case Mode::Minor:      return { MINOR,    7 };
        case Mode::PentaMajor: return { PENTA,    5 };
        case Mode::Phrygian:   return { PHRYGIAN, 7 };
        case Mode::WholeTone:  return { WHOLE,    6 };
        case Mode::Lydian:     return { LYDIAN,   7 };
        case Mode::Mixolydian: return { MIXO,     7 };
        default:               return { MAJOR,    7 };
    }
}

inline f32 semitone(f32 root, f32 st) {
    return root * std::pow(2.f, st / 12.f);
}

/// Рецепт дорожки места.
struct PlaceMusic {
    f32  root;          ///< корень, Гц
    Mode mode;
    f32  barSec;        ///< сколько длится один аккорд
    i8   chord[4];      ///< корни четырёх аккордов, полутона от root
    u8   notesPerBar;   ///< плотность мелодии; 0 — без мелодии вовсе
    f32  melodyUp;      ///< на сколько октав мелодия выше корня
    f32  padAmp;
    f32  melAmp;
    f32  airAmp;        ///< шумовой слой: ветер, прибой, шорох
    f32  airBright;     ///< 0 — гул, 1 — свист
    f32  pulseHz;       ///< тремоло пэда; 0 — ровно
    f32  detune;        ///< расстройка второго голоса, полутона
    u32  seed;
    f32  gain;
};

/// Синтез дорожки по рецепту.
///
/// Четыре аккорда по barSec, поверх — редкая мелодия из ступеней
/// лада и ровный шумовой слой. Петля сшивается по краям: разрыв
/// волны в точке склейки слышится щелчком каждые двадцать секунд, а
/// музыка играет часами.
void genPlaceMusic(Sound& s, u32 sr, const PlaceMusic& p) {
    const f32 dur = p.barSec * 4.f;
    const u32 n = (u32)((f32)sr * dur);
    s.samples.assign(n, 0.f);
    s.frames = n;
    s.sampleRate = sr;
    s.looping = true;
    s.defaultGain = p.gain;

    const ScaleDef sc = scaleOf(p.mode);
    // Терция лада — вторая ступень гаммы, квинта — четвёртая у
    // пятиступенных ладов и пятая у семиступенных. Аккорд строится
    // из лада, а не из зашитой мажорной тройки: иначе фригийский
    // биом звучал бы мажорно, и весь рецепт был бы впустую.
    const u8 thirdIdx = 2u;
    const u8 fifthIdx = (sc.count >= 7) ? 4u : 3u;

    // ---- Пэд ----
    for (u32 bar = 0; bar < 4; ++bar) {
        const f32 t0 = (f32)bar * p.barSec;
        const f32 base = semitone(p.root, (f32)p.chord[bar]);
        const f32 f1 = base;
        const f32 f2 = semitone(base, (f32)sc.steps[thirdIdx % sc.count]);
        const f32 f3 = semitone(base, (f32)sc.steps[fifthIdx % sc.count]);

        const u32 start = (u32)(t0 * (f32)sr);
        const u32 nf    = (u32)(p.barSec * (f32)sr);
        for (u32 j = 0; j < nf; ++j) {
            const u32 idx = start + j;
            if (idx >= n) break;
            const f32 t = (f32)j / (f32)sr;
            const f32 tt = t / p.barSec;
            f32 env = std::sin(tt * 3.14159265f);
            env *= env;
            // Тремоло: болото и вулкан дышат, равнина — нет.
            if (p.pulseHz > 0.f)
                env *= 0.72f + 0.28f * std::sin(6.2831853f * p.pulseHz * (t0 + t));

            const f32 tw = (t0 + t);   // расстроенный голос считаем от начала петли
            f32 v = 0.f;
            v += std::sin(tw * f1 * 6.2831853f) * 0.45f;
            v += std::sin(tw * f2 * 6.2831853f) * 0.32f;
            v += std::sin(tw * f3 * 6.2831853f) * 0.22f;
            if (p.detune != 0.f) {
                const f32 d = semitone(f1, p.detune);
                v += std::sin(tw * d * 6.2831853f) * 0.24f;
            }
            s.samples[idx] += v * env * p.padAmp;
        }
    }

    // ---- Мелодия ----
    if (p.notesPerBar > 0) {
        Rng r(p.seed);
        const f32 noteDur = p.barSec / (f32)p.notesPerBar;
        const f32 melRoot = p.root * std::pow(2.f, p.melodyUp);
        const u32 total = 4u * p.notesPerBar;
        for (u32 k = 0; k < total; ++k) {
            // Не каждую долю: ровная цепочка нот звучит как гамма.
            if ((r.next() & 3u) == 0u) continue;
            const f32 t0 = (f32)k * noteDur;
            const u8  deg = (u8)(r.next() % sc.count);
            const f32 oct = (r.next() & 7u) == 0u ? 12.f : 0.f;
            const f32 f = semitone(melRoot, (f32)sc.steps[deg] + oct);
            const u32 start = (u32)(t0 * (f32)sr);
            const u32 nf = (u32)(noteDur * 0.9f * (f32)sr);
            for (u32 j = 0; j < nf; ++j) {
                const u32 idx = start + j;
                if (idx >= n) break;
                const f32 t = (f32)j / (f32)sr;
                const f32 tt = t / (noteDur * 0.9f);
                f32 env = std::sin(tt * 3.14159265f);
                env *= env;
                s.samples[idx] += std::sin(t * f * 6.2831853f) * env * p.melAmp;
            }
        }
    }

    // ---- Шумовой слой ----
    //
    // Прибой у океана, позёмка в тундре, шелест в лесу. Один
    // однополюсный фильтр: чем выше airBright, тем ближе к свисту.
    if (p.airAmp > 0.f) {
        Rng r(p.seed ^ 0x5EEDu);
        const f32 alpha = 0.02f + 0.40f * p.airBright;
        f32 lp = 0.f;
        for (u32 i = 0; i < n; ++i) {
            const f32 t = (f32)i / (f32)sr;
            lp += alpha * (r.noise() - lp);
            const f32 swell = 0.7f + 0.3f * std::sin(6.2831853f * (0.11f / p.barSec) * t);
            s.samples[i] += lp * swell * p.airAmp * 2.4f;
        }
    }

    // ---- Сшивка петли ----
    {
        const f32 edge = 0.12f;
        const u32 ne = (u32)(edge * (f32)sr);
        for (u32 i = 0; i < ne && i < n; ++i) {
            const f32 k = (f32)i / (f32)ne;
            s.samples[i] *= k;
            s.samples[n - 1 - i] *= k;
        }
    }

    // ---- Нормировка ----
    //
    // Пэд, мелодия и шум складываются с разным весом и дают пик выше
    // единицы. Это не «громко», это клиппинг: всё, что выше, срезается
    // в квадрат и хрипит.
    f32 peak = 0.f;
    for (f32 v : s.samples) peak = std::max(peak, std::fabs(v));
    if (peak > 0.f) {
        const f32 k = 0.85f / peak;
        for (f32& v : s.samples) v *= k;
    }
}

/// Рецепты биомов. Порядок — world::BiomeId.
inline const PlaceMusic* biomeMusicTable() {
    static const PlaceMusic T[MUSIC_BIOME_COUNT] = {
        // Океан: широко и медленно, снизу прибой.
        {  65.41f, Mode::Major,      6.0f, {  0, -5, -7, -3 }, 1, 2.0f,
           0.30f, 0.07f, 0.22f, 0.10f, 0.08f,  0.00f, 0x0Cu, 0.42f },
        // Пляж: светло, коротко, шорох песка.
        { 130.81f, Mode::Major,      4.0f, {  0,  5,  7,  2 }, 2, 1.0f,
           0.26f, 0.10f, 0.13f, 0.55f, 0.00f,  0.00f, 0x1Bu, 0.44f },
        // Равнина: простор, мажорная пентатоника, почти без шума.
        { 146.83f, Mode::PentaMajor, 4.0f, {  0,  7,  5,  9 }, 2, 1.0f,
           0.28f, 0.11f, 0.05f, 0.35f, 0.00f,  0.00f, 0x2Au, 0.46f },
        // Лес: тёплый минор, мелодия чаще, шелест листвы.
        { 110.00f, Mode::Minor,      4.0f, {  0, -3,  5,  3 }, 3, 2.0f,
           0.27f, 0.09f, 0.09f, 0.45f, 0.00f,  0.00f, 0x3Du, 0.45f },
        // Тайга: холодно и редко, высокий свист позёмки.
        {  98.00f, Mode::Minor,      5.0f, {  0, -5,  3, -2 }, 1, 2.0f,
           0.26f, 0.08f, 0.11f, 0.80f, 0.00f,  0.00f, 0x4Eu, 0.43f },
        // Пустыня: фригийский лад — вторая ступень пониженная.
        { 123.47f, Mode::Phrygian,   5.0f, {  0,  1,  5,  1 }, 2, 1.0f,
           0.25f, 0.10f, 0.15f, 0.70f, 0.00f,  0.00f, 0x5Fu, 0.43f },
        // Саванна: миксолидийский, пульсирующий, бодрый шаг.
        { 146.83f, Mode::Mixolydian, 3.0f, {  0, -2,  5,  3 }, 3, 1.0f,
           0.26f, 0.11f, 0.07f, 0.50f, 2.00f,  0.00f, 0x6Au, 0.45f },
        // Тундра: целотонный лад — ни мажора, ни минора, пустота.
        {  87.31f, Mode::WholeTone,  7.0f, {  0,  2, -2,  4 }, 1, 2.0f,
           0.24f, 0.07f, 0.13f, 0.85f, 0.00f,  0.00f, 0x7Bu, 0.41f },
        // Горы: лидийский, величаво, медленно.
        {  73.42f, Mode::Lydian,     6.0f, {  0,  7,  2,  9 }, 1, 2.0f,
           0.29f, 0.08f, 0.10f, 0.30f, 0.00f,  0.00f, 0x8Cu, 0.44f },
        // Болото: вязко, низко, пэд дышит.
        {  61.74f, Mode::Minor,      6.0f, {  0, -2,  3, -5 }, 1, 2.0f,
           0.30f, 0.06f, 0.18f, 0.15f, 0.60f,  0.00f, 0x9Du, 0.40f },
        // Вулкан: гул без мелодии, расстроенный голос, тревожный пульс.
        {  55.00f, Mode::Phrygian,   5.0f, {  0,  1, -4,  1 }, 0, 0.0f,
           0.34f, 0.00f, 0.24f, 0.05f, 1.30f,  0.35f, 0xAEu, 0.40f },
        // Порча: расстроено и мертво. Мелодии нет — некому играть.
        {  58.27f, Mode::Phrygian,   6.0f, {  0, -1,  1, -6 }, 0, 0.0f,
           0.31f, 0.00f, 0.16f, 0.25f, 0.25f, -0.45f, 0xBFu, 0.40f },
    };
    return T;
}

/// Рецепты деревень. Порядок — world::VillageStyle.
inline const PlaceMusic* villageMusicTable() {
    static const PlaceMusic T[MUSIC_VILLAGE_COUNT] = {
        // Хлебная: светлый мажор, частая мелодия, людно.
        { 130.81f, Mode::Major,      3.5f, {  0,  7,  9,  5 }, 4, 1.0f,
           0.26f, 0.12f, 0.04f, 0.40f, 0.00f,  0.00f, 0xC1u, 0.46f },
        // Каменная: степенный минор, мелодия реже.
        { 110.00f, Mode::Minor,      4.0f, {  0,  5,  3,  7 }, 3, 1.0f,
           0.28f, 0.10f, 0.05f, 0.30f, 0.00f,  0.00f, 0xD2u, 0.45f },
        // Сторожевая: маршевый пульс, короткий шаг.
        {  98.00f, Mode::Minor,      3.0f, {  0, -5,  3,  5 }, 4, 1.0f,
           0.29f, 0.11f, 0.05f, 0.35f, 1.60f,  0.00f, 0xE3u, 0.45f },
        // Лесная: пентатоника, разреженно, шелест вокруг.
        { 123.47f, Mode::PentaMajor, 4.5f, {  0,  9,  2,  7 }, 2, 1.0f,
           0.27f, 0.09f, 0.10f, 0.45f, 0.00f,  0.00f, 0xF4u, 0.44f },
    };
    return T;
}

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

// Подземелье: низкий дрон, редкие капли, никакой мелодии —
// напряжение без ритма.
void genDungeonMusic(Sound& s, u32 sr) {
    const f32 dur = 20.f;
    const u32 n = (u32)(sr * dur);
    s.samples.assign(n, 0.f);
    s.frames = n;
    s.sampleRate = sr;
    s.looping = true;
    s.defaultGain = 0.45f;

    // Два дрона в кварту, слегка расстроенные — даёт биения.
    addChordTo(s, sr, 0.0f,  10.f, 55.00f, 73.42f, 82.41f, 0.30f);
    addChordTo(s, sr, 10.0f, 10.f, 49.00f, 65.41f, 73.42f, 0.30f);

    // Редкие «капли»: короткий высокий тон с длинным хвостом.
    Rng r(0xD00Du);
    for (int i = 0; i < 10; ++i) {
        const f32 t0 = (f32)i * 2.0f + (f32)(r.next() % 140) * 0.01f;
        if (t0 >= dur - 1.5f) break;
        const f32 f = 900.f + (f32)(r.next() % 700);
        const u32 start = (u32)(t0 * sr);
        const u32 nf = (u32)(1.2f * sr);
        for (u32 j = 0; j < nf; ++j) {
            const u32 idx = start + j;
            if (idx >= n) break;
            const f32 t = (f32)j / (f32)sr;
            const f32 env = std::exp(-t * 5.f);
            s.samples[idx] += std::sin(t * f * 6.2831853f) * env * 0.10f;
        }
    }

    f32 maxAmp = 0.f;
    for (f32 v : s.samples) maxAmp = std::max(maxAmp, std::abs(v));
    if (maxAmp > 0.f) {
        const f32 k = 0.85f / maxAmp;
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

    // ---- Шаги ----
    //
    // У каждой поверхности своё соотношение удара и шороха, и
    // слышно именно его. Земля — глухой толчок и короткий шорох;
    // трава — почти один шорох, дробный; камень — резкий удар со
    // звоном; доска — гулкий удар с призвуком; песок — сыпучий
    // шорох без удара; вода — плеск с длинным мокрым хвостом.
    //
    // Числа мерились, а не подбирались на слух: доля высоких частот
    // и отношение головы к хвосту считаются проверкой, и она же не
    // даёт двум поверхностям сойтись в одно.
    //                            длит  Гц   спад  удар  ярк  спад  шор  задр зерн звон
    const StepVoice DIRT_V  { 0.20f,  95.f, 42.f, 0.50f, 0.30f, 14.f, 0.50f, 0.012f, 0.35f, 0.f  };
    const StepVoice GRASS_V { 0.24f, 110.f, 60.f, 0.14f, 0.52f, 15.f, 0.62f, 0.004f, 0.70f, 0.f  };
    // Камень — самый ЗВОНКИЙ, а не самый низкий. Пока удар глушил
    // собой шорох, камень по спектру выходил глуше травы: подошва по
    // камню щёлкает, а щелчок — это верх, а не бас.
    const StepVoice STONE_V { 0.17f, 165.f, 70.f, 0.34f, 0.88f, 26.f, 0.72f, 0.001f, 0.08f, 0.40f };
    const StepVoice WOOD_V  { 0.22f, 128.f, 34.f, 0.68f, 0.40f, 22.f, 0.34f, 0.006f, 0.15f, 0.55f };
    const StepVoice SAND_V  { 0.26f,  80.f, 55.f, 0.10f, 0.34f, 12.f, 0.66f, 0.003f, 0.85f, 0.f  };
    const StepVoice WATER_V { 0.30f,  70.f, 30.f, 0.42f, 0.62f,  9.f, 0.72f, 0.010f, 0.45f, 0.f  };

    genFootstepSet(sounds_[SOUND_FOOTSTEP_DIRT],  sampleRate, DIRT_V,  0x1001, 0.60f);
    genFootstepSet(sounds_[SOUND_FOOTSTEP_GRASS], sampleRate, GRASS_V, 0x1002, 0.52f);
    genFootstepSet(sounds_[SOUND_FOOTSTEP_STONE], sampleRate, STONE_V, 0x1003, 0.70f);
    genFootstepSet(sounds_[SOUND_FOOTSTEP_WOOD],  sampleRate, WOOD_V,  0x1004, 0.62f);
    genFootstepSet(sounds_[SOUND_FOOTSTEP_SAND],  sampleRate, SAND_V,  0x1005, 0.46f);
    genFootstepSet(sounds_[SOUND_FOOTSTEP_WATER], sampleRate, WATER_V, 0x1006, 0.58f);

    // ---- Дождь ----
    genRain(sounds_[SOUND_RAIN], sampleRate);

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

    // --- Музыка (ТЗ 7) ---
    // Музыка синтезируется на своей частоте, вчетверо ниже потока:
    // у синусного пэда выше шести килогерц нет ничего, а
    // шестнадцать дорожек на полной частоте — это полсотни
    // мегабайт. Разницу доигрывает микшер.
    genExploreMusic(sounds_[SOUND_MUSIC_EXPLORE], MUSIC_RATE);
    genCombatMusic (sounds_[SOUND_MUSIC_COMBAT],  MUSIC_RATE);
    genDungeonMusic(sounds_[SOUND_MUSIC_DUNGEON], MUSIC_RATE);

    // По дорожке на биом и на уклад деревни.
    {
        const PlaceMusic* bt = biomeMusicTable();
        for (u32 i = 0; i < MUSIC_BIOME_COUNT; ++i)
            genPlaceMusic(sounds_[(u32)SOUND_MUSIC_BIOME_FIRST + i],
                          MUSIC_RATE, bt[i]);
        const PlaceMusic* vt = villageMusicTable();
        for (u32 i = 0; i < MUSIC_VILLAGE_COUNT; ++i)
            genPlaceMusic(sounds_[(u32)SOUND_MUSIC_VILLAGE_FIRST + i],
                          MUSIC_RATE, vt[i]);
    }

    initialized_ = true;
    LOGI("SoundRegistry: готово");
}

const Sound& SoundRegistry::get(SoundId id) const {
    static Sound empty;
    if (id >= SOUND_COUNT) return empty;
    return sounds_[id];
}

} // namespace audio
