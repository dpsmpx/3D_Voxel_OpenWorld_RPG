/**
 * @file audio_engine.cpp
 * @brief Звук: движок AAudio, процедурные эффекты, динамическая музыка.
 */
#include "audio_engine.h"
#include "sound_registry.h"
#include "../core/log.h"

#include <aaudio/AAudio.h>
#include <dlfcn.h>
#include <atomic>
#include <cmath>
#include <cstring>
#include <algorithm>

namespace audio {

AudioEngine& engine() {
    static AudioEngine e;
    return e;
}

namespace {

// AAudio появился в API 26, а минимальная версия по ТЗ — 24 (Android 7.0).
// При обычной линковке с libaaudio.so приложение на Android 7 не
// запустилось бы вовсе: динамический компоновщик не нашёл бы библиотеку
// ещё до первой строки кода. Поэтому символы берутся в рантайме — на
// Android 7 игра работает молча, на 8.0 и новее со звуком.
struct AAudioApi {
    aaudio_result_t (*createStreamBuilder)(AAudioStreamBuilder**);
    void (*setDirection)(AAudioStreamBuilder*, aaudio_direction_t);
    void (*setPerformanceMode)(AAudioStreamBuilder*, aaudio_performance_mode_t);
    void (*setFormat)(AAudioStreamBuilder*, aaudio_format_t);
    void (*setChannelCount)(AAudioStreamBuilder*, int32_t);
    void (*setSampleRate)(AAudioStreamBuilder*, int32_t);
    void (*setDataCallback)(AAudioStreamBuilder*, AAudioStream_dataCallback, void*);
    void (*setErrorCallback)(AAudioStreamBuilder*, AAudioStream_errorCallback, void*);
    aaudio_result_t (*openStream)(AAudioStreamBuilder*, AAudioStream**);
    aaudio_result_t (*deleteBuilder)(AAudioStreamBuilder*);
    aaudio_result_t (*requestStart)(AAudioStream*);
    aaudio_result_t (*requestStop)(AAudioStream*);
    aaudio_result_t (*closeStream)(AAudioStream*);
    int32_t (*getSampleRate)(AAudioStream*);
    int32_t (*getChannelCount)(AAudioStream*);
    int32_t (*getFramesPerBurst)(AAudioStream*);
};

/// nullptr, если AAudio на этом устройстве нет (Android 7.0 и 7.1).
const AAudioApi* aaudioApi() {
    static AAudioApi api{};
    static const AAudioApi* result = [&]() -> const AAudioApi* {
        void* lib = dlopen("libaaudio.so", RTLD_NOW | RTLD_LOCAL);
        if (!lib) {
            LOGI("AudioEngine: libaaudio нет (нужен Android 8.0+) — играем без звука");
            return nullptr;
        }
        bool ok = true;
        auto sym = [&](const char* name) -> void* {
            void* p = dlsym(lib, name);
            if (!p) {
                LOGE("AudioEngine: в libaaudio нет символа %s", name);
                ok = false;
            }
            return p;
        };
        api.createStreamBuilder = reinterpret_cast<decltype(api.createStreamBuilder)>(
            sym("AAudio_createStreamBuilder"));
        api.setDirection = reinterpret_cast<decltype(api.setDirection)>(
            sym("AAudioStreamBuilder_setDirection"));
        api.setPerformanceMode = reinterpret_cast<decltype(api.setPerformanceMode)>(
            sym("AAudioStreamBuilder_setPerformanceMode"));
        api.setFormat = reinterpret_cast<decltype(api.setFormat)>(
            sym("AAudioStreamBuilder_setFormat"));
        api.setChannelCount = reinterpret_cast<decltype(api.setChannelCount)>(
            sym("AAudioStreamBuilder_setChannelCount"));
        api.setSampleRate = reinterpret_cast<decltype(api.setSampleRate)>(
            sym("AAudioStreamBuilder_setSampleRate"));
        api.setDataCallback = reinterpret_cast<decltype(api.setDataCallback)>(
            sym("AAudioStreamBuilder_setDataCallback"));
        api.setErrorCallback = reinterpret_cast<decltype(api.setErrorCallback)>(
            sym("AAudioStreamBuilder_setErrorCallback"));
        api.openStream = reinterpret_cast<decltype(api.openStream)>(
            sym("AAudioStreamBuilder_openStream"));
        api.deleteBuilder = reinterpret_cast<decltype(api.deleteBuilder)>(
            sym("AAudioStreamBuilder_delete"));
        api.requestStart = reinterpret_cast<decltype(api.requestStart)>(
            sym("AAudioStream_requestStart"));
        api.requestStop = reinterpret_cast<decltype(api.requestStop)>(
            sym("AAudioStream_requestStop"));
        api.closeStream = reinterpret_cast<decltype(api.closeStream)>(
            sym("AAudioStream_close"));
        api.getSampleRate = reinterpret_cast<decltype(api.getSampleRate)>(
            sym("AAudioStream_getSampleRate"));
        api.getChannelCount = reinterpret_cast<decltype(api.getChannelCount)>(
            sym("AAudioStream_getChannelCount"));
        api.getFramesPerBurst = reinterpret_cast<decltype(api.getFramesPerBurst)>(
            sym("AAudioStream_getFramesPerBurst"));

        if (!ok) {
            dlclose(lib);
            return nullptr;
        }
        return &api;
    }();
    return result;
}

aaudio_data_callback_result_t onAudioData(AAudioStream* /*stream*/,
                                          void* userData,
                                          void* audioData,
                                          int32_t numFrames)
{
    AudioEngine* eng = (AudioEngine*)userData;
    eng->mixInto((f32*)audioData, (u32)numFrames);
    return AAUDIO_CALLBACK_RESULT_CONTINUE;
}

void onAudioError(AAudioStream* /*stream*/,
                  void* /*userData*/,
                  aaudio_result_t error)
{
    LOGE("AAudio error: %d", (int)error);
}

} // namespace

AudioEngine::AudioEngine() {
    for (i32 i = 0; i < MAX_VOICES; ++i) {
        isMusic_[i].store(false);
    }
}

AudioEngine::~AudioEngine() {
    shutdown();
}

bool AudioEngine::init(u32 sampleRate) {
    if (stream_) return true;
    sampleRate_ = sampleRate;

    // Проверяем доступность AAudio до синтеза банка звуков: на
    // Android 7 иначе впустую тратились бы секунды и мегабайты на то,
    // что всё равно некуда играть.
    const AAudioApi* aa = aaudioApi();
    if (!aa) return false;

    SoundRegistry::instance().init(sampleRate);

    AAudioStreamBuilder* builder = nullptr;
    aaudio_result_t r = aa->createStreamBuilder(&builder);
    if (r != AAUDIO_OK) {
        LOGE("AudioEngine: AAudio_createStreamBuilder fail: %d", (int)r);
        return false;
    }

    aa->setDirection(builder, AAUDIO_DIRECTION_OUTPUT);
    aa->setPerformanceMode(builder, AAUDIO_PERFORMANCE_MODE_LOW_LATENCY);
    aa->setFormat(builder, AAUDIO_FORMAT_PCM_FLOAT);
    aa->setChannelCount(builder, 2);
    aa->setSampleRate(builder, (i32)sampleRate);
    aa->setDataCallback(builder, onAudioData, this);
    aa->setErrorCallback(builder, onAudioError, this);

    AAudioStream* stream = nullptr;
    r = aa->openStream(builder, &stream);
    aa->deleteBuilder(builder);

    if (r != AAUDIO_OK) {
        LOGE("AudioEngine: openStream fail: %d", (int)r);
        return false;
    }
    stream_ = stream;

    r = aa->requestStart(stream_);
    if (r != AAUDIO_OK) {
        LOGE("AudioEngine: requestStart fail: %d", (int)r);
        aa->closeStream(stream_);
        stream_ = nullptr;
        return false;
    }

    i32 actualRate = aa->getSampleRate(stream_);
    i32 actualChannels = aa->getChannelCount(stream_);
    i32 framesPerBurst = aa->getFramesPerBurst(stream_);

    LOGI("AudioEngine: stream open at %d Hz, %d ch, burst=%d",
         actualRate, actualChannels, framesPerBurst);

    sampleRate_ = (u32)actualRate;
    return true;
}

void AudioEngine::shutdown() {
    if (!stream_) return;
    // Поток открыт — значит, таблица символов точно загрузилась.
    const AAudioApi* aa = aaudioApi();
    if (aa) {
        aa->requestStop(stream_);
        aa->closeStream(stream_);
    }
    stream_ = nullptr;

    // Через freeVoice, а не напрямую: иначе дескрипторы, выданные до
    // остановки, остались бы действительными после повторного пуска.
    for (i32 i = 0; i < MAX_VOICES; ++i) freeVoice(i);
    LOGI("AudioEngine: остановлен");
}

i32 AudioEngine::claimVoice() {
    for (i32 i = 0; i < MAX_VOICES; ++i) {
        u32 expected = VoiceState_Free;
        if (voices_[i].state.compare_exchange_strong(
                expected, VoiceState_Claimed,
                std::memory_order_acq_rel))
        {
            return i;
        }
    }
    return -1;
}

void AudioEngine::freeVoice(i32 idx) {
    if (idx < 0 || idx >= MAX_VOICES) return;
    // Поколение растёт ДО перевода в Free: заявка на слот читает его
    // после успешного CAS, то есть уже увидит новое значение.
    voices_[idx].generation.fetch_add(1, std::memory_order_relaxed);
    voices_[idx].state.store(VoiceState_Free, std::memory_order_release);
}

Voice* AudioEngine::resolve(VoiceHandle h) {
    if (!h.valid() || h.id >= MAX_VOICES) return nullptr;
    Voice& v = voices_[h.id];
    // Поколение и было придумано для этой проверки, но её нигде не
    // делали: дескриптор давно закончившегося звука продолжал
    // управлять тем, кто занял его слот следующим.
    if (v.generation.load(std::memory_order_acquire) != h.gen) return nullptr;
    return &v;
}

const Voice* AudioEngine::resolve(VoiceHandle h) const {
    if (!h.valid() || h.id >= MAX_VOICES) return nullptr;
    const Voice& v = voices_[h.id];
    if (v.generation.load(std::memory_order_acquire) != h.gen) return nullptr;
    return &v;
}

namespace {
/// Общая часть play и play3D: они отличались только двумя полями, а
/// повторяли по два десятка строк настройки голоса.
VoiceHandle startVoice(Voice& v, i32 idx, SoundId id, const Sound& snd,
                       f32 gain, bool looping,
                       bool spatialized, const glm::vec3& pos,
                       std::atomic<bool>& isMusicFlag)
{
    v.soundId = id;
    v.sound   = &snd;
    v.cursor  = 0;
    v.baseGain.store(gain * snd.defaultGain, std::memory_order_relaxed);
    v.liveGain.store(gain * snd.defaultGain, std::memory_order_relaxed);
    v.atten.store(1.f, std::memory_order_relaxed);
    v.panL.store(1.f, std::memory_order_relaxed);
    v.panR.store(1.f, std::memory_order_relaxed);
    v.looping = looping || snd.looping;
    v.spatialized = spatialized;
    v.position = pos;
    v.fadeStart = 1.f;
    v.fadeTime  = 0.f;
    v.fadeDur   = 0.f;

    isMusicFlag.store(false, std::memory_order_relaxed);

    VoiceHandle h;
    h.id  = idx;
    h.gen = v.generation.load(std::memory_order_relaxed);

    // Публикуем последней строкой: до неё звуковой поток слот не видит.
    v.state.store(VoiceState_Active, std::memory_order_release);
    return h;
}
} // namespace

VoiceHandle AudioEngine::play(SoundId id, f32 gain, bool looping) {
    if (id == SOUND_NONE || id >= SOUND_COUNT) return {};
    const Sound& snd = SoundRegistry::instance().get(id);
    if (!snd.valid()) return {};

    const i32 idx = claimVoice();
    if (idx < 0) return {};

    return startVoice(voices_[idx], idx, id, snd, gain, looping,
                      /*spatialized=*/false, glm::vec3(0.f), isMusic_[idx]);
}

VoiceHandle AudioEngine::play3D(SoundId id, const glm::vec3& pos,
                                 f32 gain, bool looping)
{
    if (id == SOUND_NONE || id >= SOUND_COUNT) return {};
    const Sound& snd = SoundRegistry::instance().get(id);
    if (!snd.valid()) return {};

    const i32 idx = claimVoice();
    if (idx < 0) return {};

    return startVoice(voices_[idx], idx, id, snd, gain, looping,
                      /*spatialized=*/true, pos, isMusic_[idx]);
}

void AudioEngine::stop(VoiceHandle v, f32 fadeSec) {
    Voice* vo = resolve(v);
    if (!vo) return;
    if (vo->state.load(std::memory_order_acquire) != VoiceState_Active) return;

    // Параметры затухания выставляем ДО перевода в Freeing. Раньше
    // было наоборот, и звуковой поток мог увидеть Freeing с ещё не
    // записанной длительностью — от прошлого использования слота.
    // Если та оказывалась нулевой, затухание пропускалось целиком, и
    // звук обрывался вместо того чтобы угаснуть.
    vo->fadeTime  = 0.f;
    vo->fadeDur   = fadeSec > 0.f ? fadeSec : 0.f;
    vo->fadeStart = vo->liveGain.load(std::memory_order_relaxed) *
                    vo->atten.load(std::memory_order_relaxed);
    vo->state.store(VoiceState_Freeing, std::memory_order_release);
}

void AudioEngine::setVoiceGain(VoiceHandle v, f32 gain) {
    Voice* vo = resolve(v);
    if (!vo) return;
    // Пишем в baseGain — «сколько хочет владелец голоса», — а не в
    // liveGain. В liveGain кладёт update(), домножив на громкость
    // категории; писать туда же значило затирать настройку
    // громкости, и ползунок музыки не работал вовсе.
    vo->baseGain.store(gain, std::memory_order_relaxed);
}

void AudioEngine::setVoicePosition(VoiceHandle v, const glm::vec3& pos) {
    Voice* vo = resolve(v);
    if (!vo) return;
    if (vo->state.load(std::memory_order_acquire) == VoiceState_Active) {
        vo->position = pos;
    }
}

void AudioEngine::setVoiceIsMusic(VoiceHandle v, bool isMusic) {
    if (!resolve(v)) return;
    isMusic_[v.id].store(isMusic, std::memory_order_relaxed);
}

bool AudioEngine::isMusicVoice(VoiceHandle v) const {
    if (!resolve(v)) return false;
    return isMusic_[v.id].load(std::memory_order_relaxed);
}

u32 AudioEngine::activeVoiceCount() const {
    u32 n = 0;
    for (i32 i = 0; i < MAX_VOICES; ++i) {
        if (voices_[i].state.load(std::memory_order_relaxed) == VoiceState_Active)
            ++n;
    }
    return n;
}

void AudioEngine::applySpatial(Voice& v, const AudioListener& L) {
    if (!v.spatialized) return;

    glm::vec3 d = v.position - L.position;
    f32 dist = glm::length(d);
    if (dist < 0.01f) dist = 0.01f;

    constexpr f32 REF = 30.f;
    f32 atten = REF / (REF + dist * dist / 4.f);
    if (atten < 0.02f) atten = 0.02f;
    v.atten.store(atten, std::memory_order_relaxed);

    glm::vec3 dir = d / dist;
    f32 dot = glm::dot(dir, L.right);

    v.panL.store(1.f - std::max(0.f, dot)  * 0.75f, std::memory_order_relaxed);
    v.panR.store(1.f - std::max(0.f, -dot) * 0.75f, std::memory_order_relaxed);
}

void AudioEngine::update(f32 dt, const AudioListener& listener) {
    listenerCache_ = listener;
    listenerCache_.updateBasis();

    // Phase 15: применяем объёмы к liveGain без накопления.
    // liveGain = baseGain * volMult (не зависит от аттенюации — та
    // живёт в отдельном atomic atten).
    const f32 musicVol = musicVolume_.load();
    const f32 sfxVol   = sfxVolume_.load();

    for (i32 i = 0; i < MAX_VOICES; ++i) {
        Voice& v = voices_[i];
        u32 st = v.state.load(std::memory_order_acquire);
        if (st == VoiceState_Free) continue;

        if (st == VoiceState_Active) {
            applySpatial(v, listenerCache_);

            const f32 volMult = isMusic_[i].load(std::memory_order_relaxed)
                                ? musicVol : sfxVol;
            // liveGain = «сколько хочет владелец» * громкость категории.
            v.liveGain.store(v.baseGain.load(std::memory_order_relaxed) * volMult,
                             std::memory_order_relaxed);
        }
    }
    (void)dt;
}

void AudioEngine::mixInto(f32* out, u32 frames) {
    for (u32 i = 0; i < frames * 2; ++i) out[i] = 0.f;

    const f32 master = masterVolume_.load();

    for (i32 idx = 0; idx < MAX_VOICES; ++idx) {
        Voice& v = voices_[idx];
        u32 st = v.state.load(std::memory_order_acquire);
        if (st == VoiceState_Free || st == VoiceState_Claimed) continue;

        const Sound* snd = v.sound;
        if (!snd || !snd->valid()) {
            freeVoice(idx);
            continue;
        }

        f32 fadeGain = 1.f;
        if (st == VoiceState_Freeing) {
            if (v.fadeDur <= 0.f) {
                freeVoice(idx);
                continue;
            }
            f32 t = v.fadeTime / v.fadeDur;
            if (t >= 1.f) {
                freeVoice(idx);
                continue;
            }
            fadeGain = v.fadeStart * (1.f - t);
            v.fadeTime += (f32)frames / (f32)sampleRate_;
        }

        f32 live = v.liveGain.load(std::memory_order_relaxed);
        f32 atten = v.atten.load(std::memory_order_relaxed);
        f32 baseGain = live * atten * master * fadeGain;

        const f32 gL = baseGain * v.panL.load(std::memory_order_relaxed);
        const f32 gR = baseGain * v.panR.load(std::memory_order_relaxed);

        const u32 totalFrames = snd->frames;
        u64 cursor = v.cursor;
        bool loop = v.looping;

        for (u32 i = 0; i < frames; ++i) {
            if (cursor >= totalFrames) {
                if (loop) {
                    cursor = 0;
                } else {
                    freeVoice(idx);
                    break;
                }
            }
            f32 s = snd->samples[cursor];
            out[i * 2 + 0] += s * gL;
            out[i * 2 + 1] += s * gR;
            ++cursor;
        }

        // Положение в сэмплах сохраняем и для затухающего голоса.
        // Раньше — только для активного, и затухание проигрывало один
        // и тот же кусок буфера снова и снова: вместо угасания
        // получалось жужжание. Освобождённый слот не трогаем: его
        // мог уже занять другой звук и обнулить курсор.
        const u32 stNow = v.state.load(std::memory_order_relaxed);
        if (stNow == VoiceState_Active || stNow == VoiceState_Freeing) {
            v.cursor = cursor;
        }
    }

    for (u32 i = 0; i < frames * 2; ++i) {
        f32 x = out[i];
        if (x > 1.f) x = 1.f;
        if (x < -1.f) x = -1.f;
        out[i] = x;
    }
}

} // namespace audio
