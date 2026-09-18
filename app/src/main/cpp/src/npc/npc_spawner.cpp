/**
 * @file npc_spawner.cpp
 * @brief NPC: роли, диалоги с ветвлением, поведение жителей.
 */
#include "npc_spawner.h"
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

namespace {

// Здесь лежала копия хэша из features.cpp — «тот же, что в
// structs::layoutFor». Раскладку деревни теперь спрашивают у
// генератора, и копия не нужна.


// Детерминированное позиционирование NPC внутри деревни
// (позиция + роль + ID).
struct NpcSpec {
    u16      typeId;
    glm::vec3 pos;
};

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

    // 1 элдер (рядом с колодцем)
    addNpc(NPC_QUEST_GIVER, 2.f, 2.f);
    // 1-2 торговца
    addNpc(NPC_TRADER, -3.f, 2.f);
    if ((h >> 20) & 1) addNpc(NPC_TRADER, 3.f, -3.f);
    // 1 кузнец
    addNpc(NPC_BLACKSMITH, -5.f, -3.f);
    // 1 лекарь
    addNpc(NPC_HEALER, 5.f, 5.f);
    // 3-5 стражников
    i32 guards = 3 + (i32)((h >> 22) & 0x3);
    for (i32 i = 0; i < guards; ++i) {
        f32 ang = (f32)i / (f32)guards * 6.28318f;
        f32 r = 12.f + (f32)((h >> (i * 2)) & 0x7);
        addNpc(NPC_GUARD, std::cos(ang) * r, std::sin(ang) * r);
    }
    // 3-6 жителей (случайно разбросаны)
    i32 villagers = 3 + (i32)((h >> 26) & 0x3);
    for (i32 i = 0; i < villagers; ++i) {
        f32 ang = (f32)((h >> (i + 4)) & 0xFF) / 255.f * 6.28318f;
        f32 r   = 6.f + (f32)((h >> (i * 3 + 8)) & 0xF);
        addNpc(NPC_VILLAGER, std::cos(ang) * r, std::sin(ang) * r);
    }

    // Индексы для ключей
    for (u32 i = 0; i < out.size(); ++i) {
        keysOut.push_back(npcPersistentKey(sx, sz, i));
    }

    (void)wy;
    return true;
}

} // namespace

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

void NpcSpawner::update(world::ChunkManager& world,
                        ecs::Registry& reg,
                        const glm::vec3& playerPos,
                        u64 worldSeed)
{
    spawnTimer_ += 1.f / 60.f;
    despawnTimer_ += 1.f / 60.f;

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

                    ecs::Entity e = reg.create();

                    ecs::Transform tf;
                    tf.position = s.pos;
                    reg.add(e, tf);

                    reg.add(e, ecs::Velocity{});
                    // Поворот — состояние сущности, а не вычисление
                    // в рендере. NPC доворачивается спокойнее мобов.
                    reg.add(e, ecs::Facing{ 0.f, 0.f, 5.f });

                    // Облик особи — из ПОСТОЯННОГО ключа NPC, а не из
                    // номера сущности: иначе селянин менял бы рост и
                    // цвет рубахи всякий раз, как чанк выгружался и
                    // загружался обратно.
                    const u32 look = (u32)(key ^ (key >> 32));
                    reg.add(e, ecs::Appearance{ look });

                    // Длина шага — из ТОЙ ЖЕ оснастки, которую
                    // нарисует рендер: у низкого селянина шаг короче,
                    // и ноги не должны при этом скользить.
                    reg.add(e, ecs::Gait{ 0.f, rigFor(s.typeId, look).strideLength });
                    reg.add(e, ecs::Locomotion{});
                    reg.add(e, ecs::Health{ def.maxHealth, def.maxHealth, 0.f, 0.f });

                    ecs::Collider col;
                    col.halfExtents = glm::vec3(def.bodyRadius,
                                                def.bodyHeight * 0.5f,
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
                    tag.id = s.typeId;
                    tag.persistKey = key;
                    reg.add(e, tag);

                    NpcAI ai;
                    ai.homePos = s.pos;
                    ai.state   = NpcAI::Idle;
                    reg.add(e, ai);

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
