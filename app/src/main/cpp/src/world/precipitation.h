/**
 * @file precipitation.h
 * @brief Мир: частицы дождя и снега вокруг игрока.
 */
#pragma once
#include "../core/types.h"
#include "chunk_manager.h"
#include <glm/glm.hpp>
#include <vector>

namespace world {

/// Одна капля или снежинка.
struct Drop {
    glm::vec3 pos{0.f};
    glm::vec3 vel{0.f};
    /// Ниже этой высоты капля кончилась: там земля, крыша или листва.
    ///
    /// Считается ОДИН раз при рождении, сканированием колонки вниз.
    /// Проверять столкновение каждый кадр значило бы тысячу с лишним
    /// чтений вокселей в кадр ради ответа, который не меняется.
    f32  killY = 0.f;
    /// Фаза качания. У снега — то, из-за чего он кружит, а не падает
    /// отвесно; у дождя не используется.
    f32  sway = 0.f;
    bool alive = false;
};

/// Осадки вокруг игрока.
///
/// Частицы живут ТОЛЬКО вокруг камеры: дождь над всем миром не нужен
/// никому — его не видно дальше двух десятков блоков, а считать
/// пришлось бы всё.
class Precipitation {
public:
    /// Потолок числа частиц.
    ///
    /// Полторы тысячи кубиков — это полторы тысячи инстансов, один
    /// вызов рисования и около сотни килобайт на кадр. Больше на
    /// телефоне не нужно: в ливень они всё равно сливаются в сетку.
    static constexpr u32 MAX_DROPS = 1400;

    /// Радиус области, в которой идут осадки, в блоках.
    static constexpr f32 RADIUS = 18.f;

    /// На сколько выше глаз они зарождаются.
    static constexpr f32 SPAWN_ABOVE = 14.f;

    /// Насколько глубоко под глазами капля считается кончившейся,
    /// если под ней так ничего и не нашлось.
    static constexpr f32 FALL_DEPTH = 26.f;

    void reset();

    /// intensity — 0..1 сила осадков, snowMix — 0 дождь, 1 снег.
    void update(ChunkManager& world, const glm::vec3& eye,
                f32 intensity, f32 snowMix, const glm::vec2& wind, f32 dt);

    const std::vector<Drop>& drops() const { return drops_; }
    u32 liveCount() const { return live_; }

    /// Сколько частиц должно быть при такой силе осадков.
    ///
    /// Снега меньше, чем дождя, и это не экономия: снежинка крупная и
    /// падает медленно, поэтому в воздухе их разом видно больше при
    /// том же числе.
    static u32 targetCount(f32 intensity, f32 snowMix);

private:
    /// Возродить каплю в новом месте. false — рождать негде
    /// (под крышей, в пещере, под водой).
    bool respawn(VoxelReader& vr, Drop& d, const glm::vec3& eye, bool snow);

    f32 frand();

    std::vector<Drop> drops_;
    u32 live_ = 0;
    u32 rng_  = 0x9E3779B9u;
};

} // namespace world
