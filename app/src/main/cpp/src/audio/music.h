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

    /// Биом под игроком, world::BiomeId. Вне диапазона — «неизвестно»,
    /// тогда играет общий трек исследования.
    u32  biome             = 0xFFFFFFFFu;
    /// Уклад деревни, world::VillageStyle. Читается только когда
    /// inVillage; вне диапазона — хлебная.
    u32  villageStyle      = 0xFFFFFFFFu;
};

/// MusicDirector — управляет музыкой (ТЗ 7).
///
/// Устройство: один постоянный боевой слой и ДВА слота места.
///
/// Раньше все треки крутились одновременно на нулевой громкости, и
/// переключение было сменой их уровней. С четырьмя дорожками это
/// работало, но у каждого биома и каждого уклада деревни теперь своя
/// музыка — шестнадцать дорожек разом не держат ни память, ни
/// микшер. Поэтому место играет в одном из двух слотов: новый трек
/// запускается на свободном с нулевой громкостью, старый угасает, и
/// переход всё так же не рвётся.
///
/// Место меняется НЕ СРАЗУ. На границе биомов игрок переступает её
/// туда-обратно десяток раз за минуту, и музыка, честно следующая за
/// биомом, дёргалась бы вместе с ним. Новое место должно продержаться
/// PLACE_HOLD_SEC, прежде чем зазвучит.
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

    /// Что за место сейчас звучит. Спрашивают снаружи: иначе о
    /// выдержке и о смене трека можно судить только на слух.
    SoundId placeTrack() const { return place_[cur_].id; }
    /// Какое место ждёт своей очереди (и сколько уже ждёт).
    SoundId pendingTrack() const { return pending_; }
    f32     pendingHeld()  const { return pendingTime_; }
    /// Идёт ли сейчас кроссфейд между слотами.
    bool    crossfading()  const { return place_[1 - cur_].gain > 0.f; }
    /// Уровни слотов: ведущего и угасающего. Спрашивают снаружи,
    /// потому что «переход плавный» иначе приходится слушать.
    f32     placeGain()    const { return place_[cur_].gain; }
    f32     fadingGain()   const { return place_[1 - cur_].gain; }

    /// Какой трек полагается этому контексту — без всякой выдержки.
    /// Отдельная функция, потому что правило «где что играет» и
    /// правило «когда переключаться» — разные правила.
    static SoundId trackFor(const MusicContext& ctx);

    /// Сколько должно продержаться новое место, прежде чем зазвучит.
    static constexpr f32 PLACE_HOLD_SEC = 6.f;
    /// Длительность перехода между местами.
    static constexpr f32 PLACE_FADE_SEC = 3.5f;

private:
    /// Плавно ведёт текущий уровень к целевому.
    static void approach(f32& value, f32 target, f32 rate, f32 dt);

    struct PlaceSlot {
        VoiceHandle voice;
        SoundId     id   = SOUND_NONE;
        f32         gain = 0.f;
    };

    AudioEngine* engine_ = nullptr;

    /// Боевой слой играет всегда: его громкость и есть напряжение.
    VoiceHandle combat_;
    f32         combatLevel_ = 0.f;

    PlaceSlot place_[2];
    u32       cur_ = 0;          ///< какой слот сейчас ведущий

    SoundId pending_     = SOUND_NONE;
    f32     pendingTime_ = 0.f;

    f32 tension_       = 0.f;
    f32 targetTension_ = 0.f;
    f32 pauseGain_     = 1.f;

    bool paused_ = false;

    /// Время атаки/релаксации кроссфейда напряжения.
    static constexpr f32 ATTACK_TIME  = 0.8f;
    static constexpr f32 RELEASE_TIME = 2.5f;
    /// Общий потолок громкости музыки.
    static constexpr f32 MUSIC_LEVEL  = 0.8f;
};

} // namespace audio
