/**
 * @file fire.cpp
 * @brief Мир: огонь на поверхностях и на телах — пламя магии огня.
 */
#include "fire.h"
#include "block.h"
#include "particles.h"
#include "../combat/components.h"
#include "../combat/damage.h"
#include "../combat/status_effects.h"
#include "../core/math.h"
#include "../ecs/components.h"
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <vector>

namespace world {

namespace {

/// Цвета пламени: жёлтая сердцевина и рыжий край. Горсть из двух
/// оттенков читается огнём, из одного — оранжевой заливкой.
constexpr u32 FLAME_CORE = 0xFFD84AFFu;
constexpr u32 FLAME_EDGE = 0xFF6A1EFFu;
constexpr u32 SMOKE      = 0x4A4642B4u;
constexpr u32 STEAM      = 0xE6ECF0C8u;

bool burnable(u16 block) {
    if (block == UNKNOWN) return false;
    const BlockDef& d = blocks().get(block);
    return !d.isSolid && !d.isLiquid;
}

bool isWater(u16 block) {
    return block != UNKNOWN && blocks().get(block).isLiquid && block != LAVA;
}

void steamPuff(const glm::vec3& at) {
    Burst b;
    b.origin  = at;
    b.dir     = glm::vec3(0.f, 1.f, 0.f);
    b.count   = 6;
    b.speed   = 1.6f;
    b.spread  = 0.5f;
    b.size    = 0.14f;
    b.life    = 0.8f;
    b.gravity = -2.5f;
    b.color   = STEAM;
    particles().emit(b);
}

} // namespace

FireField& fires() {
    static FireField f;
    return f;
}

bool FireField::flammable(u16 block) {
    switch (block) {
        case GRASS: case DRY_GRASS: case WOOD: case LEAVES: case PLANK: case THATCH:
        // Ствол дерева: сгорает целиком — ChunkManager::setVoxel роняет
        // весь столб, и модель дерева уходит вместе с ним.
        case TRUNK:
            return true;
        default:
            return false;
    }
}

void FireField::reset() {
    spots_.clear();
    glows_.clear();
    pending_.clear();
}

f32 FireField::frand() {
    return math::frand01(rng_);
}

u32 FireField::liveCount() const {
    u32 n = 0;
    for (const FireSpot& s : spots_) n += s.alive ? 1u : 0u;
    return n;
}

FireSpot* FireField::find(const glm::ivec3& cell) {
    for (FireSpot& s : spots_)
        if (s.alive && s.cell == cell) return &s;
    return nullptr;
}

void FireField::addGlow(const glm::vec3& pos, f32 power, f32 radius) {
    pending_.push_back({ pos, power, radius });
}

bool FireField::ignite(ChunkManager& world, const glm::ivec3& base,
                       const glm::ivec3& cell, u32 owner, u32 faction, u8 gen)
{
    // Гореть можно только на грани: клетка — сосед блока по оси.
    const glm::ivec3 n = cell - base;
    if (std::abs(n.x) + std::abs(n.y) + std::abs(n.z) != 1) return false;

    const u16 baseBlock = world.getVoxel(base.x, base.y, base.z);
    if (baseBlock == UNKNOWN || !blocks().get(baseBlock).isSolid) return false;
    const u16 cellBlock = world.getVoxel(cell.x, cell.y, cell.z);
    if (!burnable(cellBlock)) return false;

    const bool fuel = flammable(baseBlock);
    const f32 burn = (fuel ? BURN_FUEL : BURN_BARE) * (0.8f + frand() * 0.4f);

    if (FireSpot* s = find(cell)) {
        // Подпитать: струя, которую держат на одном месте, не должна
        // гаснуть под собственным огнём.
        s->life = std::max(s->life, burn);
        s->lifeTime = std::max(s->lifeTime, s->life);
        s->owner = owner;
        s->faction = faction;
        return true;
    }

    if (spots_.capacity() < MAX) spots_.reserve(MAX);
    FireSpot* slot = nullptr;
    for (FireSpot& s : spots_)
        if (!s.alive) { slot = &s; break; }
    if (!slot) {
        if (spots_.size() < MAX) {
            spots_.emplace_back();
            slot = &spots_.back();
        } else {
            slot = &*std::min_element(spots_.begin(), spots_.end(),
                [](const FireSpot& a, const FireSpot& b) { return a.life < b.life; });
        }
    }

    FireSpot s;
    s.cell = cell;
    s.base = base;
    s.life = s.lifeTime = burn;
    s.spread = SPREAD_EVERY * (0.5f + frand());
    s.hurt = 0.f;
    s.owner = owner;
    s.faction = faction;
    s.gen = gen;
    s.fuel = fuel;
    // Небо над пламенем: дюжина клеток вверх без твёрдого. Один раз,
    // при поджоге, — крыша над пятном за его жизнь не появится.
    s.open = true;
    for (i32 dy = 1; dy <= 12; ++dy) {
        const u16 b = world.getVoxel(cell.x, cell.y + dy, cell.z);
        if (b != UNKNOWN && blocks().get(b).isSolid) { s.open = false; break; }
    }
    s.alive = true;
    *slot = s;
    return true;
}

void FireField::emitFlame(const glm::vec3& at, const glm::vec3& up, f32 size) {
    Burst b;
    b.origin  = at;
    b.dir     = up;
    b.count   = 1;
    b.speed   = 1.3f;
    b.spread  = 0.22f;
    b.size    = size;
    b.life    = 0.55f;
    b.gravity = -3.5f;   // пламя поднимается, а не падает
    b.color   = FLAME_CORE;
    b.color2  = FLAME_EDGE;
    particles().emit(b);
}

void FireField::update(ChunkManager& world, ecs::Registry& reg, f32 rain, f32 dt) {
    glows_.swap(pending_);
    pending_.clear();
    // Место под все пятна — заранее: перекинувшийся огонь дописывает
    // пятно посреди обхода, и перекладка вектора в этот момент
    // оставила бы обход со ссылкой в никуда.
    if (spots_.capacity() < MAX) spots_.reserve(MAX);

    // Кто стоит в огне — собирается один раз на кадр и только если
    // пора кого-то обжечь: перебирать всех существ ради каждого из
    // сорока пятен каждый кадр незачем.
    struct Body { ecs::Entity e; glm::vec3 lo, hi; u32 faction; };
    std::vector<Body> bodies;
    bool bodiesReady = false;
    auto gatherBodies = [&]() {
        if (bodiesReady) return;
        bodiesReady = true;
        auto& hp = reg.pool<ecs::Health>();
        for (usize i = 0; i < hp.size(); ++i) {
            const ecs::Entity e = hp.entityAt((u32)i);
            const auto* h = hp.get(e);
            const auto* tf = reg.get<ecs::Transform>(e);
            if (!h || !tf || h->current <= 0.f) continue;
            const auto* col = reg.get<ecs::Collider>(e);
            const glm::vec3 half = col ? col->halfExtents : glm::vec3(0.3f, 0.9f, 0.3f);
            bodies.push_back({ e,
                               tf->position - glm::vec3(half.x, 0.f, half.z),
                               tf->position + glm::vec3(half.x, half.y * 2.f, half.z),
                               combat::Faction::of(reg, e) });
        }
    };

    for (FireSpot& s : spots_) {
        if (!s.alive) continue;

        // Дождь гасит открытое пламя: вчетверо быстрее в ливень.
        const f32 wet = s.open ? rain * 3.f : 0.f;
        s.life -= dt * (1.f + wet);
        if (s.life <= 0.f) { s.alive = false; continue; }

        const glm::vec3 n = glm::vec3(s.cell - s.base);
        // Центр горящей грани.
        const glm::vec3 face = glm::vec3(s.base) + glm::vec3(0.5f) + n * 0.5f;
        const f32 fade = std::min(1.f, s.life / std::max(0.01f, s.lifeTime * 0.3f));

        // ---- Пламя ----
        s.emit += dt * (s.fuel ? 9.f : 6.f) * fade;
        while (s.emit >= 1.f) {
            s.emit -= 1.f;
            // Случайная точка на грани, чуть над ней.
            glm::vec3 off{ frand() - 0.5f, frand() - 0.5f, frand() - 0.5f };
            off *= 0.9f;
            off -= n * glm::dot(off, n);
            const glm::vec3 at = face + off + n * 0.05f;
            // Пламя на стене лижет её вверх, на полу — просто вверх.
            const glm::vec3 up = glm::normalize(glm::vec3(0.f, 1.f, 0.f) + n * 0.35f);
            emitFlame(at, up, 0.10f + frand() * 0.05f);
            // Горючее дымит.
            if (s.fuel && frand() < 0.12f) {
                Burst b;
                b.origin  = at + glm::vec3(0.f, 0.3f, 0.f);
                b.dir     = glm::vec3(0.f, 1.f, 0.f);
                b.count   = 1;
                b.speed   = 0.9f;
                b.spread  = 0.3f;
                b.size    = 0.16f;
                b.life    = 1.2f;
                b.gravity = -1.6f;
                b.color   = SMOKE;
                particles().emit(b);
            }
        }

        glows_.push_back({ face + n * 0.4f + glm::vec3(0.f, 0.3f, 0.f),
                           (s.fuel ? 0.55f : 0.4f) * fade,
                           s.fuel ? 7.f : 5.f });

        // ---- Ожог стоящим в пламени ----
        s.hurt -= dt;
        if (s.hurt <= 0.f) {
            s.hurt = HURT_EVERY;

            // Вода, пролитая в огонь, его гасит.
            if (isWater(world.getVoxel(s.cell.x, s.cell.y, s.cell.z))) {
                steamPuff(face);
                s.alive = false;
                continue;
            }

            gatherBodies();
            const glm::vec3 lo = glm::vec3(s.cell);
            const glm::vec3 hi = lo + glm::vec3(1.f);
            for (const Body& b : bodies) {
                if ((u32)b.e == s.owner) continue;
                // Своё пламя своих не жжёт, а жителей не жжёт
                // перекинувшийся пожар: убийство деревни из-за
                // огня, ушедшего по траве, игрок бы себе не выбрал.
                if (!combat::Faction::hostileTo(s.faction, b.faction)) continue;
                if (b.faction == combat::Faction::NPC) continue;
                if (b.hi.x < lo.x || b.lo.x > hi.x ||
                    b.hi.y < lo.y || b.lo.y > hi.y ||
                    b.hi.z < lo.z || b.lo.z > hi.z) continue;
                combat::DamageInstance d{};
                d.amount       = HURT_DAMAGE;
                d.type         = combat::DamageType::Fire;
                d.burnTime     = HURT_BURN_TIME;
                d.burnDps      = HURT_BURN_DPS;
                d.sourceEntity = s.owner;
                d.targetEntity = (u32)b.e;
                d.sourceName   = "Flame";
                combat::applyDamage(reg, b.e, d);
            }
        }

        // ---- Перекинуться по сухому ----
        if (s.fuel && s.gen < MAX_GEN) {
            s.spread -= dt;
            if (s.spread <= 0.f) {
                s.spread = SPREAD_EVERY * (0.7f + frand() * 0.6f);
                if (frand() < SPREAD_CHANCE) {
                    // Соседний блок из 26, грань — первая свободная.
                    const glm::ivec3 d{ (i32)(frand() * 3.f) - 1,
                                        (i32)(frand() * 3.f) - 1,
                                        (i32)(frand() * 3.f) - 1 };
                    const glm::ivec3 nb = s.base + d;
                    if (d != glm::ivec3(0) &&
                        flammable(world.getVoxel(nb.x, nb.y, nb.z)))
                    {
                        static const glm::ivec3 FACES[6] = {
                            { 0, 1, 0 }, { 1, 0, 0 }, { -1, 0, 0 },
                            { 0, 0, 1 }, { 0, 0, -1 }, { 0, -1, 0 },
                        };
                        const u32 first = (u32)(frand() * 6.f) % 6u;
                        const u32 owner = s.owner, faction = s.faction;
                        const u8 gen = (u8)(s.gen + 1);
                        for (u32 k = 0; k < 6; ++k) {
                            const glm::ivec3 c = nb + FACES[(first + k) % 6u];
                            // Новое пятно может занять место самого
                            // догоревшего, в том числе этого: после
                            // вызова `s` уже не читаем.
                            if (burnable(world.getVoxel(c.x, c.y, c.z))) {
                                ignite(world, nb, c, owner, faction, gen);
                                break;
                            }
                        }
                    }
                }
            }
        }
    }

    burnBodies(world, reg, dt);
}

void FireField::burnBodies(ChunkManager& world, ecs::Registry& reg, f32 dt) {
    auto& pool = reg.pool<combat::StatusEffects>();
    for (usize i = 0; i < pool.size(); ++i) {
        const ecs::Entity e = pool.entityAt((u32)i);
        auto* se = pool.get(e);
        if (!se || se->burnTime <= 0.f) continue;
        const auto* tf = reg.get<ecs::Transform>(e);
        if (!tf) continue;

        const auto* col = reg.get<ecs::Collider>(e);
        const glm::vec3 half = col ? col->halfExtents : glm::vec3(0.3f, 0.9f, 0.3f);
        const glm::vec3 mid = tf->position + glm::vec3(0.f, half.y, 0.f);

        // Нырнул — погас.
        const u16 at = world.getVoxel((i32)std::floor(mid.x), (i32)std::floor(mid.y),
                                      (i32)std::floor(mid.z));
        if (isWater(at)) {
            se->burnTime = 0.f;
            se->burnDps = 0.f;
            steamPuff(mid);
            continue;
        }

        // Пламя по всему телу: больше тело — больше огня.
        const f32 area = std::max(0.3f, half.x * half.y * 4.f);
        const f32 rate = 10.f + 12.f * area;
        const f32 n = rate * dt;
        u32 count = (u32)n;
        if (frand() < n - (f32)count) ++count;
        for (u32 k = 0; k < count; ++k) {
            const glm::vec3 at3 = tf->position +
                glm::vec3((frand() * 2.f - 1.f) * half.x,
                          frand() * half.y * 2.f,
                          (frand() * 2.f - 1.f) * half.z);
            emitFlame(at3, glm::vec3(0.f, 1.f, 0.f), 0.08f + frand() * 0.05f);
        }
        glows_.push_back({ mid, 0.5f, 6.f });
    }
}

} // namespace world
