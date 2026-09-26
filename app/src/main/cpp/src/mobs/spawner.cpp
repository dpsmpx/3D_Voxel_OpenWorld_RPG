/**
 * @file spawner.cpp
 * @brief Мобы: определения, конечный автомат ИИ, спавн, боссы.
 */
#include "spawner.h"
#include "../physics/creature_motion.h"
#include "../entity/mob_rigs.h"
#include "mob_def.h"
#include "mob_ai.h"
#include "../ecs/components.h"
#include "../combat/components.h"
#include "../world/block.h"
#include "../core/log.h"
#include <algorithm>
#include <cmath>
#include <random>
#include <vector>

namespace mobs {

using namespace ecs;

namespace {

std::mt19937& rng() {
    static std::mt19937 g(0x12345678);
    return g;
}

u32 urand() { return rng()(); }

bool findSpawnSpot(world::ChunkManager& world,
                   i32 cx, i32 cz,
                   i32& outX, i32& outY, i32& outZ)
{
    // Все двенадцать попыток бьют в один и тот же чанк, а каждая
    // проходит колонку сверху донизу по три чтения на шаг: до четырёх
    // с половиной тысяч чтений на вызов, и так шесть раз дважды в
    // секунду. Через getVoxel это больше миллисекунды в одном кадре.
    world::VoxelReader rd(world);
    for (int tries = 0; tries < 12; ++tries) {
        i32 x = cx * world::CHUNK_SIZE + (i32)(urand() % world::CHUNK_SIZE);
        i32 z = cz * world::CHUNK_SIZE + (i32)(urand() % world::CHUNK_SIZE);

        // Все годные места колонки, а не первое сверху: поверхность и
        // дно пещер под ней.
        //
        // Годное — только на земле (world::Footing::Ground). Первое
        // сверху «воздух над твёрдым» — это то кровля дома, то верх
        // невидимого ствола под кроной: там твари и рождались. Внутри
        // построек рождаются свои: в подземельях и замках — гарнизон
        // (updateGarrison), в деревнях — жители (npc::NpcSpawner).
        constexpr i32 MAX_SPOTS = 8;
        i32 spotY[MAX_SPOTS];
        i32 spots = 0;
        for (i32 y = world::CHUNK_SIZE_Y - 2; y > 1; --y) {
            const u16 b   = rd.at(x, y, z);
            const u16 bel = rd.at(x, y - 1, z);
            const u16 abv = rd.at(x, y + 1, z);
            if (b != world::AIR || abv != world::AIR) continue;
            if (world::footingOf(bel) != world::Footing::Ground) continue;
            if (spots < MAX_SPOTS) spotY[spots++] = y;
        }
        if (spots == 0) continue;

        // Чаще всего — поверхность: мир живёт наверху, и внутренности
        // не должны отнимать у него тварей. Но не всегда.
        const i32 pick = ((urand() & 3u) == 0u && spots > 1)
                       ? (i32)(urand() % (u32)spots)
                       : 0;
        outX = x; outY = spotY[pick]; outZ = z;
        return true;
    }
    return false;
}

} // namespace

f32 Spawner::lightAt(world::ChunkManager& world, const world::DayCycle& day,
                     i32 x, i32 y, i32 z)
{
    // Небесный свет доходит до точки, если над ней нет непрозрачных
    // блоков. Полноценное запекание света чанк пока не считает,
    // поэтому проверяем колонку напрямую — дёшево и достаточно.
    world::VoxelReader rd(world);
    for (i32 yy = y + 1; yy < world::CHUNK_SIZE_Y; ++yy) {
        const u16 b = rd.at(x, yy, z);
        if (b == world::AIR) continue;
        if (world::blocks().isTransparent(b)) continue;
        return 0.05f;   // под перекрытием — пещерная темнота
    }
    return day.skyLight();
}

u16 mobIdForBiome(world::BiomeId biome, bool night, u32 rngVal) {
    f32 r = (f32)(rngVal & 0xFFFF) / 65536.f;

    switch (biome) {
        case world::Ocean:      return MOB_NONE;
        case world::Beach:      return (r < 0.5f) ? MOB_CHICKEN : MOB_SHEEP;
        // Днём мир не обязан быть пустым. Овца и корова остаются
        // самыми частыми, но между ними теперь попадается и то, от
        // чего приходится доставать оружие при солнце.
        case world::Plains:
            return night ? ((r < 0.6f) ? MOB_SKELETON : MOB_GOBLIN)
                         : ((r < 0.35f) ? MOB_SHEEP
                         :  (r < 0.65f) ? MOB_COW
                         :  (r < 0.85f) ? MOB_BOAR
                                        : MOB_BANDIT);
        case world::Forest:
            return night ? ((r < 0.4f) ? MOB_WOLF
                         : (r < 0.75f) ? MOB_SKELETON
                                       : MOB_GOBLIN)
                         : ((r < 0.35f) ? MOB_SHEEP
                         :  (r < 0.60f) ? MOB_COW
                         :  (r < 0.85f) ? MOB_BOAR
                                        : MOB_WOLF);
        case world::Taiga:
            return night ? ((r < 0.5f) ? MOB_WOLF : MOB_SKELETON)
                         : ((r < 0.40f) ? MOB_SHEEP
                         :  (r < 0.65f) ? MOB_COW
                                        : MOB_WOLF);
        case world::Desert:
            return night ? MOB_SKELETON
                         : ((r < 0.5f) ? MOB_CHICKEN
                         :  (r < 0.8f) ? MOB_NONE
                                       : MOB_BANDIT);
        case world::Savanna:
            return night ? MOB_GOBLIN
                         : ((r < 0.45f) ? MOB_COW
                         :  (r < 0.75f) ? MOB_BOAR
                                        : MOB_BANDIT);
        case world::Tundra:
            return (r < 0.6f) ? MOB_SHEEP : MOB_WOLF;
        // В горах пусто не бывает: разбойники держат перевалы, а ночью
        // к ним добавляются гоблины.
        case world::Mountains:
            return night ? MOB_GOBLIN
                         : ((r < 0.6f) ? MOB_NONE : MOB_BANDIT);
        // Болото: слизни, волки и ведьмы. Ведьма — не «ещё один
        // моб», а причина ходить на болото с опаской: травит ударом
        // и залечивается.
        case world::Swamp:
            return (r < 0.40f) ? MOB_SLIME
                 : (r < 0.70f) ? MOB_WOLF
                               : MOB_WITCH;
        case world::Volcanic:
            return MOB_SLIME;
        // Чёрный лес: только чудовища, и никакой скотины. Овца,
        // мирно пасущаяся среди сухостоя, зловещим это место быть
        // перестаёт.
        case world::Blight:
            return (r < 0.35f) ? MOB_SKELETON
                 : (r < 0.70f) ? MOB_GOBLIN
                 : (r < 0.90f) ? MOB_WOLF
                               : MOB_SLIME;
        default:
            return MOB_NONE;
    }
}

u16 Spawner::lairMobId(world::LairKind kind) {
    switch (kind) {
        case world::LairKind::Wolves:    return MOB_WOLF;
        case world::LairKind::Skeletons: return MOB_SKELETON;
        case world::LairKind::Goblins:   return MOB_GOBLIN;
        case world::LairKind::Slimes:    return MOB_SLIME;
        case world::LairKind::None:      break;
    }
    return MOB_NONE;
}

ecs::Entity spawnMob(world::ChunkManager& world,
                     ecs::Registry& reg,
                     u16 mobId,
                     const glm::vec3& pos)
{
    const MobDef& def = mobRegistry().get(mobId);
    if (def.maxHealth <= 0.f) return {};

    // ---- Место должно вмещать тварь ЦЕЛИКОМ ----
    //
    // Место искали по двум блокам воздуха над твёрдым — и это всё,
    // что проверялось, для кого угодно. Каменный страж ростом в три
    // с половиной блока и шириной в два с половиной рождался внутри
    // потолка зала, а босса в подземелье ставили вовсе без проверки
    // свободного места: только «пол под ногами не воздух». Дальше
    // спасательный подъём выдавливал его вверх сквозь этажи, и самый
    // опасный противник игры встречал игрока, стоя на крыше.
    physics::CreatureBody body;
    body.halfWidth = def.bodyRadius;
    body.height    = def.bodyHeight;

    glm::vec3 at = pos;
    if (!physics::settle(world, at, body)) return {};

    ecs::Entity e = reg.create();

    ecs::Transform tf;
    tf.position = at;
    reg.add(e, tf);

    reg.add(e, ecs::Velocity{});
    // Поворот — состояние сущности, а не вычисление в рендере.
    reg.add(e, ecs::Facing{});
    // Длина шага — свойство модели, а не состояния: берём из
    // оснастки вида, где она выведена из длины ног.
    reg.add(e, ecs::Gait{ 0.f, rigFor(mobId).strideLength });
    reg.add(e, ecs::Locomotion{});
    reg.add(e, ecs::Health{ def.maxHealth, def.maxHealth, 0.f, 0.f });
    reg.add(e, ecs::Collider{
        glm::vec3(def.bodyRadius, def.bodyHeight * 0.5f, def.bodyRadius)
    });
    reg.add(e, ecs::Kind{
        def.hostile ? ecs::EntityKind::Mob : ecs::EntityKind::NPC
    });
    if (def.hostile) reg.add(e, ecs::EnemyTag{});
    reg.add(e, ecs::AIAgent{});
    reg.add(e, MobTag{ mobId });

    MobAI ai;
    ai.homePos = at;
    ai.moveParams.bodyHeight  = def.bodyHeight;
    ai.moveParams.allowFall   = true;
    ai.moveParams.allowWater  = def.canSwim;
    ai.moveParams.allowJump   = true;
    reg.add(e, ai);

    // --- Боевые компоненты (Phase 8) ---
    combat::Combatant cmb;
    cmb.faction = def.hostile ? combat::Faction::Hostile
                              : combat::Faction::Passive;
    cmb.radius  = def.bodyRadius;
    cmb.height  = def.bodyHeight;
    if (def.hostile) {
        cmb.resistance.physical = 0.05f;
    }
    reg.add(e, cmb);

    reg.add(e, combat::StatusEffects{});

    return e;
}

void Spawner::updateGarrison(world::ChunkManager& world, ecs::Registry& reg,
                             const glm::vec3& playerPos, u64 worldSeed, f32 dt)
{
    garrisonTimer_ += dt;
    if (garrisonTimer_ < 2.0f) return;
    garrisonTimer_ = 0.f;

    const i32 sx = (i32)std::floor(playerPos.x / (f32)world::SUPER_CHUNK_BLOCKS);
    const i32 sz = (i32)std::floor(playerPos.z / (f32)world::SUPER_CHUNK_BLOCKS);

    auto& blocks = world::blocks();
    world::VoxelReader rd(world);

    for (i32 dz = -1; dz <= 1; ++dz)
        for (i32 dx = -1; dx <= 1; ++dx) {
            const i32 cx = sx + dx, cz = sz + dz;
            const u64 key = ((u64)(u32)cx << 32) | (u32)cz;
            if (garrisonPlaced_.count(key)) continue;

            // Где стоит постройка и как высоко её внутренность.
            glm::ivec3 at{0};
            i32 top = 0;
            const world::DungeonSite tree =
                world::treeDungeonAt(cx, cz, worldSeed, &world.generator());
            if (tree.exists) {
                at = tree.center;
                top = tree.center.y;
            } else {
                const world::CastleSite castle =
                    world::castleAt(cx, cz, worldSeed, &world.generator());
                if (!castle.exists) continue;
                at = castle.center;
                top = castle.center.y + 30;
            }

            const f32 ddx = (f32)at.x - playerPos.x;
            const f32 ddz = (f32)at.z - playerPos.z;
            if (ddx * ddx + ddz * ddz > 150.f * 150.f) continue;
            if (!world.findChunk(at.x >> 5, at.z >> 5)) continue;

            const i32 ground = world.generator().surfaceHeight(at.x, at.z);

            // Места ищем ПО ВОКСЕЛЯМ, а не по числам постройки:
            // этажи дерева и залы замка — это и есть «воздух над
            // твёрдым», и второй раз выписывать их раскладку значило
            // бы завести копию, которая однажды разойдётся.
            u32 placed = 0;
            for (i32 k = 0; k < 40 && placed < GARRISON_PER_SITE; ++k) {
                const u32 h = world::feat_util::hashXZ(cx * 61 + k, cz * 37 + k,
                                                       worldSeed ^ 0x9A881u);
                const i32 ox = (i32)(h % 9u) - 4;
                const i32 oz = (i32)((h >> 8) % 9u) - 4;
                const i32 y  = ground + 2 + (i32)((h >> 16) %
                                                 (u32)std::max(4, top - ground));

                const i32 wx = at.x + ox, wz = at.z + oz;
                if (!blocks.isSolid(rd.at(wx, y - 1, wz))) continue;
                if (rd.at(wx, y, wz)     != world::AIR) continue;
                if (rd.at(wx, y + 1, wz) != world::AIR) continue;

                // Внутри — значит под крышей: если прямо над головой
                // открытое небо, это двор, а не зал.
                bool roofed = false;
                for (i32 up = y + 2; up < world::CHUNK_SIZE_Y && !roofed; ++up)
                    if (blocks.isSolid(rd.at(wx, up, wz))) roofed = true;
                if (!roofed) continue;

                static const u16 KIND[3] = { MOB_SKELETON, MOB_GOBLIN, MOB_BANDIT };
                const u16 id = KIND[(h >> 24) % 3u];
                const glm::vec3 pos{ (f32)wx + 0.5f, (f32)y, (f32)wz + 0.5f };
                rd.release();   // spawnMob сам читает мир через менеджер
                if (spawnMob(world, reg, id, pos).valid()) {
                    ++placed;
                    ++mobCount_;
                }
            }

            if (placed > 0) {
                garrisonPlaced_.insert(key);
                LOGI("Гарнизон подземелья (%d, %d): %u тварей", at.x, at.z, placed);
            }
        }
}

void Spawner::updateBosses(world::ChunkManager& world, ecs::Registry& reg,
                           const glm::vec3& playerPos, u64 worldSeed, f32 dt)
{
    bossTimer_ += dt;
    if (bossTimer_ < 3.0f) return;
    bossTimer_ = 0.f;

    // Супер-чанк игрока и восемь соседних: подземелье большое,
    // его центр может лежать в соседней ячейке.
    const i32 sx = (i32)std::floor(playerPos.x / (f32)world::SUPER_CHUNK_BLOCKS);
    const i32 sz = (i32)std::floor(playerPos.z / (f32)world::SUPER_CHUNK_BLOCKS);

    for (i32 dz = -1; dz <= 1; ++dz) {
        for (i32 dx = -1; dx <= 1; ++dx) {
            const i32 cx = sx + dx, cz = sz + dz;
            const u64 key = siteKey(cx, cz);
            if (bossAlive_.count(key) || bossDefeated(key)) continue;

            // Подземелья трёх видов, и обходятся они одним кодом:
            // различаются только тем, где стоит зал.
            //
            // Замок до сих пор в этот список не входил: донжон в
            // семьдесят блоков по стороне стоял пустым, и самое
            // заметное строение в мире было единственным, где никого
            // не было.
            world::DungeonSite site = world::dungeonAt(cx, cz, worldSeed);
            if (!site.exists)
                site = world::treeDungeonAt(cx, cz, worldSeed, &world.generator());
            if (!site.exists) {
                const world::CastleSite castle =
                    world::castleAt(cx, cz, worldSeed, &world.generator());
                if (castle.exists) {
                    site.exists = true;
                    // На уровне двора: донжон строится от него вверх.
                    site.center = castle.center;
                    site.seed = world::feat_util::hashXZ(cx, cz, worldSeed ^ 0xCA57u);
                }
            }
            if (!site.exists) continue;

            // Ждём, пока чанк с залом действительно загрузится:
            // иначе босс провалится сквозь несуществующий пол.
            const glm::vec3 pos{ (f32)site.center.x + 0.5f,
                                 (f32)site.center.y + 1.f,
                                 (f32)site.center.z + 0.5f };
            const f32 ddx = pos.x - playerPos.x, ddz = pos.z - playerPos.z;
            if (ddx * ddx + ddz * ddz > 160.f * 160.f) continue;
            if (!world.findChunk(site.center.x >> 5, site.center.z >> 5)) continue;

            // Пол под залом должен быть твёрдым.
            if (world.getVoxel(site.center.x, site.center.y - 1, site.center.z)
                == world::AIR) continue;

            const u16 bossId = (site.seed & 1) ? MOB_BOSS_WARDEN : MOB_BOSS_HOLLOW;
            if (const ecs::Entity boss = spawnMob(world, reg, bossId, pos); boss.valid()) {
                bossAlive_[key] = boss;
                ++mobCount_;
                LOGI("Босс %s размещён в подземелье (%d, %d)",
                     mobRegistry().get(bossId).name, site.center.x, site.center.z);
            }
        }
    }
}

void Spawner::updateAmbushes(world::ChunkManager& world, ecs::Registry& reg,
                             const glm::vec3& playerPos, u64 worldSeed)
{
    // Четверо: трое — это стычка, шестеро — это уже бой, которого
    // игрок первого уровня не переживёт нигде.
    constexpr u32 AMBUSH_SIZE = 4;
    constexpr f32 TRIGGER     = 14.f;

    const i32 sc0x = (i32)std::floor(playerPos.x / 256.f);
    const i32 sc0z = (i32)std::floor(playerPos.z / 256.f);

    for (i32 dz = -1; dz <= 1; ++dz)
        for (i32 dx = -1; dx <= 1; ++dx) {
            const i32 sx = sc0x + dx, sz = sc0z + dz;
            const world::VillageSite a = world::villageAt(
                sx, sz, worldSeed, &world.generator());
            if (!a.exists) continue;

            // Те же отрезки, что кладёт applyRoads: на восток и на юг.
            const world::VillageSite ends[2] = {
                world::villageAt(sx + 1, sz, worldSeed, &world.generator()),
                world::villageAt(sx, sz + 1, worldSeed, &world.generator()),
            };
            for (u32 side = 0; side < 2; ++side) {
                const world::VillageSite& b = ends[side];
                if (!b.exists) continue;

                const u64 key = ((u64)(u32)sx << 40) ^ ((u64)(u32)sz << 16) ^ side;
                if (std::find(sprung_.begin(), sprung_.end(), key) != sprung_.end())
                    continue;

                // Место засады — не ровно середина: середину видно
                // с обоих концов, а засаду ставят там, где дорога
                // уже отошла от деревни.
                const u32 h = (u32)(key * 0x9E3779B1u);
                const f32 t = 0.35f + (f32)(h & 0xFF) / 255.f * 0.30f;
                const glm::vec2 A{ (f32)a.center.x, (f32)a.center.z };
                const glm::vec2 B{ (f32)b.center.x, (f32)b.center.z };
                const glm::vec2 P = A + (B - A) * t;

                const f32 ddx = P.x - playerPos.x, ddz = P.y - playerPos.z;
                if (ddx * ddx + ddz * ddz > TRIGGER * TRIGGER) continue;

                // Сработала — выставляем разом и вокруг.
                u32 placed = 0;
                for (u32 i = 0; i < AMBUSH_SIZE; ++i) {
                    const f32 ang = (f32)i / (f32)AMBUSH_SIZE * 6.28318f;
                    const i32 bx = (i32)(playerPos.x + std::cos(ang) * 7.f);
                    const i32 bz = (i32)(playerPos.z + std::sin(ang) * 7.f);
                    if (!world.findChunk(bx >> 5, bz >> 5)) continue;
                    const i32 gy = world.generator().surfaceHeight(bx, bz);
                    if (world.getVoxel(bx, gy - 1, bz) == world::AIR) continue;

                    const u16 id = ((h >> (i * 2)) & 1) ? MOB_GOBLIN : MOB_SKELETON;
                    const glm::vec3 at{ (f32)bx + 0.5f, (f32)gy, (f32)bz + 0.5f };
                    if (spawnMob(world, reg, id, at).valid()) {
                        ++mobCount_;
                        ++placed;
                    }
                }
                // Засада считается сработавшей, только если из неё
                // кто-то вышел: иначе она сгорела бы впустую на
                // незагруженных чанках.
                if (placed > 0) sprung_.push_back(key);
            }
        }
}

bool Spawner::bossDefeated(u64 key) const {
    return std::binary_search(bossDefeated_.begin(), bossDefeated_.end(), key);
}

void Spawner::setDefeatedBosses(std::vector<u64> keys) {
    std::sort(keys.begin(), keys.end());
    keys.erase(std::unique(keys.begin(), keys.end()), keys.end());
    bossDefeated_ = std::move(keys);
}

void Spawner::watchBosses(ecs::Registry& reg) {
    // Каждый кадр: смерть длится полторы секунды до удаления тела, и
    // таймер расстановки (раз в три секунды) её бы пропускал.
    for (auto it = bossAlive_.begin(); it != bossAlive_.end();) {
        const ecs::Entity e = it->second;
        const auto* agent = reg.alive(e) ? reg.get<ecs::AIAgent>(e) : nullptr;
        if (agent && agent->state == ecs::AIAgent::Dead) {
            const auto pos = std::lower_bound(bossDefeated_.begin(), bossDefeated_.end(), it->first);
            if (pos == bossDefeated_.end() || *pos != it->first)
                bossDefeated_.insert(pos, it->first);
            it = bossAlive_.erase(it);
        } else if (!agent) {
            it = bossAlive_.erase(it);
        } else {
            ++it;
        }
    }
}

void Spawner::update(world::ChunkManager& world,
                     ecs::Registry& reg,
                     const glm::vec3& playerPos,
                     const world::DayCycle& day,
                     u64 worldSeed,
                     f32 dt)
{
    watchBosses(reg);
    updateBosses(world, reg, playerPos, worldSeed, dt);
    updateGarrison(world, reg, playerPos, worldSeed, dt);

    mobCount_ = (u32)reg.pool<MobTag>().size();

    const i32 pcx = (i32)std::floor(playerPos.x / world::CHUNK_SIZE);
    const i32 pcz = (i32)std::floor(playerPos.z / world::CHUNK_SIZE);

    spawnTimer_ += dt;
    if (spawnTimer_ >= 0.5f && mobCount_ < MAX_MOBS_TOTAL) {
        spawnTimer_ = 0.f;
        const bool night = day.isNight();

        for (int attempt = 0; attempt < 6; ++attempt) {
            const i32 dx = (i32)(urand() % 7) - 3;
            const i32 dz = (i32)(urand() % 7) - 3;
            const i32 cx = pcx + dx;
            const i32 cz = pcz + dz;

            world::ChunkCoord ck{ cx, cz };
            // Предел на чанк здесь общий; логово поднимает его ниже,
            // когда уже известно, что точка в него попала.
            if (perChunk_[ck] >= MAX_MOBS_PER_LAIR_CHUNK) continue;

            i32 sx = 0, sy = 0, sz = 0;
            if (!findSpawnSpot(world, cx, cz, sx, sy, sz)) continue;

            const f32 ddx = (f32)sx - playerPos.x;
            const f32 ddz = (f32)sz - playerPos.z;
            const f32 dd  = std::sqrt(ddx * ddx + ddz * ddz);
            if (dd < SPAWN_RADIUS_MIN || dd > SPAWN_RADIUS) continue;

            // Освещённость точки: при ярком свете враждебные мобы
            // не появляются даже ночью в закрытом помещении наоборот.
            const f32 light = lightAt(world, day, sx, sy, sz);
            const bool dark = light < 0.35f;

            // Логово: область, где таблица биома не действует вовсе.
            //
            // Обычный спавн ровен — волк и скелет встречаются везде
            // поровну, и идти куда-то за кем-то конкретным незачем. В
            // логове водится ровно один вид, и водится он днём тоже:
            // иначе логово днём — это просто поле.
            const world::LairSite lair = world::lairCovering(
                sx, sz, worldSeed, &world.generator());

            // Чёрный лес — такая же земля без дневной передышки, как
            // логово: нечисть там водится и при солнце.
            const bool blight =
                world.generator().biomeAt(sx, sz) == world::Blight;

            const u32 rv = urand();
            const u16 id = lair.exists
                ? lairMobId(lair.kind)
                : mobIdForBiome(world.generator().biomeAt(sx, sz),
                                night || dark, rv);
            if (id == MOB_NONE) continue;

            const MobDef& def = mobRegistry().get(id);
            if (def.isBoss) continue;   // боссов ставит только updateBosses
            // Дневные твари ставятся и при солнце: они не нежить,
            // и прятаться от света им незачем.
            if (def.hostile && !def.dayActive &&
                !dark && !night && !lair.exists && !blight) continue;

            // Густота: в логове зверья больше, чем в чистом поле, —
            // иначе «область, где водится волк» ничем не отличается
            // от поля, по которому изредка пробегает волк.
            const u32 cap = (lair.exists || blight) ? MAX_MOBS_PER_LAIR_CHUNK
                                                    : MAX_MOBS_PER_CHUNK;
            if (perChunk_[ck] >= cap) continue;

            const glm::vec3 pos { (f32)sx + 0.5f, (f32)sy, (f32)sz + 0.5f };
            if (spawnMob(world, reg, id, pos).valid()) {
                perChunk_[ck]++;
                ++mobCount_;
                break;
            }
        }
    }

    // ---- Засады ----
    //
    // Раз в полсекунды: чаще незачем, а реже — и игрок успеет
    // пройти место засады насквозь.
    ambushTimer_ += dt;
    if (ambushTimer_ >= 0.5f) {
        ambushTimer_ = 0.f;
        updateAmbushes(world, reg, playerPos, worldSeed);
    }

    despawnTimer_ += dt;
    if (despawnTimer_ >= 2.0f) {
        despawnTimer_ = 0.f;

        std::vector<ecs::Entity> toRemove;
        auto& pool = reg.pool<MobAI>();
        // Босс живёт в своём подземелье независимо от того, где игрок:
        // уходить и возвращаться, чтобы сбросить бой, нельзя.
        for (usize i = 0; i < pool.size(); ++i) {
            const ecs::Entity e = pool.entityAt((u32)i);
            if (const auto* t = reg.get<MobTag>(e); t && mobRegistry().get(t->id).isBoss) continue;
            auto* tf = reg.get<ecs::Transform>(e);
            if (!tf) continue;

            glm::vec3 d = tf->position - playerPos;
            d.y = 0.f;
            if (glm::length(d) > DESPAWN_RADIUS) {
                toRemove.push_back(e);
            }
        }
        for (auto e : toRemove) reg.destroy(e);

        std::erase_if(perChunk_, [&](auto& kv) {
            const i32 dx = kv.first.x - pcx;
            const i32 dz = kv.first.z - pcz;
            return (dx * dx + dz * dz) > 100;
        });
    }
}

} // namespace mobs
