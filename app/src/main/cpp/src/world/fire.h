/**
 * @file fire.h
 * @brief Мир: огонь на поверхностях и на телах — пламя магии огня.
 */
#pragma once
#include "../core/types.h"
#include "../ecs/registry.h"
#include "chunk_manager.h"
#include <glm/glm.hpp>
#include <vector>

namespace world {

/// Пятно пламени на грани блока.
///
/// Горит не блок, а магия на нём: мир при этом не меняется ни на
/// один воксель. Выгоревший лес пришлось бы хранить в сохранении
/// правками по тысяче блоков, а пожар, съевший деревню из-за промаха
/// по волку, — не то, чего хочет игрок. Огонь здесь — это частицы,
/// свет и жар: он обжигает стоящих в нём, перекидывается по сухому и
/// гаснет сам.
struct FireSpot {
    glm::ivec3 cell{0};   ///< клетка, в которой горит (не твёрдая)
    glm::ivec3 base{0};   ///< блок, на грани которого горит
    f32  life     = 0.f;
    f32  lifeTime = 0.f;
    f32  emit     = 0.f;  ///< задолженные частицы: их дробное число за кадр
    f32  spread   = 0.f;  ///< до следующей попытки перекинуться
    f32  hurt     = 0.f;  ///< до следующего ожога стоящих в пламени
    u32  owner    = 0;    ///< кто поджёг — ему и опыт за сгоревших
    u32  faction  = 0;
    u8   gen      = 0;    ///< через сколько перекидываний дошёл сюда
    bool fuel     = false;///< горит на горючем: дольше и перекидывается
    bool open     = false;///< над ним небо — дождь его гасит
    bool alive    = false;
};

/// Источник света от огня на этот кадр.
struct FireGlow {
    glm::vec3 pos{0.f};
    f32 power  = 0.f;
    f32 radius = 0.f;
};

/// Весь огонь в мире: пятна на поверхностях, горящие существа и
/// струя пламени из ладони.
class FireField {
public:
    /// Сколько пятен горит одновременно. Больше на телефоне не
    /// нужно: сорок пятен — это уже стена огня во весь экран.
    static constexpr u32 MAX = 40;
    /// Сколько горит пятно на камне и на горючем, сек.
    static constexpr f32 BURN_BARE = 3.5f;
    static constexpr f32 BURN_FUEL = 8.f;
    /// Пожар не бесконечен: пятно, дошедшее через столько
    /// перекидываний, дальше не идёт.
    static constexpr u8  MAX_GEN = 5;
    static constexpr f32 SPREAD_EVERY  = 0.9f;
    static constexpr f32 SPREAD_CHANCE = 0.45f;
    /// Ожог стоящим в пламени — раз в столько секунд.
    static constexpr f32 HURT_EVERY = 0.5f;
    static constexpr f32 HURT_DAMAGE = 3.f;
    static constexpr f32 HURT_BURN_TIME = 2.5f;
    static constexpr f32 HURT_BURN_DPS  = 4.f;

    void reset();

    /// Поджечь грань блока `base`, смотрящую в клетку `cell`.
    ///
    /// Не горит в воде и в твёрдом. Уже горящее пятно подпитывается.
    /// Когда места нет, гаснет пятно, которому и так осталось меньше
    /// всех: свежий огонь перед глазами важнее догорающего.
    /// @return загорелось ли (или подпиталось)
    bool ignite(ChunkManager& world, const glm::ivec3& base,
                const glm::ivec3& cell, u32 owner, u32 faction, u8 gen = 0);

    /// Кадр: пятна горят, светят, обжигают и перекидываются;
    /// горящие существа пылают и гаснут в воде.
    /// `rain` — сила дождя 0..1: под открытым небом он гасит пламя.
    void update(ChunkManager& world, ecs::Registry& reg, f32 rain, f32 dt);

    /// Свет струи из ладони на следующий кадр — её зовёт бой.
    void addGlow(const glm::vec3& pos, f32 power, f32 radius);

    const std::vector<FireSpot>& spots() const { return spots_; }
    const std::vector<FireGlow>& glows() const { return glows_; }
    u32 liveCount() const;

    /// Горит ли этот блок по-настоящему: трава, дерево, листва,
    /// доски, солома. На остальном пламя магии тлеет недолго.
    static bool flammable(u16 block);

private:
    FireSpot* find(const glm::ivec3& cell);
    void burnBodies(ChunkManager& world, ecs::Registry& reg, f32 dt);
    void emitFlame(const glm::vec3& at, const glm::vec3& up, f32 size);
    f32  frand();

    std::vector<FireSpot> spots_;
    std::vector<FireGlow> glows_;
    std::vector<FireGlow> pending_;
    u32 rng_ = 0x9E3779B9u;
};

/// Общий огонь мира.
///
/// Глобален по той же причине, что и пул частиц: поджигает бой — из
/// глубины расчёта удара, — а горит мир. Живёт в главном потоке.
FireField& fires();

} // namespace world
