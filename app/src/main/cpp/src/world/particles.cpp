/**
 * @file particles.cpp
 * @brief Мир: воксельные частицы — осколки удара, слома и приземления.
 */
#include "particles.h"
#include "block.h"
#include "../core/math.h"
#include <algorithm>
#include <cmath>

namespace world {

namespace {

constexpr f32 TAU = 6.2831853f;

/// Осветлить или притемнить цвет, не трогая прозрачность.
u32 shade(u32 rgba, f32 k) {
    const f32 r = (f32)((rgba >> 24) & 0xFFu) * k;
    const f32 g = (f32)((rgba >> 16) & 0xFFu) * k;
    const f32 b = (f32)((rgba >>  8) & 0xFFu) * k;
    const u32 a = rgba & 0xFFu;
    auto q = [](f32 v) -> u32 {
        return (u32)std::min(255.f, std::max(0.f, v));
    };
    return (q(r) << 24) | (q(g) << 16) | (q(b) << 8) | a;
}

} // namespace

Particles& particles() {
    static Particles p;
    return p;
}

void Particles::reset() {
    pool_.clear();
    cursor_ = 0;
    live_ = 0;
}

f32 Particles::frand() {
    return math::frand01(rng_);
}

Particle& Particles::allocate() {
    if (pool_.size() < MAX) {
        pool_.emplace_back();
        cursor_ = (u32)pool_.size() % MAX;
        return pool_.back();
    }
    // Сначала ищем мёртвого: затирать живого, пока есть свободное
    // место, значило бы обрывать осколки на полпути при полном пуле,
    // даже когда половина его уже догорела.
    for (u32 i = 0; i < MAX; ++i) {
        const u32 k = (cursor_ + i) % MAX;
        if (!pool_[k].alive) {
            cursor_ = (k + 1) % MAX;
            return pool_[k];
        }
    }
    Particle& p = pool_[cursor_];
    cursor_ = (cursor_ + 1) % MAX;
    return p;
}

void Particles::emit(const Burst& b) {
    if (b.count == 0 || b.life <= 0.f || b.size <= 0.f) return;

    const f32 dirLen = std::sqrt(b.dir.x * b.dir.x + b.dir.y * b.dir.y +
                                 b.dir.z * b.dir.z);
    const glm::vec3 base = (dirLen > 1e-4f) ? b.dir / dirLen
                                            : glm::vec3(0.f, 1.f, 0.f);
    const f32 spread = std::min(1.f, std::max(0.f, b.spread));

    // Место под полный пул берётся один раз: горсть рождается в
    // момент удара, и перекладывать массив тогда же — последнее, чего
    // от этого кадра хочется.
    if (pool_.capacity() < MAX) pool_.reserve(MAX);

    const u32 n = std::min(b.count, MAX);
    for (u32 i = 0; i < n; ++i) {
        // Равномерно по сфере: разброс по углам сгущает направления
        // у полюсов, и горсть получается не горстью, а двумя пучками.
        const f32 z = frand() * 2.f - 1.f;
        const f32 a = frand() * TAU;
        const f32 r = std::sqrt(std::max(0.f, 1.f - z * z));
        const glm::vec3 rnd{ r * std::cos(a), z, r * std::sin(a) };

        glm::vec3 d = base * (1.f - spread) + rnd * spread;
        const f32 dl = std::sqrt(d.x * d.x + d.y * d.y + d.z * d.z);
        d = (dl > 1e-4f) ? d / dl : base;

        Particle& p = allocate();
        if (!p.alive) ++live_;
        p.pos = b.origin;
        p.vel = d * (b.speed * (0.55f + frand() * 0.9f));
        p.size = b.size * (0.65f + frand() * 0.7f);
        p.lifeTime = b.life * (0.7f + frand() * 0.6f);
        p.life = p.lifeTime;
        p.gravity = b.gravity;
        p.restY = 0.f;
        p.spin = frand() * TAU;
        p.spinRate = (frand() * 2.f - 1.f) * 9.f;
        // Оттенок у каждого осколка свой: горсть кубов одного цвета
        // читается пятном, а не обломками.
        const u32 src = (b.color2 != 0 && frand() < 0.5f) ? b.color2 : b.color;
        p.color = shade(src, 0.86f + frand() * 0.28f);
        p.probed = false;
        p.resting = false;
        p.alive = true;
    }
}

void Particles::update(ChunkManager& world, const glm::vec3& eye, f32 dt) {
    if (pool_.empty()) { live_ = 0; return; }

    // Один курсор на кадр: осколки одной горсти стоят в пуле подряд и
    // лежат в одной колонке, поэтому чанк почти всегда уже открыт.
    VoxelReader vr(world);
    u32 probes = PROBE_BUDGET;
    u32 alive = 0;

    const f32 cullSq = CULL_RADIUS * CULL_RADIUS;

    for (Particle& p : pool_) {
        if (!p.alive) continue;

        if (!p.probed && probes > 0) {
            --probes;
            p.probed = true;
            const i32 bx = (i32)std::floor(p.pos.x);
            const i32 bz = (i32)std::floor(p.pos.z);
            const i32 top = (i32)std::floor(p.pos.y);
            const i32 bottom = std::max(0, top - GROUND_PROBE);
            p.restY = -1e9f;
            for (i32 by = std::min(CHUNK_SIZE_Y - 1, top); by >= bottom; --by) {
                if (vr.isSolid(bx, by, bz)) { p.restY = (f32)(by + 1); break; }
            }
        }

        if (!p.resting) {
            p.vel.y -= p.gravity * dt;
            // Затухание скорости, а не её обнуление: осколок должен
            // терять разгон, а не тормозить о воздух намертво.
            const f32 damp = std::max(0.f, 1.f - DRAG * dt);
            p.vel *= damp;
            p.pos += p.vel * dt;
            p.spin += p.spinRate * dt;

            if (p.probed && p.pos.y <= p.restY) {
                p.pos.y = p.restY;
                p.vel = glm::vec3(0.f);
                p.resting = true;
                // Лежать долго осколку незачем: он уже сказал всё,
                // что должен был.
                p.life = std::min(p.life, REST_LIFE);
            }
        }

        p.life -= dt;

        const f32 dx = p.pos.x - eye.x;
        const f32 dy = p.pos.y - eye.y;
        const f32 dz = p.pos.z - eye.z;
        const bool tooFar = dx * dx + dy * dy + dz * dz > cullSq;

        if (p.life <= 0.f || tooFar || p.pos.y < -8.f) {
            p.alive = false;
            continue;
        }
        ++alive;
    }

    live_ = alive;
}

// ------------------------------------------------------------
// Поводы
// ------------------------------------------------------------

void blockBreakBurst(const glm::ivec3& block, u16 blockId) {
    const BlockDef& def = blocks().get(blockId);

    Burst b{};
    b.origin = glm::vec3((f32)block.x + 0.5f, (f32)block.y + 0.5f,
                         (f32)block.z + 0.5f);
    b.dir = glm::vec3(0.f, 1.f, 0.f);
    b.count = 14;
    b.speed = 4.2f;
    b.spread = 0.95f;
    b.size = 0.13f;
    b.life = 0.75f;
    b.gravity = 22.f;
    // Цвет у щепок не свой: он ровно тот, что был у блока. Камень
    // серый потому, что камень серый.
    b.color = def.colorTop;
    b.color2 = def.colorSide;
    particles().emit(b);
}

void landingBurst(const glm::vec3& feet, u16 groundBlock, f32 fallSpeed) {
    // Пыль поднимает только ощутимое падение. Шаг со ступеньки её не
    // поднимает — иначе она идёт за игроком не переставая и перестаёт
    // что-либо значить.
    if (fallSpeed < 7.f) return;
    const BlockDef& def = blocks().get(groundBlock);
    if (!def.isSolid) return;

    const f32 k = std::min(1.f, (fallSpeed - 7.f) / 18.f);

    Burst b{};
    b.origin = feet + glm::vec3(0.f, 0.12f, 0.f);
    // В стороны, а не вверх: пыль из-под подошв расходится кольцом.
    b.dir = glm::vec3(0.f, 0.22f, 0.f);
    b.count = 6 + (u32)(k * 12.f);
    b.speed = 2.6f + k * 2.6f;
    b.spread = 1.f;
    b.size = 0.10f;
    b.life = 0.45f + k * 0.35f;
    b.gravity = 12.f;
    b.color = def.colorTop;
    b.color2 = def.colorSide;
    particles().emit(b);
}

void woundBurst(const glm::vec3& at, const glm::vec3& away,
                u32 bodyColor, f32 severity, bool critical)
{
    const f32 s = std::min(1.f, std::max(0.f, severity));

    Burst b{};
    b.origin = at;
    b.dir = away;
    // Слабый удар высекает три искры, сильный — дюжину. Раньше сила
    // удара была видна только по полоске здоровья.
    b.count = 3u + (u32)(s * 9.f);
    b.speed = 3.2f + s * 3.4f;
    b.spread = 0.55f;
    b.size = 0.085f;
    b.life = 0.42f;
    b.gravity = 17.f;
    b.color = bodyColor;

    if (critical) {
        // Крит не «сильнее», а ДРУГОЙ: вдвое гуще, быстрее и с
        // золотой искрой. До этого крит в бою нельзя было заметить
        // вовсе — он существовал только в числах.
        b.count += 6;
        b.speed += 2.5f;
        b.size *= 1.25f;
        b.color2 = 0xFFD24AFFu;
    }
    particles().emit(b);
}

void deathBurst(const glm::vec3& at, u32 bodyColor) {
    Burst b{};
    b.origin = at;
    b.dir = glm::vec3(0.f, 1.f, 0.f);
    b.count = 22;
    b.speed = 4.6f;
    b.spread = 0.85f;
    b.size = 0.15f;
    b.life = 0.85f;
    b.gravity = 19.f;
    b.color = bodyColor;
    b.color2 = shade(bodyColor, 0.62f);
    particles().emit(b);
}

} // namespace world
