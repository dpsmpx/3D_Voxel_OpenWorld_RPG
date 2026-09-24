/**
 * @file item_pickup.cpp
 * @brief Предметы: определения, инвентарь, лут, подбор, использование.
 */
#include "item_pickup.h"
#include "currency.h"
#include "../quests/quest.h"
#include "../audio/audio_events.h"
#include "../ecs/components.h"
#include "../physics/raycast.h"
#include "../world/block.h"
#include "../progression/progression.h"
#include "../core/log.h"
#include <cmath>
#include <vector>

namespace items {

using namespace ecs;

ecs::Entity spawnPickup(ecs::Registry& reg,
                        const glm::vec3& position,
                        const ItemStack& stack,
                        const glm::vec3& initialVelocity)
{
    if (stack.empty()) return {};

    ecs::Entity e = reg.create();

    Transform tf;
    tf.position = position;
    reg.add(e, tf);

    Velocity vel;
    vel.linear = initialVelocity;
    reg.add(e, vel);

    ItemPickup p;
    p.stack = stack;
    p.currencyAmount = (stack.itemId == ITEM_GOLD_COIN) ? (u64)stack.count : 0;
    p.velocity = initialVelocity;
    reg.add(e, p);

    Collider col;
    col.halfExtents = glm::vec3(0.2f, 0.2f, 0.2f);
    reg.add(e, col);

    reg.add(e, Kind{ EntityKind::Item });
    return e;
}

void updatePickups(world::ChunkManager& world,
                   ecs::Registry& reg,
                   ecs::Entity playerEntity,
                   const glm::vec3& playerPos,
                   f32 dt)
{
    auto* inv = reg.get<Inventory>(playerEntity);

    auto& pool = reg.pool<ItemPickup>();
    std::vector<ecs::Entity> toRemove;

    for (usize i = 0; i < pool.size(); ++i) {
        ecs::Entity e = pool.entityAt((u32)i);
        auto* p  = pool.get(e);
        auto* tf = reg.get<Transform>(e);
        auto* v  = reg.get<Velocity>(e);
        if (!p || !tf || !v) continue;

        if (!p->waits) p->lifeRemaining -= dt;
        if (p->lifeRemaining <= 0.f) {
            toRemove.push_back(e);
            continue;
        }

        if (p->lifeRemaining < 5.f) {
            p->blinkTimer += dt * 8.f;
        }

        // ============================================================
        // Phase 15: физика с raycast-коллизией против вокселей
        // ============================================================
        if (!p->onGround) {
            // Гравитация
            p->velocity.y -= 20.f * dt;

            // Определяем шаг как velocity * dt
            glm::vec3 step = p->velocity * dt;
            f32 stepLen = glm::length(step);

            if (stepLen > 1e-5f) {
                glm::vec3 dir = step / stepLen;
                auto vhit = physics::raycastVoxels(world, tf->position, dir, stepLen);
                if (vhit.hit) {
                    // Подошли к блоку. Определяем, это пол или стена.
                    // Если направление вниз и блок ниже — приземляемся.
                    if (dir.y < -0.3f && vhit.normal.y > 0.5f) {
                        // Пол
                        tf->position = glm::vec3(tf->position.x,
                                                 (f32)(vhit.block.y + 1) + 0.001f,
                                                 tf->position.z);
                        p->velocity = glm::vec3(0.f);
                        p->onGround = true;
                        v->linear = p->velocity;
                        continue;
                    }
                    // Стена — гасим горизонтальную скорость
                    tf->position += dir * (vhit.distance - 0.01f);
                    if (vhit.normal.x != 0) p->velocity.x = -p->velocity.x * 0.3f;
                    if (vhit.normal.z != 0) p->velocity.z = -p->velocity.z * 0.3f;
                    p->velocity.y = 0.f;
                    v->linear = p->velocity;
                    continue;
                }
            }

            tf->position += step;

            // Трение
            p->velocity.x *= std::exp(-4.f * dt);
            p->velocity.z *= std::exp(-4.f * dt);
            v->linear = p->velocity;
        }

        // Автоподбор
        if (!inv) continue;
        if (p->pickDelay > 0.f) {
            p->pickDelay -= dt;
            continue;
        }

        glm::vec3 d = tf->position - playerPos;
        d.y *= 0.5f;
        f32 d2 = glm::dot(d, d);
        if (d2 > p->pickupRadius * p->pickupRadius) continue;

        // ---- Монеты идут в КОШЕЛЁК ----
        //
        // Иначе они ложились в сумку предметом, которым нельзя ни
        // заплатить, ни воспользоваться: категория Currency в
        // item_use объявлена неиспользуемой. А золото в мире уже
        // лежало — тайник под руинами выдаёт от шестидесяти до
        // двухсот монет, и это была обещанная награда, на которую
        // нельзя купить ничего.
        if (p->stack.itemId == ITEM_GOLD_COIN) {
            if (auto* wal = reg.get<Wallet>(playerEntity)) {
                const u64 amount = p->currencyAmount > 0 ? p->currencyAmount
                                                         : (u64)p->stack.count;
                if (amount == 0) { toRemove.push_back(e); continue; }
                wal->receive(amount);
                audio::events().pickupItem();
                toRemove.push_back(e);
            }
            continue;
        }

        // Вид предмета запоминаем до того, как стек опустеет.
        const u16 pickedId = p->stack.itemId;
        auto res = inv->addStack(p->stack);
        if (res.added > 0) {
            p->stack.count -= res.added;
            if (p->stack.empty() || p->stack.count == 0) {
                toRemove.push_back(e);
            }
            // Цели вида «принести N таких-то». Функция
            // quests::notifyItemCollected существовала, но её никто не
            // вызывал: такие квесты можно было взять, набить полный
            // рюкзак нужного — и счётчик оставался в нуле.
            quests::notifyItemCollected(reg, (u32)playerEntity,
                                        pickedId, (i32)res.added);
            // Звук подбора: событие было написано, но не проигрывалось
            // нигде — предметы подбирались беззвучно.
            audio::events().pickupItem();
        }
    }

    for (ecs::Entity e : toRemove) reg.destroy(e);
}

} // namespace items