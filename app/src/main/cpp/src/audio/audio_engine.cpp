/**
 * @file audio_engine.cpp
 * @brief Звук: движок AAudio, процедурные эффекты, динамическая музыка.
 */
#include "audio_engine.h"
#include "sound_registry.h"
#include "../core/log.h"

#include <aaudio/AAudio.h>
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

    SoundRegistry::instance().init(sampleRate);

    AAudioStreamBuilder* builder = nullptr;
    aaudio_result_t r = AAudio_createStreamBuilder(&builder);
    if (r != AAUDIO_OK) {
        LOGE("AudioEngine: AAudio_createStreamBuilder fail: %d", (int)r);
        return false;
    }

    AAudioStreamBuilder_setDirection(builder, AAUDIO_DIRECTION_OUTPUT);
    AAudioStreamBuilder_setPerformanceMode(builder,
        AAUDIO_PERFORMANCE_MODE_LOW_LATENCY);
    AAudioStreamBuilder_setFormat(builder, AAUDIO_FORMAT_PCM_FLOAT);
    AAudioStreamBuilder_setChannelCount(builder, 2);
    AAudioStreamBuilder_setSampleRate(builder, (i32)sampleRate);
    AAudioStreamBuilder_setDataCallback(builder, onAudioData, this);
    AAudioStreamBuilder_setErrorCallback(builder, onAudioError, this);

    AAudioStream* stream = nullptr;
    r = AAudioStreamBuilder_openStream(builder, &stream);
    AAudioStreamBuilder_delete(builder);

    if (r != AAUDIO_OK) {
        LOGE("AudioEngine: openStream fail: %d", (int)r);
        return false;
    }
    stream_ = stream;

    r = AAudioStream_requestStart(stream_);
    if (r != AAUDIO_OK) {
        LOGE("AudioEngine: requestStart fail: %d", (int)r);
        AAudioStream_close(stream_);
        stream_ = nullptr;
        return false;
    }

    i32 actualRate = AAudioStream_getSampleRate(stream_);
    i32 actualChannels = AAudioStream_getChannelCount(stream_);
    i32 framesPerBurst = AAudioStream_getFramesPerBurst(stream_);

    LOGI("AudioEngine: stream open at %d Hz, %d ch, burst=%d",
         actualRate, actualChannels, framesPerBurst);

    sampleRate_ = (u32)actualRate;
    return true;
}

void AudioEngine::shutdown() {
    if (!stream_) return;
    AAudioStream_requestStop(stream_);
    AAudioStream_close(stream_);
    stream_ = nullptr;

    for (auto& v : voices_) {
        v.state.store(VoiceState_Free);
    }
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

void AudioEngine::releaseVoice(i32 idx) {
    if (idx < 0 || idx >= MAX_VOICES) return;
    voices_[idx].generation++;
    voices_[idx].state.store(VoiceState_Free, std::memory_order_release);
}

VoiceHandle AudioEngine::play(SoundId id, f32 gain, bool looping) {
    if (id == SOUND_NONE || id >= SOUND_COUNT) return {};
    const Sound& snd = SoundRegistry::instance().get(id);
    if (!snd.valid()) return {};

    i32 idx = claimVoice();
    if (idx < 0) return {};

    Voice& v = voices_[idx];

    v.soundId = id;
    v.sound   = &snd;
    v.cursor  = 0;
    v.baseGain = gain * snd.defaultGain;
    v.liveGain.store(v.baseGain);
    v.atten.store(1.f);
    v.panL    = 1.f;
    v.panR    = 1.f;
    v.looping = looping || snd.looping;
    v.spatialized = false;
    v.position = glm::vec3(0.f);
    v.fadeStart = 1.f;
    v.fadeTime  = 0.f;
    v.fadeDur   = 0.f;

    isMusic_[idx].store(false);

    v.state.store(VoiceState_Active, std::memory_order_release);

    VoiceHandle h;
    h.id = idx;
    h.gen = v.generation;
    return h;
}

VoiceHandle AudioEngine::play3D(SoundId id, const glm::vec3& pos,
                                 f32 gain, bool looping)
{
    if (id == SOUND_NONE || id >= SOUND_COUNT) return {};
    const Sound& snd = SoundRegistry::instance().get(id);
    if (!snd.valid()) return {};

    i32 idx = claimVoice();
    if (idx < 0) return {};

    Voice& v = voices_[idx];

    v.soundId = id;
    v.sound   = &snd;
    v.cursor  = 0;
    v.baseGain = gain * snd.defaultGain;
    v.liveGain.store(v.baseGain);
    v.atten.store(1.f);
    v.panL    = 1.f;
    v.panR    = 1.f;
    v.looping = looping || snd.looping;
    v.spatialized = true;
    v.position = pos;
    v.fadeStart = 1.f;
    v.fadeTime  = 0.f;
    v.fadeDur   = 0.f;

    isMusic_[idx].store(false);

    v.state.store(VoiceState_Active, std::memory_order_release);

    VoiceHandle h;
    h.id = idx;
    h.gen = v.generation;
    return h;
}

void AudioEngine::stop(VoiceHandle v, f32 fadeSec) {
    if (!v.valid() || v.id >= MAX_VOICES) return;
    Voice& vo = voices_[v.id];
    if (vo.state.load() != VoiceState_Active) return;

    if (fadeSec <= 0.f) {
        vo.state.store(VoiceState_Freeing, std::memory_order_release);
        vo.fadeTime = 0.f;
        vo.fadeDur  = 0.f;
        vo.fadeStart = vo.liveGain.load() * vo.atten.load();
    } else {
        vo.state.store(VoiceState_Freeing, std::memory_order_release);
        vo.fadeTime = 0.f;
        vo.fadeDur  = fadeSec;
        vo.fadeStart = vo.liveGain.load() * vo.atten.load();
    }
}

void AudioEngine::setVoiceGain(VoiceHandle v, f32 gain) {
    if (!v.valid() || v.id >= MAX_VOICES) return;
    voices_[v.id].liveGain.store(gain, std::memory_order_relaxed);
}

void AudioEngine::setVoicePosition(VoiceHandle v, const glm::vec3& pos) {
    if (!v.valid() || v.id >= MAX_VOICES) return;
    if (voices_[v.id].state.load() == VoiceState_Active) {
        voices_[v.id].position = pos;
    }
}

void AudioEngine::setVoiceIsMusic(VoiceHandle v, bool isMusic) {
    if (!v.valid() || v.id >= MAX_VOICES) return;
    isMusic_[v.id].store(isMusic);
}

bool AudioEngine::isMusicVoice(VoiceHandle v) const {
    if (!v.valid() || v.id >= MAX_VOICES) return false;
    return isMusic_[v.id].load();
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

    v.panL = 1.f - std::max(0.f, dot)  * 0.75f;
    v.panR = 1.f - std::max(0.f, -dot) * 0.75f;
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

            f32 volMult = isMusic_[i].load() ? musicVol : sfxVol;
            // liveGain хранит baseGain * volMult (перезапись, без накопления).
            v.liveGain.store(v.baseGain * volMult, std::memory_order_relaxed);
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
            v.state.store(VoiceState_Free, std::memory_order_release);
            continue;
        }

        f32 fadeGain = 1.f;
        if (st == VoiceState_Freeing) {
            if (v.fadeDur <= 0.f) {
                v.state.store(VoiceState_Free, std::memory_order_release);
                continue;
            }
            f32 t = v.fadeTime / v.fadeDur;
            if (t >= 1.f) {
                v.state.store(VoiceState_Free, std::memory_order_release);
                continue;
            }
            fadeGain = v.fadeStart * (1.f - t);
            v.fadeTime += (f32)frames / (f32)sampleRate_;
        }

        f32 live = v.liveGain.load(std::memory_order_relaxed);
        f32 atten = v.atten.load(std::memory_order_relaxed);
        f32 baseGain = live * atten * master * fadeGain;

        f32 gL = baseGain * v.panL;
        f32 gR = baseGain * v.panR;

        const u32 totalFrames = snd->frames;
        u64 cursor = v.cursor;
        bool loop = v.looping;

        for (u32 i = 0; i < frames; ++i) {
            if (cursor >= totalFrames) {
                if (loop) {
                    cursor = 0;
                } else {
                    v.state.store(VoiceState_Free, std::memory_order_release);
                    break;
                }
            }
            f32 s = snd->samples[cursor];
            out[i * 2 + 0] += s * gL;
            out[i * 2 + 1] += s * gR;
            ++cursor;
        }

        if (v.state.load(std::memory_order_relaxed) == VoiceState_Active) {
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
