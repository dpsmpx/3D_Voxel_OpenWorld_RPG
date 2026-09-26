/**
 * @file npc_spawner.cpp
 * @brief NPC: роли, диалоги с ветвлением, поведение жителей.
 */
#include "npc_spawner.h"
#include "../physics/creature_motion.h"
#include "../combat/weapon.h"
#include "npc_rig.h"
#include "npc_def.h"
#include "npc_ai.h"
#include "../ecs/components.h"
#include "../combat/components.h"
#include "../trade/trade.h"
#include "../items/currency.h"
#include "../world/block.h"
#include "../core/log.h"
#include <algorithm>
#include <cmath>
#include <unordered_set>
#include <utility>
#include <vector>

namespace npc {

using namespace ecs;

// Детерминированный уникальный ключ для NPC.
// Комбинируем координаты super-chunk + индекс внутри деревни.
u64 npcPersistentKey(i32 sx, i32 sz, u32 idx) {
    u64 k = (u64)(u32)sx;
    k |= (u64)(u32)sz << 32;
    k ^= (u64)idx * 0x9E3779B97F4A7C15ULL;
    return k;
}

ecs::Entity spawnNpc(world::ChunkManager& world,
                     ecs::Registry& reg,
                     u16 typeId,
                     const glm::vec3& at,
                     u64 persistKey)
{
    const NpcDef& def = npcRegistry().get(typeId);
    if (def.maxHealth <= 0.f) return {};

    // Место должно вмещать жителя целиком: дома ставит генератор, и
    // точка у порога легко оказывается внутри стены или косяка.
    // Раньше такого жителя каждый кадр выдавливало вверх по полблока,
    // и он уезжал на крышу собственного дома.
    physics::CreatureBody body;
    body.halfWidth = def.bodyRadius;
    body.height    = def.bodyHeight;
    glm::vec3 spot = at;
    if (!physics::settle(world, spot, body)) return {};

    const ecs::Entity e = reg.create();

    ecs::Transform tf;
    tf.position = spot;
    reg.add(e, tf);
    reg.add(e, ecs::Velocity{});
    // Поворот — состояние сущности, а не вычисление в рендере.
    // NPC доворачивается спокойнее мобов.
    reg.add(e, ecs::Facing{ 0.f, 0.f, 5.f });

    // Облик особи — из ПОСТОЯННОГО ключа, а не из номера сущности:
    // иначе селянин менял бы рост и цвет рубахи всякий раз, как чанк
    // выгружался и загружался обратно.
    const u32 look = persistKey ? (u32)(persistKey ^ (persistKey >> 32))
                                : ((u32)(i32)at.x * 73856093u) ^ ((u32)(i32)at.z * 19349663u);
    reg.add(e, ecs::Appearance{ look });

    // Длина шага — из ТОЙ ЖЕ оснастки, которую нарисует рендер: у
    // низкого селянина шаг короче, и ноги не должны при этом скользить.
    reg.add(e, ecs::Gait{ 0.f, rigFor(typeId, look).strideLength });
    reg.add(e, ecs::Locomotion{});
    reg.add(e, ecs::Health{ def.maxHealth, def.maxHealth, 0.f, 0.f });

    ecs::Collider col;
    col.halfExtents = glm::vec3(def.bodyRadius, def.bodyHeight * 0.5f,
                                def.bodyRadius);
    reg.add(e, col);

    reg.add(e, ecs::Kind{ ecs::EntityKind::NPC });
    reg.add(e, ecs::NPCTag{});

    combat::Combatant cmb;
    cmb.faction = combat::Faction::NPC;
    cmb.radius  = def.bodyRadius;
    cmb.height  = def.bodyHeight;
    reg.add(e, cmb);
    reg.add(e, combat::StatusEffects{});

    NpcTag tag;
    tag.id = typeId;
    tag.persistKey = persistKey;
    reg.add(e, tag);

    // Оружие в руках — настоящее, из общего реестра.
    if (def.weaponId != combat::WEAPON_NONE)
        reg.add(e, combat::EquippedWeapon{ def.weaponId, {} });

    NpcAI ai;
    ai.homePos = spot;
    ai.state   = NpcAI::Idle;
    ai.moveParams.bodyHeight = def.bodyHeight;
    ai.moveParams.allowJump  = true;
    ai.moveParams.allowFall  = true;
    ai.moveParams.allowWater = false;
    reg.add(e, ai);

    return e;
}


// generateVillageNpcs объявлена в заголовке: состав деревни —
// факт о мире, и спрашивают его снаружи.

// Здесь лежала копия хэша из features.cpp — «тот же, что в
// structs::layoutFor». Раскладку деревни теперь спрашивают у
// генератора, и копия не нужна.


// Детерминированное позиционирование NPC внутри деревни
// (позиция + роль + ID).
// Генерирует список NPC для конкретной деревни.
// Возвращает false, если super-chunk — не Village.
bool generateVillageNpcs(world::ChunkManager& world,
                         i32 sx, i32 sz, u64 worldSeed,
                         std::vector<NpcSpec>& out,
                         std::vector<u64>& keysOut)
{
    // Раскладку спрашиваем у генератора, а не повторяем его правила.
    //
    // Здесь лежала копия: тот же хэш, тот же порог 51, то же смещение
    // — с комментарием «совпадает с features::structs::layoutFor».
    // Две копии одного правила расходятся молча: достаточно сдвинуть
    // порог в одном месте, и жители останутся стоять в чистом поле,
    // где деревни больше нет.
    const world::VillageSite site =
        world::villageAt(sx, sz, worldSeed, &world.generator());
    if (!site.exists) return false;

    const u32 h  = site.seed;
    const i32 cx = site.center.x;
    const i32 cz = site.center.z;

    const i32 wy = world.generator().surfaceHeight(cx, cz);

    out.clear();
    keysOut.clear();

    auto addNpc = [&](u16 type, f32 ox, f32 oz) {
        NpcSpec s;
        s.typeId = type;
        i32 px = cx + (i32)ox;
        i32 pz = cz + (i32)oz;
        i32 py = world.generator().surfaceHeight(px, pz);
        s.pos = { (f32)px + 0.5f, (f32)py + 0.5f, (f32)pz + 0.5f };
        out.push_back(s);
    };

    // Состав зависит от уклада. Деревни были населены одинаково —
    // один и тот же набор до человека, — и разница между ними
    // сводилась к тому, где стоит кузнец.
    const world::VillageStyle style = site.style;

    // 1 элдер (рядом с колодцем) — он есть везде: без него некому
    // выдавать задания, а сюжет идёт через него.
    addNpc(NPC_QUEST_GIVER, 2.f, 2.f);

    // Торговцы. В ремесленной их больше: там есть чем торговать.
    addNpc(NPC_TRADER, -3.f, 2.f);
    if (style == world::VillageStyle::Stonemason || ((h >> 20) & 1))
        addNpc(NPC_TRADER, 3.f, -3.f);

    // Кузнец. В ремесленной — двое: она тем и живёт. В лесной его
    // нет вовсе — до кузни оттуда ходят к соседям.
    if (style != world::VillageStyle::Woodland)
        addNpc(NPC_BLACKSMITH, -5.f, -3.f);
    if (style == world::VillageStyle::Stonemason)
        addNpc(NPC_BLACKSMITH, 6.f, -5.f);

    // Лекарь. В сторожевой он нужнее прочего, в лесной его нет.
    if (style != world::VillageStyle::Woodland)
        addNpc(NPC_HEALER, 5.f, 5.f);

    // Стража. В сторожевой её вдвое: деревню держат как заставу.
    i32 guards = 3 + (i32)((h >> 22) & 0x3);
    if (style == world::VillageStyle::Garrison) guards *= 2;
    else if (style == world::VillageStyle::Woodland) guards = 1 + (guards / 2);
    for (i32 i = 0; i < guards; ++i) {
        f32 ang = (f32)i / (f32)guards * 6.28318f;
        f32 r = 12.f + (f32)((h >> (i * 2)) & 0x7);
        addNpc(NPC_GUARD, std::cos(ang) * r, std::sin(ang) * r);
    }
    // Жители. В хлебной их больше всех: она тем и живёт.
    i32 villagers = 3 + (i32)((h >> 26) & 0x3);
    if (style == world::VillageStyle::Farmstead) villagers += 3;
    for (i32 i = 0; i < villagers; ++i) {
        f32 ang = (f32)((h >> (i + 4)) & 0xFF) / 255.f * 6.28318f;
        // До девяти жителей: сдвиг 8 + 3·8 = 32 для 32-битного h — UB.
        // «& 31» даёт то же, что процессор делал и так (ARM и x86
        // берут сдвиг по модулю 32), — раскладка деревень не меняется.
        f32 r   = 6.f + (f32)((h >> ((i * 3 + 8) & 31)) & 0xF);
        addNpc(NPC_VILLAGER, std::cos(ang) * r, std::sin(ang) * r);
    }

    // Индексы для ключей
    for (u32 i = 0; i < out.size(); ++i) {
        keysOut.push_back(npcPersistentKey(sx, sz, i));
    }

    (void)wy;
    return true;
}



bool NpcSpawner::isDead(u64 key) const {
    return std::binary_search(deadKeys_.begin(), deadKeys_.end(), key);
}

void NpcSpawner::markDead(u64 key) {
    if (key == 0) return;
    auto it = std::lower_bound(deadKeys_.begin(), deadKeys_.end(), key);
    if (it != deadKeys_.end() && *it == key) return;
    deadKeys_.insert(it, key);
}

void NpcSpawner::setDeadKeys(std::vector<u64> keys) {
    std::sort(keys.begin(), keys.end());
    keys.erase(std::unique(keys.begin(), keys.end()), keys.end());
    deadKeys_ = std::move(keys);
}

namespace {

/// Посыльный на дороге.
///
/// Набор составных частей тот же, что у деревенского жителя: иначе он
/// не будет ни рисоваться, ни двигаться, ни получать урон. Отличия
/// ровно два — состояние Travel и цель пути.
ecs::Entity spawnCourier(world::ChunkManager& world, ecs::Registry& reg,
                         const glm::vec3& at, const glm::vec3& target)
{
    const ecs::Entity e = spawnNpc(world, reg, NPC_COURIER, at, /*persistKey=*/0);
    if (!e.valid()) return {};

    // Посыльный не принадлежит деревне: «не появляться снова» к нему
    // не относится — он и так уходит. Отличается от жителя ровно
    // двумя вещами: состоянием Travel и целью пути.
    if (auto* ai = reg.get<NpcAI>(e)) {
        ai->travelTarget = target;
        ai->state        = NpcAI::Travel;
    }
    return e;
}

} // namespace

void NpcSpawner::updateCouriers(world::ChunkManager& world, ecs::Registry& reg,
                                const glm::vec3& playerPos, u64 worldSeed)
{
    // Сколько их уже ходит. Двое — это «дорога живая», а не толпа.
    constexpr u32 MAX_COURIERS = 2;
    u32 alive = 0;
    {
        auto& pool = reg.pool<NpcTag>();
        for (usize i = 0; i < pool.size(); ++i) {
            auto* t = pool.get(pool.entityAt((u32)i));
            if (t && t->id == NPC_COURIER) ++alive;
        }
    }
    if (alive >= MAX_COURIERS) return;

    const i32 sc0x = (i32)std::floor(playerPos.x / (f32)SUPER_BLOCKS);
    const i32 sc0z = (i32)std::floor(playerPos.z / (f32)SUPER_BLOCKS);

    // Те же отрезки, что кладёт applyRoads: от деревни к соседней на
    // восток и на юг. Правило одно, и списка дорог заводить не нужно.
    for (i32 dz = -2; dz <= 2; ++dz)
        for (i32 dx = -2; dx <= 2; ++dx) {
            const world::VillageSite a = world::villageAt(
                sc0x + dx, sc0z + dz, worldSeed, &world.generator());
            if (!a.exists) continue;

            const world::VillageSite ends[2] = {
                world::villageAt(sc0x + dx + 1, sc0z + dz, worldSeed, &world.generator()),
                world::villageAt(sc0x + dx, sc0z + dz + 1, worldSeed, &world.generator()),
            };
            for (const auto& b : ends) {
                if (!b.exists) continue;

                // Ближайшая точка отрезка к игроку: посыльного ставим
                // на дорогу, а не рядом с ней.
                const glm::vec2 A{ (f32)a.center.x, (f32)a.center.z };
                const glm::vec2 B{ (f32)b.center.x, (f32)b.center.z };
                const glm::vec2 P{ playerPos.x, playerPos.z };
                const glm::vec2 v = B - A;
                const f32 len2 = glm::dot(v, v);
                if (len2 < 1.f) continue;
                f32 t = glm::dot(P - A, v) / len2;
                t = std::max(0.f, std::min(1.f, t));
                const glm::vec2 near = A + v * t;
                if (glm::distance(near, P) > 70.f) continue;

                // Ставим впереди по дороге, а не под ноги: посыльный,
                // возникший вплотную, выглядит как подброшенный.
                const glm::vec2 dir = glm::normalize(v);
                const glm::vec2 spotXZ = near + dir * 34.f;

                const i32 bx = (i32)std::floor(spotXZ.x);
                const i32 bz = (i32)std::floor(spotXZ.y);
                if (!world.findChunk(bx >> 5, bz >> 5)) continue;
                const i32 gy = world.generator().surfaceHeight(bx, bz);
                if (world.getVoxel(bx, gy - 1, bz) == world::AIR) continue;

                const glm::vec3 spot{ (f32)bx + 0.5f, (f32)gy, (f32)bz + 0.5f };
                const ecs::Entity e = spawnCourier(world, reg, spot,
                                                   { (f32)b.center.x + 0.5f,
                                                     (f32)b.center.y,
                                                     (f32)b.center.z + 0.5f });
                if (e.valid()) { ++activeCount_; return; }
            }
        }
}

void NpcSpawner::update(world::ChunkManager& world,
                        ecs::Registry& reg,
                        const glm::vec3& playerPos,
                        u64 worldSeed,
                        f32 dt)
{
    dt = std::clamp(dt, 0.f, 0.25f);
    spawnTimer_ += dt;
    despawnTimer_ += dt;

    // ---- Кто умер, тот больше не появится ----
    //
    // Проход идёт каждый кадр, без таймера: тело убитого живёт три
    // секунды (npc_ai), и пропустить их можно, только если спавнер
    // перестанут звать. Порядок здесь важен. Умирающего помечаем и
    // оставляем дожить свою анимацию; живого убираем только если его
    // ключ УЖЕ в списке — а это бывает единственным способом:
    // список пришёл из сохранения.
    {
        std::vector<ecs::Entity> loadedDead;
        auto& pool = reg.pool<NpcAI>();
        for (usize i = 0; i < pool.size(); ++i) {
            ecs::Entity e = pool.entityAt((u32)i);
            auto* ai  = pool.get(e);
            auto* tag = reg.get<NpcTag>(e);
            if (!ai || !tag || tag->persistKey == 0) continue;

            auto* hp = reg.get<Health>(e);
            const bool dying = (ai->state == NpcAI::Dead) ||
                               (hp && hp->current <= 0.f);
            if (dying) { markDead(tag->persistKey); continue; }
            if (isDead(tag->persistKey)) loadedDead.push_back(e);
        }
        for (auto e : loadedDead) {
            reg.destroy(e);
            if (activeCount_ > 0) --activeCount_;
        }
    }

    // ---- Спавн ----
    if (spawnTimer_ >= 0.75f) {
        spawnTimer_ = 0.f;

        const i32 playerSx = (i32)std::floor(playerPos.x / (f32)SUPER_BLOCKS);
        const i32 playerSz = (i32)std::floor(playerPos.z / (f32)SUPER_BLOCKS);

        for (i32 dz = -2; dz <= 2; ++dz) {
            for (i32 dx = -2; dx <= 2; ++dx) {
                const i32 sx = playerSx + dx;
                const i32 sz = playerSz + dz;

                // Уже есть NPC из этой деревни?
                // Проверяем по наличию хотя бы одного NpcAI с ключом super-chunk.
                // Простой способ — используем первый ключ как маркер.
                bool anyExists = false;
                {
                    auto& pool = reg.pool<NpcAI>();
                    for (usize i = 0; i < pool.size(); ++i) {
                        auto* ai = pool.get(pool.entityAt((u32)i));
                        if (!ai) continue;
                        // Используем homePos как маркер деревни
                        // Ищем совпадение по super-chunk
                        i32 nSx = (i32)std::floor(ai->homePos.x / (f32)SUPER_BLOCKS);
                        i32 nSz = (i32)std::floor(ai->homePos.z / (f32)SUPER_BLOCKS);
                        if (nSx == sx && nSz == sz) {
                            anyExists = true;
                            break;
                        }
                    }
                }
                if (anyExists) continue;

                std::vector<NpcSpec> specs;
                std::vector<u64> keys;
                if (!generateVillageNpcs(world, sx, sz, worldSeed, specs, keys)) continue;

                // Проверка расстояния до игрока — не спавним далеко
                f32 ddx = specs.empty() ? 9999.f
                                        : specs[0].pos.x - playerPos.x;
                f32 ddz = specs.empty() ? 9999.f
                                        : specs[0].pos.z - playerPos.z;
                if (ddx * ddx + ddz * ddz > SPAWN_DIST * SPAWN_DIST) continue;

                for (usize si = 0; si < specs.size(); ++si) {
                    const NpcSpec& s = specs[si];

                    // Постоянный ключ особи. Он же решает, вселять ли
                    // её вообще: убитого не воскрешаем.
                    const u64 key = (si < keys.size()) ? keys[si] : (u64)si;
                    if (isDead(key)) continue;

                    const NpcDef& def = npcRegistry().get(s.typeId);
                    if (def.maxHealth <= 0.f) continue;

                    const ecs::Entity e = spawnNpc(world, reg, s.typeId,
                                                   s.pos, key);
                    if (!e.valid()) continue;

                    // Торговцу — прилавок. Без него trade::buy и
                    // trade::sell всегда отвечали «нет такой позиции»:
                    // компонент TradeInventory не добавлялся никому,
                    // а значит вся торговля — цены, репутация, запас,
                    // ежедневное обновление ассортимента и целый экран
                    // интерфейса — была недостижима.
                    //
                    // Зерно — от координат деревни и типа NPC: у
                    // одного и того же торговца ассортимент один и тот
                    // же от запуска к запуску.
                    if (def.role == NpcRole::Trader ||
                        def.role == NpcRole::Blacksmith)
                    {
                        trade::TradeInventory shop;
                        const u64 shopSeed = worldSeed
                            ^ ((u64)(u32)sx * 0x9E3779B97F4A7C15ull)
                            ^ ((u64)(u32)sz * 0xC4CEB9FE1A85EC53ull)
                            ^ ((u64)s.typeId * 0xFF51AFD7ED558CCDull);
                        trade::generateTraderInventory(shop, shopSeed);
                        reg.add(e, std::move(shop));
                        // Кошелёк торговца: без него продажа проходила
                        // бы вообще без проверки, хватает ли у него
                        // денег, — золото бралось бы из ниоткуда.
                        reg.add(e, items::Wallet{ shop.gold });
                    }

                    ++activeCount_;
                }
            }
        }
    }

    // ---- Посыльные ----
    //
    // Раз в четыре секунды, а не каждый кадр: проход перебирает
    // двадцать пять super-chunk'ов и для каждого спрашивает раскладку
    // деревни. Дорога не появляется и не исчезает, спешить некуда.
    courierTimer_ += dt;
    if (courierTimer_ >= 4.0f) {
        courierTimer_ = 0.f;
        updateCouriers(world, reg, playerPos, worldSeed);
    }

    // ---- Деспавн ----
    if (despawnTimer_ >= 2.0f) {
        despawnTimer_ = 0.f;

        std::vector<ecs::Entity> toRemove;
        auto& pool = reg.pool<NpcAI>();
        for (usize i = 0; i < pool.size(); ++i) {
            ecs::Entity e = pool.entityAt((u32)i);
            auto* ai = pool.get(e);
            auto* tf = reg.get<Transform>(e);
            if (!ai || !tf) continue;

            glm::vec3 d = tf->position - playerPos;
            d.y = 0.f;
            if (glm::length(d) > DESPAWN_DIST) {
                toRemove.push_back(e);
            }
        }
        for (auto e : toRemove) {
            reg.destroy(e);
            if (activeCount_ > 0) --activeCount_;
        }
    }
}

} // namespace npc