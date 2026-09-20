/**
 * @file precipitation.cpp
 * @brief Мир: частицы дождя и снега вокруг игрока.
 */
#include "precipitation.h"
#include "block.h"
#include "../core/math.h"
#include <cmath>
#include <algorithm>

namespace world {

namespace {

/// Скорость падения, блоков в секунду.
constexpr f32 RAIN_FALL = 26.f;
constexpr f32 SNOW_FALL = 2.4f;

/// Насколько ветер сносит падающее.
///
/// Снежинку — целиком, каплю — вчетверо слабее: капля тяжелее и
/// летит быстрее, ветер успевает сделать с ней меньше.
constexpr f32 RAIN_DRIFT = 0.22f;
constexpr f32 SNOW_DRIFT = 0.85f;

/// Как широко кружит снежинка и как быстро.
constexpr f32 SNOW_SWAY_AMP  = 0.9f;
constexpr f32 SNOW_SWAY_RATE = 1.7f;

/// На сколько блоков выше точки рождения ищется крыша.
///
/// Двадцать четыре: выше поднимаются только кроны и башни, а под
/// ними дождь как раз и должен идти — они не жильё.
constexpr i32 SKY_PROBE = 24;

} // namespace

u32 Precipitation::targetCount(f32 intensity, f32 snowMix) {
    const f32 k = glm::clamp(intensity, 0.f, 1.f);
    const f32 s = glm::clamp(snowMix, 0.f, 1.f);
    const f32 share = glm::mix(1.f, 0.45f, s);
    return (u32)((f32)MAX_DROPS * k * share);
}

void Precipitation::reset() {
    drops_.clear();
    live_ = 0;
}

f32 Precipitation::frand() {
    return math::frand01(rng_);
}

bool Precipitation::respawn(VoxelReader& vr, Drop& d, const glm::vec3& eye,
                            bool snow)
{
    // Равномерно по кругу, а не по квадрату: в углах квадрата капли
    // гуще, и при повороте головы это видно.
    const f32 ang = frand() * 6.2831853f;
    const f32 rad = std::sqrt(frand()) * RADIUS;
    const f32 x = eye.x + std::cos(ang) * rad;
    const f32 z = eye.z + std::sin(ang) * rad;

    // Разброс по высоте, иначе первый же кадр даст ровную плоскость
    // капель, которая поедет вниз стеной.
    const f32 y = eye.y + frand() * SPAWN_ABOVE;

    const i32 bx = (i32)std::floor(x);
    const i32 bz = (i32)std::floor(z);
    const i32 top = (i32)std::floor(y);
    const i32 bottom = std::max(0, (i32)std::floor(eye.y - FALL_DEPTH));
    const i32 scanTop = std::min(CHUNK_SIZE_Y - 1, top + SKY_PROBE);

    // Один скан сверху вниз отвечает сразу на два вопроса.
    //
    // Первое твёрдое ВЫШЕ точки рождения — значит над ней крыша, и
    // осадков здесь нет вовсе: ни в доме, ни в пещере, ни под
    // скалой. Первое твёрдое НИЖЕ — это земля (или та же крыша, если
    // капля родилась над ней), и на ней капля кончится.
    //
    // Скан начинается не от потолка мира, а на два десятка блоков
    // выше точки рождения: выше бывают только кроны и башни, а сотня
    // лишних чтений вокселя на каждую рождающуюся каплю — это
    // десятки тысяч чтений в кадр.
    i32 first = bottom - 1;
    for (i32 by = scanTop; by >= bottom; --by) {
        if (vr.isSolid(bx, by, bz)) { first = by; break; }
    }

    if (first >= top) { d.alive = false; return false; }
    const i32 hit = first;

    d.pos   = { x, y, z };
    d.killY = (f32)(hit + 1);
    d.sway  = frand() * 6.2831853f;
    d.vel   = glm::vec3(0.f, snow ? -SNOW_FALL : -RAIN_FALL, 0.f);
    d.alive = true;
    return true;
}

void Precipitation::update(ChunkManager& world, const glm::vec3& eye,
                           f32 intensity, f32 snowMix, const glm::vec2& wind,
                           f32 dt)
{
    const u32 want = targetCount(intensity, snowMix);
    if (want == 0) { reset(); return; }

    if (drops_.size() != MAX_DROPS) drops_.resize(MAX_DROPS);

    const bool snow = snowMix > 0.5f;
    const f32 drift = snow ? SNOW_DRIFT : RAIN_DRIFT;

    // Один курсор на весь кадр: он держит чанк и его замок, и
    // соседние колонки почти всегда лежат в том же чанке.
    VoxelReader vr(world);

    // Сколько капель разрешено РОДИТЬ за кадр. Без потолка первый
    // кадр ливня просканировал бы полторы тысячи колонок разом.
    u32 budget = 96;
    u32 alive = 0;

    for (u32 i = 0; i < MAX_DROPS; ++i) {
        Drop& d = drops_[i];

        if (i >= want) { d.alive = false; continue; }

        if (d.alive) {
            d.pos += d.vel * dt;
            d.pos.x += wind.x * drift * dt;
            d.pos.z += wind.y * drift * dt;

            if (snow) {
                // Снежинка кружит: отвесная сетка одинаковых точек
                // читается как помеха на экране, а не как снег.
                d.sway += SNOW_SWAY_RATE * dt;
                d.pos.x += std::cos(d.sway) * SNOW_SWAY_AMP * dt;
                d.pos.z += std::sin(d.sway * 0.8f) * SNOW_SWAY_AMP * dt;
            }

            const f32 dx = d.pos.x - eye.x;
            const f32 dz = d.pos.z - eye.z;
            const bool gone = d.pos.y <= d.killY ||
                              d.pos.y < eye.y - FALL_DEPTH ||
                              dx * dx + dz * dz > (RADIUS * 1.5f) * (RADIUS * 1.5f);
            if (gone) d.alive = false;
        }

        if (!d.alive && budget > 0) {
            --budget;
            respawn(vr, d, eye, snow);
        }
        if (d.alive) ++alive;
    }

    live_ = alive;
}

} // namespace world
