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

/// Голос микшера.
///
/// Поля делятся на три группы, и путать их нельзя:
///   * пишутся только под заявкой (между Claimed и Active) — обычные,
///     их читает потом только звуковой поток;
///   * пишутся игровым потоком на живом голосе — обязаны быть
///     атомарными, иначе это гонка с микшером;
///   * пишутся только микшером — обычные.
///
/// panL/panR попадали во вторую группу, но атомарными не были:
/// applySpatial пишет их из игрового потока, а mixInto читает из
/// звукового.
struct Voice {
    std::atomic<u32> state{ VoiceState_Free };

    SoundId       soundId      = SOUND_NONE;
    const Sound*  sound        = nullptr;
    /// Положение в источнике, 16.16 долей сэмпла. Только микшер.
    u64           cursor       = 0;
    /// Громкость, которую заказал владелец голоса. Игровой поток
    /// меняет её через setVoiceGain, update() домножает на громкость
    /// категории и кладёт результат в liveGain.
    std::atomic<f32> baseGain{ 1.f };
    std::atomic<f32> liveGain{ 1.f };
    std::atomic<f32> atten{ 1.f };
    std::atomic<f32> panL{ 1.f };
    std::atomic<f32> panR{ 1.f };
    bool          looping      = false;
    bool          spatialized  = false;
    bool          randomizedStart = false;

    glm::vec3     position{0.f};   ///< только игровой поток

    f32           fadeStart    = 1.f;
    f32           fadeTime     = 0.f;
    f32           fadeDur      = 0.f;

    /// Растёт при каждом освобождении слота. По ней отличается живой
    /// голос от давно закончившегося, чей слот уже занят другим
    /// звуком. Упорядочена освобождением/заявкой слота: микшер
    /// увеличивает её ДО перевода в Free (release), заявка читает
    /// после успешного CAS (acquire).
    std::atomic<u32> generation{ 0 };
};

class AudioEngine {
public:
    static constexpr i32 MAX_VOICES = 64;

    /// Дробная часть курсора воспроизведения: 16 бит.
    ///
    /// Нужна затем, что частота звука и частота потока не обязаны
    /// совпадать. Музыка синтезируется на MUSIC_RATE — у синусного
    /// пэда выше шести килогерц нет ничего, а шестнадцать дорожек на
    /// полной частоте стоили бы полсотни мегабайт.
    static constexpr u32 FIXED_SHIFT = 16;
    static constexpr u32 FIXED_ONE   = 1u << FIXED_SHIFT;

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

    /// Из обратного вызова ошибки AAudio (его поток). Поток, отключённый
    /// системой (сняли наушники, отвалился Bluetooth), больше не играет,
    /// и закрывать его из этого обратного вызова нельзя — только
    /// пометить, а переоткрыть в update() на игровом потоке.
    void onStreamDisconnected() {
        disconnected_.store(true, std::memory_order_release);
    }

    u32 activeVoiceCount() const;
    u32 sampleRate() const { return sampleRate_; }

private:
    i32 claimVoice();
    /// Освобождает слот, увеличив поколение. Единственный способ
    /// перевести голос в Free: без роста поколения старые дескрипторы
    /// продолжали бы управлять чужим звуком.
    void freeVoice(i32 idx);
    /// Голос по дескриптору — или nullptr, если дескриптор устарел.
    Voice* resolve(VoiceHandle h);
    const Voice* resolve(VoiceHandle h) const;
    void applySpatial(Voice& v, const AudioListener& L);
    bool openStream();
    void closeStream();

    AAudioStream* stream_ = nullptr;
    u32 sampleRate_ = 48000;
    u32 requestedRate_ = 48000;
    /// Между init() и shutdown(): звук нужен, даже если поток сейчас
    /// потерян и переоткрыть его пока не вышло.
    bool wantStream_ = false;
    f32  reopenIn_ = 0.f;          ///< сек до следующей попытки
    std::atomic<bool> disconnected_{ false };

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
