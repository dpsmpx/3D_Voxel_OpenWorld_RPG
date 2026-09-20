/**
 * @file npc_ai.cpp
 * @brief NPC: роли, диалоги с ветвлением, поведение жителей.
 */
#include "npc_ai.h"
#include "../ecs/components.h"
#include "../combat/components.h"
#include "../combat/status_effects.h"
#include "../combat/hit_detection.h"
#include "../factions/faction.h"
#include "../world/block.h"
#include "../world/ai/pathfinding.h"
#include "../physics/creature_motion.h"
#include "../combat/weapon.h"
#include "../audio/audio_events.h"
#include "../core/log.h"
#include <cmath>
#include <random>
#include <algorithm>
#include <vector>

namespace npc {

using namespace ecs;

namespace {

std::mt19937& rng() {
    static std::mt19937 g(0x11AA22BB);
    return g;
}

f32 frand(f32 lo, f32 hi) {
    return std::uniform_real_distribution<f32>(lo, hi)(rng());
}

constexpr f32 IDLE_MIN = 2.0f;
constexpr f32 IDLE_MAX = 5.0f;
constexpr f32 WANDER_RADIUS = 6.f;
physics::CreatureBody bodyOf(const NpcDef& def) {
    physics::CreatureBody b;
    b.halfWidth  = def.bodyRadius;
    b.height     = def.bodyHeight;
    b.stepHeight = 1.05f;      // ступень в блок — базовая способность
    b.jumpSpeed  = 7.0f;
    return b;
}

/// Бюджет поисков пути на кадр — тот же довод, что у мобов: один A*
/// это тысячи чтений вокселей. Жителей в деревне два десятка, и если
/// каждый раз в секунду попросит путь, кадр этого не переживёт.
i32 g_npcPathBudget = 0;

void repath(world::ChunkManager& world, NpcAI& ai,
            const glm::vec3& from, const glm::vec3& to)
{
    if (g_npcPathBudget <= 0) return;
    --g_npcPathBudget;

    world::ai::PathResult r = world::ai::findPath(
        world,
        { (i32)std::floor(from.x), (i32)std::floor(from.y), (i32)std::floor(from.z) },
        { (i32)std::floor(to.x),   (i32)std::floor(to.y),   (i32)std::floor(to.z) },
        ai.moveParams, 900);

    if (r.ok && r.waypoints.size() > 1) {
        world::ai::smoothPath(world, r.waypoints, ai.moveParams);
        ai.path = std::move(r.waypoints);
        ai.pathIndex = 1;
    } else {
        ai.path.clear();
        ai.pathIndex = 0;
    }
}

/// Направление вдоль пути. Если путь кончился или его нет — ноль, и
/// звать надо прямую.
glm::vec3 followPath(NpcAI& ai, const glm::vec3& pos) {
    if (ai.path.empty() || ai.pathIndex >= (i32)ai.path.size()) return {0,0,0};
    const glm::vec3 wp = glm::vec3(ai.path[ai.pathIndex]) + glm::vec3(0.5f, 0.f, 0.5f);
    glm::vec3 d = wp - pos;
    d.y = 0.f;
    const f32 len = glm::length(d);
    if (len < 0.5f) {
        ++ai.pathIndex;
        return {0,0,0};
    }
    return d / len;
}

/// Идти к точке: по пути, если он есть, иначе напрямую.
glm::vec3 steerTo(world::ChunkManager& world, NpcAI& ai,
                  const glm::vec3& pos, const glm::vec3& goal,
                  f32 repathEvery)
{
    if (ai.repathCooldown <= 0.f) {
        repath(world, ai, pos, goal);
        ai.repathCooldown = repathEvery;
    }
    glm::vec3 dir = followPath(ai, pos);
    if (glm::dot(dir, dir) < 1e-6f) {
        glm::vec3 d = goal - pos;
        d.y = 0.f;
        const f32 len = glm::length(d);
        if (len > 0.05f) dir = d / len;
    }
    return dir;
}

bool hasLOS(world::ChunkManager& world,
            const glm::vec3& from, const glm::vec3& to)
{
    glm::vec3 d = to - from;
    f32 dist = glm::length(d);
    if (dist < 0.001f) return true;
    d /= dist;
    auto& reg = world::blocks();
    world::VoxelReader rd(world);
    const f32 step = 0.4f;
    for (f32 t = 0.4f; t < dist; t += step) {
        glm::vec3 p = from + d * t;
        i32 x = (i32)std::floor(p.x);
        i32 y = (i32)std::floor(p.y);
        i32 z = (i32)std::floor(p.z);
        if (reg.isSolid(rd.at(x, y, z))) return false;
    }
    return true;
}

// Найти ближайшего врага (для Guard).
/// Ближайший помеченный враг — мобы деревни.
ecs::Entity findNearestEnemy(ecs::Registry& reg,
                             const glm::vec3& pos,
                             const glm::vec3& home,
                             f32 maxDist,
                             f32 homeRadius)
{
    ecs::Entity best{};
    f32 bestD2 = maxDist * maxDist;

    auto& hp = reg.pool<ecs::Health>();
    for (usize i = 0; i < hp.size(); ++i) {
        ecs::Entity e = hp.entityAt((u32)i);
        if (!reg.has<ecs::EnemyTag>(e)) continue;
        // Мёртвый враг — не враг. Труп лежит ещё полторы секунды со
        // всеми своими метками, и стража всё это время рубила его,
        // стоя над ним, вместо того чтобы взяться за следующего.
        auto* h = reg.get<ecs::Health>(e);
        if (!h || h->current <= 0.f) continue;
        auto* tf = reg.get<ecs::Transform>(e);
        if (!tf) continue;

        // Угроза ДЕРЕВНЕ, а не «что-то шевелится за горизонтом».
        // Без этого стража уходила за волком в лес и не возвращалась,
        // а деревня оставалась без защиты.
        if (homeRadius > 0.f) {
            glm::vec3 hd = tf->position - home;
            hd.y = 0.f;
            if (glm::dot(hd, hd) > homeRadius * homeRadius) continue;
        }

        glm::vec3 d = tf->position - pos;
        d.y = 0.f;
        f32 d2 = glm::dot(d, d);
        if (d2 < bestD2) {
            bestD2 = d2;
            best = e;
        }
    }
    return best;
}

/// Кого стража считает врагом.
///
/// Игрок попадает сюда, когда его репутация у фракции NPC упала до
/// враждебной. RelationModifiers::hostile («атакуют ли NPC игрока»)
/// был написан и не читался нигде: вырезав полдеревни, игрок мог
/// спокойно ходить мимо стражи.
///
/// Игрок в приоритете перед мобами: стража, отвернувшаяся от убийцы
/// ради ближайшего гоблина, выглядит сломанной.
/// Докуда защитник гонится за угрозой от своего поста.
///
/// Деревню защищают в деревне. Загнав волка за околицу, стражник
/// поворачивает обратно: иначе одна забредшая тварь уводит весь
/// гарнизон в лес, и следующая заходит в пустую деревню.
constexpr f32 DEFEND_RADIUS = 34.f;

ecs::Entity findGuardTarget(ecs::Registry& reg,
                            ecs::Entity player,
                            const NpcDef& def,
                            const glm::vec3& pos,
                            const glm::vec3& home,
                            f32 maxDist)
{
    if (player.valid() && def.faction != factions::FactionId::None) {
        if (auto* rep = reg.get<factions::Reputation>(player)) {
            if (factions::modifiersFor(rep->tier(def.faction)).hostile) {
                auto* tf = reg.get<Transform>(player);
                auto* hp = reg.get<ecs::Health>(player);
                if (tf && hp && hp->current > 0.f) {
                    glm::vec3 d = tf->position - pos;
                    d.y = 0.f;
                    if (glm::dot(d, d) < maxDist * maxDist) return player;
                }
            }
        }
    }
    return findNearestEnemy(reg, pos, home, maxDist, DEFEND_RADIUS);
}

/// Чем и как сильно бьёт этот NPC.
///
/// Безоружный бьёт голыми руками — тем, что записано в его
/// определении. У вооружённого решает ОРУЖИЕ: урон, длина замаха,
/// отбрасывание и скорость ударов берутся из общего реестра, того
/// же, которым считается удар игрока.
struct Swing {
    f32 damage;
    f32 reach;
    f32 period;      ///< сколько между ударами
    f32 knockback;
    f32 critChance;
    f32 critMult;
};

Swing swingOf(const NpcDef& def) {
    if (def.weaponId == combat::WEAPON_NONE)
        return { def.attackDamage, def.attackRange, 1.4f, 0.f, 0.f, 1.f };

    const combat::WeaponDef& w = combat::weapons().get(def.weaponId);
    return { def.attackDamage + w.baseDamage,
             std::max(def.attackRange, w.reach),
             w.windupTime + w.recoveryTime + 0.35f,
             w.knockback, w.critChance, w.critMult };
}

void npcAttack(ecs::Registry& reg, ecs::Entity npc, ecs::Entity target,
               const Swing& sw)
{
    auto* h = reg.get<ecs::Health>(target);
    if (!h || h->current <= 0.f) return;

    combat::DamageInstance d{};
    d.amount = sw.damage;
    if (sw.critChance > 0.f && combat::rollCritical(sw.critChance)) {
        d.amount *= sw.critMult;
        d.isCritical = true;
    }
    d.type   = combat::DamageType::Physical;
    d.sourceEntity = (u32)npc;
    d.targetEntity = (u32)target;
    combat::applyDamage(reg, target, d);

    // Отбрасывание. Без него удар стражи ничем не отличается от
    // укуса: волк стоит вплотную и грызёт сквозь замах.
    if (sw.knockback > 0.f) {
        auto* atk = reg.get<ecs::Transform>(npc);
        auto* ttf = reg.get<ecs::Transform>(target);
        auto* tv  = reg.get<ecs::Velocity>(target);
        if (atk && ttf && tv) {
            glm::vec3 push = ttf->position - atk->position;
            push.y = 0.f;
            const f32 len = glm::length(push);
            if (len > 0.01f) {
                push /= len;
                tv->linear.x += push.x * sw.knockback;
                tv->linear.z += push.z * sw.knockback;
            }
        }
    }
}

/// Есть ли рядом враждебная тварь, от которой безоружному надо бежать.
ecs::Entity threatNear(ecs::Registry& reg, const glm::vec3& pos, f32 radius) {
    auto& hp = reg.pool<ecs::Health>();
    const f32 r2 = radius * radius;
    for (usize i = 0; i < hp.size(); ++i) {
        ecs::Entity e = hp.entityAt((u32)i);
        if (!reg.has<ecs::EnemyTag>(e)) continue;
        auto* h = reg.get<ecs::Health>(e);
        auto* tf = reg.get<ecs::Transform>(e);
        if (!h || !tf || h->current <= 0.f) continue;
        glm::vec3 d = tf->position - pos;
        d.y = 0.f;
        if (glm::dot(d, d) < r2) return e;
    }
    return {};
}

} // namespace

void updateNpcs(world::ChunkManager& world,
                ecs::Registry& reg,
                ecs::Entity playerEntity,
                const glm::vec3& playerPos,
                f32 dt)
{
    // Два пути на кадр — как у мобов. Защитникам они нужнее всего, и
    // очередь до них доходит: бой заказывает путь каждые полсекунды,
    // прогулка — раз в две.
    g_npcPathBudget = 2;

    auto& pool = reg.pool<NpcAI>();
    const usize n = pool.size();
    std::vector<ecs::Entity> toRemove;

    for (usize i = 0; i < n; ++i) {
        ecs::Entity e = pool.entityAt((u32)i);
        auto* ai = pool.get(e);
        auto* tf = reg.get<Transform>(e);
        auto* vel = reg.get<Velocity>(e);
        auto* hp = reg.get<Health>(e);
        auto* tag = reg.get<NpcTag>(e);
        if (!ai || !tf || !vel || !hp || !tag) continue;

        const NpcDef& def = npcRegistry().get(tag->id);
        if (tag->id == NPC_NONE) continue;

        // Таймеры
        if (hp->invulnTime > 0.f) hp->invulnTime = std::max(0.f, hp->invulnTime - dt);
        if (ai->damageFlash > 0.f) ai->damageFlash = std::max(0.f, ai->damageFlash - dt);
        if (ai->attackCooldown > 0.f) ai->attackCooldown -= dt;
        if (ai->offerCooldown > 0.f) ai->offerCooldown -= dt;
        ai->stateTime += dt;

        if (ai->repathCooldown > 0.f) ai->repathCooldown -= dt;
        if (ai->scanCooldown > 0.f)   ai->scanCooldown -= dt;
        if (ai->jumpCooldown > 0.f)   ai->jumpCooldown -= dt;
        if (ai->alertTimer > 0.f)     ai->alertTimer -= dt;

        const glm::vec3 pos = tf->position;
        const physics::CreatureBody body = bodyOf(def);
        const Swing swing = swingOf(def);
        // Опора считается в конце кадра тем же кодом, что у игрока.
        if (auto* lo = reg.get<ecs::Locomotion>(e)) lo->grounded = ai->motion.onGround;

        // Смерть
        if (ai->state == NpcAI::Dead || hp->current <= 0.f) {
            ai->state = NpcAI::Dead;
            ai->deathTimer += dt;
            if (ai->deathTimer > 3.0f) toRemove.push_back(e);
            vel->linear.x *= std::exp(-8.f * dt);
            vel->linear.z *= std::exp(-8.f * dt);
            continue;
        }

        // Диалог — NPC стоит, но всё равно падает, если под ним
        // разобрали пол: гравитацию ведёт общий шаг в конце кадра.
        if (ai->inDialogue) {
            vel->linear.x *= std::exp(-8.f * dt);
            vel->linear.z *= std::exp(-8.f * dt);
            physics::stepCreature(world, ai->motion, tf->position, vel->linear,
                                  body, dt, false);
            continue;
        }

        // ---- Угроза деревне ----
        //
        // Раньше врага искали только в Idle и Wander. Значит защитник,
        // уже вошедший в бой, не видел ни второго волка, ни того, что
        // первый давно мёртв: он стоял над трупом до конца времён.
        // Теперь осмотр идёт ВСЕГДА, два раза в секунду.
        if (def.defender && def.aggroRange > 0.f && ai->scanCooldown <= 0.f) {
            ai->scanCooldown = 0.5f;
            const ecs::Entity enemy = findGuardTarget(reg, playerEntity, def,
                                                      pos, ai->homePos,
                                                      def.aggroRange);
            // Увидеть врага надо ГЛАЗАМИ: сквозь стену дома тварь
            // не видно. Вплотную — слышно и так.
            bool noticed = enemy.valid();
            if (noticed && ai->guardTarget != (u32)enemy) {
                auto* etf = reg.get<Transform>(enemy);
                if (etf) {
                    glm::vec3 d = etf->position - pos;
                    d.y = 0.f;
                    noticed = glm::length(d) < 4.f ||
                              hasLOS(world, pos + glm::vec3(0.f, 1.5f, 0.f),
                                     etf->position + glm::vec3(0.f, 0.9f, 0.f));
                }
            }
            if (noticed) {
                const bool fresh = (ai->guardTarget != (u32)enemy);
                ai->guardTarget = (u32)enemy;
                ai->alertTimer = 6.f;
                if (ai->state != NpcAI::Combat) {
                    ai->state = NpcAI::Combat;
                    ai->stateTime = 0.f;
                }
                if (fresh) ai->repathCooldown = 0.f;
            } else if (ai->state == NpcAI::Combat && ai->alertTimer <= 0.f) {
                // Врагов нет и тревога отзвучала — по домам.
                ai->guardTarget = 0;
                ai->state = NpcAI::Idle;
                ai->stateTime = 0.f;
                ai->path.clear();
                ai->pathIndex = 0;
            }
        }

        // ---- Безоружные разбегаются ----
        //
        // Пекарь и торговец стояли посреди боя как столбы и умирали
        // молча. Драться им нечем, но убежать они могут.
        if (!def.defender && def.attackDamage <= 0.f &&
            ai->state != NpcAI::Flee && ai->state != NpcAI::Travel)
        {
            if (threatNear(reg, pos, 10.f).valid()) {
                ai->state = NpcAI::Flee;
                ai->stateTime = 0.f;
            }
        }

        switch (ai->state) {

        case NpcAI::Idle: {
            vel->linear.x *= std::exp(-6.f * dt);
            vel->linear.z *= std::exp(-6.f * dt);

            // Далеко от дома — возвращаемся. Защитник после погони
            // иначе так и остался бы стоять там, где кончился бой.
            {
                glm::vec3 home = ai->homePos - pos;
                home.y = 0.f;
                if (glm::length(home) > 6.f) {
                    ai->state = NpcAI::Wander;
                    ai->wanderTarget = ai->homePos;
                    ai->stateTime = 0.f;
                    ai->repathCooldown = 0.f;
                    break;
                }
            }

            if (ai->stateTime > frand(IDLE_MIN, IDLE_MAX)) {
                ai->state = NpcAI::Wander;
                ai->stateTime = 0.f;
                f32 a = frand(0.f, 6.28318f);
                f32 r = frand(2.f, WANDER_RADIUS);
                ai->wanderTarget = ai->homePos +
                    glm::vec3(std::cos(a) * r, 0, std::sin(a) * r);
            }
            break;
        }

        case NpcAI::Wander: {
            glm::vec3 d = ai->wanderTarget - pos;
            d.y = 0.f;
            const f32 len = glm::length(d);
            if (len < 0.8f || ai->stateTime > 12.f) {
                ai->state = NpcAI::Idle;
                ai->stateTime = 0.f;
                ai->path.clear();
                ai->pathIndex = 0;
                break;
            }
            const glm::vec3 dir = steerTo(world, *ai, pos, ai->wanderTarget, 2.0f);
            vel->linear.x = dir.x * def.moveSpeed;
            vel->linear.z = dir.z * def.moveSpeed;
            break;
        }

        case NpcAI::Combat: {
            const ecs::Entity target = reg.fromId(ai->guardTarget);
            auto* ttf = reg.get<Transform>(target);
            auto* thp = reg.get<Health>(target);
            if (!ttf || !thp || thp->current <= 0.f) {
                // Цель кончилась. Не домой сразу: рядом может быть
                // второй, и осмотр в начале кадра его найдёт.
                ai->guardTarget = 0;
                if (ai->alertTimer <= 0.f) {
                    ai->state = NpcAI::Idle;
                    ai->stateTime = 0.f;
                }
                vel->linear.x *= std::exp(-6.f * dt);
                vel->linear.z *= std::exp(-6.f * dt);
                break;
            }

            // Ушёл за околицу — не преследуем: деревню защищают в
            // деревне.
            glm::vec3 fromHome = ttf->position - ai->homePos;
            fromHome.y = 0.f;
            if (glm::length(fromHome) > DEFEND_RADIUS * 1.25f) {
                ai->guardTarget = 0;
                ai->state = NpcAI::Idle;
                ai->stateTime = 0.f;
                ai->path.clear();
                ai->pathIndex = 0;
                break;
            }

            glm::vec3 d = ttf->position - pos;
            d.y = 0.f;
            const f32 dist = glm::length(d);

            if (dist < swing.reach) {
                vel->linear.x *= std::exp(-4.f * dt);
                vel->linear.z *= std::exp(-4.f * dt);
                ai->path.clear();
                ai->pathIndex = 0;
                if (ai->attackCooldown <= 0.f) {
                    npcAttack(reg, e, target, swing);
                    ai->attackCooldown = swing.period;
                    audio::events().mobAttack(tf->position);
                }
            } else {
                // Бежим на врага БЕГОМ и ПО ПУТИ: прямая упиралась в
                // угол первого же дома, и защитник топтался у стены,
                // пока волк за ней грыз жителя.
                const glm::vec3 dir = steerTo(world, *ai, pos, ttf->position, 0.6f);
                const f32 speed = def.moveSpeed * 1.35f;
                vel->linear.x = dir.x * speed;
                vel->linear.z = dir.z * speed;
            }

            // Смотреть надо на того, кого бьёшь, — даже стоя на
            // месте в замахе. moveYaw при остановке держит прежнее
            // направление, и защитник рубил, отвернувшись.
            if (auto* fc = reg.get<ecs::Facing>(e))
                if (dist > 0.01f) fc->moveYaw = std::atan2(d.x, d.z);
            break;
        }

        case NpcAI::Flee: {
            // Бежим от ближайшей твари, а если её нет — от игрока.
            // Прежний вариант всегда убегал от игрока: житель,
            // которого грызёт волк, бежал ОТ подошедшей подмоги.
            glm::vec3 from = playerPos;
            if (const ecs::Entity th = threatNear(reg, pos, 14.f); th.valid())
                if (auto* ttf = reg.get<Transform>(th)) from = ttf->position;

            glm::vec3 away = pos - from;
            away.y = 0.f;
            const f32 len = glm::length(away);
            if (len > 0.1f) away /= len;
            vel->linear.x = away.x * def.moveSpeed * 1.5f;
            vel->linear.z = away.z * def.moveSpeed * 1.5f;

            if (ai->stateTime > 6.f && !threatNear(reg, pos, 12.f).valid()) {
                ai->state = NpcAI::Idle;
                ai->stateTime = 0.f;
            }
            break;
        }

        case NpcAI::Travel: {
            // Посыльный идёт к цели и, дойдя, меняет её местами с
            // домом: одного поля хватает на маршрут туда и обратно.
            glm::vec3 d = ai->travelTarget - pos;
            d.y = 0.f;
            const f32 len = glm::length(d);
            if (len < 2.5f) {
                std::swap(ai->travelTarget, ai->homePos);
                ai->stateTime = 0.f;
                break;
            }
            const glm::vec3 dir = steerTo(world, *ai, pos, ai->travelTarget, 2.5f);
            vel->linear.x = dir.x * def.moveSpeed;
            vel->linear.z = dir.z * def.moveSpeed;
            break;
        }

        case NpcAI::Talk:
        case NpcAI::Follow:
        case NpcAI::Dead:
        default: break;
        }

        // ---- Движение ----
        const glm::vec2 wish{ vel->linear.x, vel->linear.z };
        const bool wantsMove = glm::dot(wish, wish) > 0.04f;
        physics::stepCreature(world, ai->motion, tf->position, vel->linear,
                              body, dt, wantsMove);

        // Упёрлись и стоим — прыжок через забор и новый путь.
        if (ai->motion.stuckTime > 0.6f && ai->jumpCooldown <= 0.f) {
            if (physics::tryJump(ai->motion, vel->linear, body)) {
                ai->jumpCooldown = 1.2f;
                ai->motion.stuckTime = 0.f;
            }
            ai->repathCooldown = 0.f;
        }

        if (tf->position.y < -10.f) toRemove.push_back(e);
    }

    for (ecs::Entity e : toRemove) reg.destroy(e);
}

ecs::Entity findInteractableNpc(ecs::Registry& reg,
                                const glm::vec3& playerPos,
                                f32 maxDist)
{
    ecs::Entity best{};
    f32 bestD2 = maxDist * maxDist;

    auto& pool = reg.pool<NpcAI>();
    for (usize i = 0; i < pool.size(); ++i) {
        ecs::Entity e = pool.entityAt((u32)i);
        auto* ai = pool.get(e);
        auto* tf = reg.get<Transform>(e);
        if (!ai || !tf) continue;
        if (ai->state == NpcAI::Dead) continue;

        glm::vec3 d = tf->position - playerPos;
        d.y = 0.f;
        f32 d2 = glm::dot(d, d);
        if (d2 < bestD2) {
            bestD2 = d2;
            best = e;
        }
    }
    return best;
}

} // namespace npc
