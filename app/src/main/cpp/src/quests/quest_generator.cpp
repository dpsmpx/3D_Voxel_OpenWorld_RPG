/**
 * @file quest_generator.cpp
 * @brief Квесты: шаблоны, процедурная генерация, журнал заданий.
 */
#include "quest_generator.h"
#include "quest.h"
#include "../world/block.h"
#include "../ecs/components.h"
#include "../progression/progression.h"
#include "../progression/resource_regen.h"
#include "../world/biome.h"
#include "../world/terrain.h"
#include "../core/log.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace quests {

namespace {

// Детерминированный PRNG (xorshift64*)
struct GenRng {
    u64 s;
    explicit GenRng(u64 seed) : s(seed ? seed : 0x9E3779B97F4A7C15ULL) {}
    u32 next() {
        s ^= s >> 12;
        s ^= s << 25;
        s ^= s >> 27;
        return (u32)((s * 0x2545F4914F6CDD1DULL) >> 32);
    }
    u32 range(u32 lo, u32 hi) {
        if (hi <= lo) return lo;
        return lo + (next() % (hi - lo + 1));
    }
    i32 rangeI(i32 lo, i32 hi) {
        if (hi <= lo) return lo;
        return lo + (i32)(next() % (u32)(hi - lo + 1));
    }
    f32 f01() { return (f32)(next() & 0xFFFFFF) / (f32)0x1000000; }
};

void hashStr(char* out, usize outSize, const char* fmt,
             const char* a, i32 n, const char* b)
{
    std::snprintf(out, outSize, fmt,
                  a ? a : "", n, b ? b : "");
}

} // namespace

QuestDifficulty pickDifficulty(u32 playerLevel, u64 seed) {
    GenRng rng(seed ^ 0xD1FF1C);

    // Уровень 1-3  → Trivial / Easy
    // Уровень 4-8  → Easy / Normal
    // Уровень 9-20 → Normal / Hard
    // Уровень 21+  → Hard / Epic
    u32 roll = rng.next() % 100;

    if (playerLevel <= 3) {
        return (roll < 60) ? QuestDifficulty::Trivial : QuestDifficulty::Easy;
    }
    if (playerLevel <= 8) {
        if (roll < 30) return QuestDifficulty::Trivial;
        if (roll < 80) return QuestDifficulty::Easy;
        return QuestDifficulty::Normal;
    }
    if (playerLevel <= 20) {
        if (roll < 20) return QuestDifficulty::Easy;
        if (roll < 65) return QuestDifficulty::Normal;
        if (roll < 90) return QuestDifficulty::Hard;
        return QuestDifficulty::Epic;
    }
    // 21+
    if (roll < 10) return QuestDifficulty::Normal;
    if (roll < 55) return QuestDifficulty::Hard;
    return QuestDifficulty::Epic;
}

QuestType pickType(const QuestGenOptions& o, u64 seed) {
    // Собираем список допустимых типов и выбираем взвешенно.
    struct Opt { QuestType t; u32 weight; };
    Opt list[5];
    u32 n = 0;

    // Kill — самый частый в деревнях (стражники)
    if (o.allowKill)    list[n++] = { QuestType::Kill,    5 };

    // Collect — очень частый у торговцев
    if (o.allowCollect) list[n++] = { QuestType::Collect, 4 };

    // Explore — средний
    if (o.allowExplore) list[n++] = { QuestType::Explore, 2 };

    if (o.allowDefend)  list[n++] = { QuestType::Defend,  1 };
    if (o.allowDeliver) list[n++] = { QuestType::Deliver, 1 };

    if (n == 0) return QuestType::Collect;

    u32 total = 0;
    for (u32 i = 0; i < n; ++i) total += list[i].weight;

    GenRng rng(seed ^ 0xABCDEF);
    u32 roll = rng.next() % total;

    u32 acc = 0;
    for (u32 i = 0; i < n; ++i) {
        acc += list[i].weight;
        if (roll < acc) return list[i].t;
    }
    return list[0].t;
}

u16 pickMobId(world::ChunkManager& world,
              const glm::ivec3& giverPos,
              const QuestGenOptions& options, u64 seed)
{
    using namespace mobs;

    // Биом вокруг NPC
    world::BiomeId biome = world.generator().biomeAt(giverPos.x, giverPos.z);

    // Допустимые мобы по биому
    u16 pool[6] = {};
    u32 n = 0;

    auto addPool = [&](u16 id) {
        for (u32 i = 0; i < n; ++i) if (pool[i] == id) return;
        if (n < 6) pool[n++] = id;
    };

    switch (biome) {
        case world::Ocean:
            return MOB_NONE;
        case world::Beach:
            addPool(MOB_CHICKEN);
            addPool(MOB_SHEEP);
            break;
        case world::Plains:
            addPool(MOB_SHEEP);
            addPool(MOB_COW);
            addPool(MOB_WOLF);
            addPool(MOB_GOBLIN);
            break;
        case world::Forest:
            addPool(MOB_WOLF);
            addPool(MOB_GOBLIN);
            addPool(MOB_SKELETON);
            break;
        case world::Taiga:
            addPool(MOB_WOLF);
            addPool(MOB_SKELETON);
            break;
        case world::Desert:
            addPool(MOB_SKELETON);
            addPool(MOB_CHICKEN);
            break;
        case world::Savanna:
            addPool(MOB_GOBLIN);
            addPool(MOB_COW);
            break;
        case world::Tundra:
            addPool(MOB_SHEEP);
            addPool(MOB_WOLF);
            break;
        case world::Mountains:
            addPool(MOB_GOBLIN);
            break;
        case world::Swamp:
            addPool(MOB_SLIME);
            addPool(MOB_WOLF);
            break;
        case world::Volcanic:
            addPool(MOB_SLIME);
            break;
        default:
            return MOB_SHEEP;
    }

    if (n == 0) return MOB_SHEEP;

    GenRng rng(seed ^ 0xBEEF);
    return pool[rng.next() % n];
}

u16 pickBlockId(world::ChunkManager& world,
                const glm::ivec3& giverPos, u64 seed)
{
    using namespace world;

    BiomeId biome = world.generator().biomeAt(giverPos.x, giverPos.z);

    // Набор «доступных для сбора» блоков — те, что видны рядом.
    u16 pool[6] = {};
    u32 n = 0;
    auto addPool = [&](u16 id) {
        for (u32 i = 0; i < n; ++i) if (pool[i] == id) return;
        if (n < 6) pool[n++] = id;
    };

    switch (biome) {
        case world::Ocean:
        case world::Beach:
            addPool(SAND);
            addPool(STONE);
            break;
        case world::Plains:
        case world::Forest:
            addPool(WOOD);
            addPool(DIRT);
            addPool(STONE);
            addPool(IRON_ORE);
            break;
        case world::Taiga:
        case world::Tundra:
            addPool(SNOW);
            addPool(WOOD);
            addPool(STONE);
            addPool(IRON_ORE);
            break;
        case world::Desert:
            addPool(SAND);
            addPool(STONE);
            addPool(GOLD_ORE);
            break;
        case world::Savanna:
            addPool(WOOD);
            addPool(STONE);
            addPool(IRON_ORE);
            break;
        case world::Mountains:
            addPool(STONE);
            addPool(IRON_ORE);
            addPool(GOLD_ORE);
            break;
        case world::Swamp:
            addPool(DIRT);
            addPool(WOOD);
            break;
        case world::Volcanic:
            addPool(STONE);
            addPool(GOLD_ORE);
            break;
        default:
            addPool(STONE);
    }

    if (n == 0) return STONE;

    GenRng rng(seed ^ 0xC0DE);
    return pool[rng.next() % n];
}

glm::ivec3 pickExploreLocation(world::ChunkManager& world,
                               const glm::ivec3& giverPos,
                               QuestDifficulty diff, u64 seed)
{
    GenRng rng(seed ^ 0xE7F10DE);

    // Радиус поиска зависит от сложности
    i32 minR = 40, maxR = 80;
    switch (diff) {
        case QuestDifficulty::Trivial: minR = 30; maxR = 60;  break;
        case QuestDifficulty::Easy:    minR = 60; maxR = 120; break;
        case QuestDifficulty::Normal:  minR = 100; maxR = 200; break;
        case QuestDifficulty::Hard:    minR = 180; maxR = 340; break;
        case QuestDifficulty::Epic:    minR = 300; maxR = 600; break;
        default: break;
    }

    // Угол
    f32 angle = rng.f01() * 6.2831853f;
    i32 r = rng.rangeI(minR, maxR);

    i32 dx = (i32)(std::cos(angle) * (f32)r);
    i32 dz = (i32)(std::sin(angle) * (f32)r);

    i32 tx = giverPos.x + dx;
    i32 tz = giverPos.z + dz;

    // Ищем поверхность
    i32 ty = world.generator().surfaceHeight(tx, tz);

    return glm::ivec3{ tx, ty, tz };
}

void finalizeQuest(Quest& q,
                   const QuestGenOptions& options,
                   world::ChunkManager& world)
{
    const auto& tmplDef = questTemplates().get(q.tmpl.type, q.tmpl.difficulty);

    GenRng rng(options.seed ^ 0x0F1A1);

    // --- Количество ---
    i32 count = rng.rangeI(tmplDef.minCount, tmplDef.maxCount);
    q.tmpl.requiredCount = count;

    // --- Время (для Defend / ограничение) ---
    if (q.tmpl.type == QuestType::Defend) {
        q.tmpl.timeLimit = (f32)count;
    } else if (q.tmpl.type == QuestType::Kill ||
               q.tmpl.type == QuestType::Collect) {
        // 30 минут базово + 2 минуты на требуемый юнит
        q.tmpl.timeLimit = 1800.f + (f32)count * 120.f;
    }

    // --- Награды ---
    u32 diffVal = questDifficultyValue(q.tmpl.difficulty);
    u64 xp   = tmplDef.baseXP   * diffVal / 3;
    u32 gold = tmplDef.baseGold * diffVal / 3;

    // Бонус за репутацию с фракцией
    auto mods = factions::modifiersFor(options.repTier);
    xp   = (u64)((f32)xp   * mods.rewardMult);
    gold = (u32)((f32)gold * mods.rewardMult);

    q.rewards.xp              = xp;
    q.rewards.gold            = gold;
    q.rewards.reputationDelta = tmplDef.baseRep;
    q.rewards.reputationFaction = options.faction;

    // Предметы как награда — Phase 12 (инвентарь). Пока — блок.
    if (q.tmpl.difficulty >= QuestDifficulty::Hard) {
        q.rewards.itemBlockId = world::IRON_ORE;
        q.rewards.itemCount   = 1 + (u8)(rng.next() % 3);
    }

    // --- Title / Description ---
    switch (q.tmpl.type) {
        case QuestType::Kill: {
            const auto& mob = mobs::mobRegistry().get(q.tmpl.targetMobId);
            hashStr(q.title, sizeof(q.title),
                    tmplDef.titlePattern, mob.name, 0, "");
            hashStr(q.description, sizeof(q.description),
                    tmplDef.descPattern, mob.name, count, "");
            break;
        }
        case QuestType::Collect: {
            const char* blockName = "materials";
            // Упрощённо — используем имя блока по ID
            // В реальном проекте — BlockRegistry::get(id).name
            static const char* names[] = {
                "air","stone","dirt","grass","sand","water","wood",
                "leaves","snow","ice","lava","iron ore","gold ore","bedrock"
            };
            if (q.tmpl.targetBlockId < 14) blockName = names[q.tmpl.targetBlockId];
            hashStr(q.title, sizeof(q.title),
                    tmplDef.titlePattern, "", count, blockName);
            hashStr(q.description, sizeof(q.description),
                    tmplDef.descPattern, "", count, blockName);
            break;
        }
        case QuestType::Explore: {
            std::snprintf(q.title, sizeof(q.title), "%s", tmplDef.titlePattern);
            std::snprintf(q.description, sizeof(q.description),
                          "%s Target: (%d, %d, %d).",
                          tmplDef.descPattern,
                          q.tmpl.targetLocation.x,
                          q.tmpl.targetLocation.y,
                          q.tmpl.targetLocation.z);
            break;
        }
        case QuestType::Defend: {
            std::snprintf(q.title, sizeof(q.title), "%s", tmplDef.titlePattern);
            std::snprintf(q.description, sizeof(q.description),
                          tmplDef.descPattern, count);
            break;
        }
        case QuestType::Deliver: {
            std::snprintf(q.title, sizeof(q.title), "%s", tmplDef.titlePattern);
            std::snprintf(q.description, sizeof(q.description), "%s",
                          tmplDef.descPattern);
            break;
        }
        default: break;
    }
}

ecs::Entity generateQuest(ecs::Registry& reg,
                          world::ChunkManager& world,
                          const QuestGenOptions& options)
{
    GenRng rng(options.seed ^ 0x5A17E);

    Quest q{};
    q.id             = nextQuestId();
    q.tmpl.giverFaction = options.faction;
    q.giverEntity    = options.giverEntity;
    q.ownerEntity    = options.ownerEntity;
    q.state          = QuestState::Available;

    q.tmpl.difficulty = pickDifficulty(options.playerLevel, rng.next());
    q.tmpl.type       = pickType(options, rng.next());

    // --- Заполняем специфичные поля шаблона ---
    switch (q.tmpl.type) {
        case QuestType::Kill: {
            u16 mobId = pickMobId(world, options.giverPos, options, rng.next());
            if (mobId == mobs::MOB_NONE) {
                // Fallback — сделать Collect
                q.tmpl.type = QuestType::Collect;
                q.tmpl.targetBlockId = pickBlockId(world, options.giverPos, rng.next());
            } else {
                q.tmpl.targetMobId = mobId;
            }
            break;
        }
        case QuestType::Collect: {
            q.tmpl.targetBlockId = pickBlockId(world, options.giverPos, rng.next());
            break;
        }
        case QuestType::Explore: {
            q.tmpl.targetLocation = pickExploreLocation(
                world, options.giverPos, q.tmpl.difficulty, rng.next());
            q.tmpl.targetRadius = 6;
            break;
        }
        case QuestType::Defend: {
            q.tmpl.targetLocation = options.giverPos;
            break;
        }
        case QuestType::Deliver: {
            q.tmpl.targetLocation = pickExploreLocation(
                world, options.giverPos, q.tmpl.difficulty, rng.next());
            break;
        }
        default: break;
    }

    finalizeQuest(q, options, world);

    // --- Создаём ECS-сущность ---
    ecs::Entity e = reg.create();
    reg.add(e, q);
    reg.add(e, ecs::Kind{ ecs::EntityKind::Item }); // «неживая» сущность

    return e;
}

bool grantRewards(ecs::Registry& reg, u32 playerEntity, const Quest& q) {
    // XP
    if (q.rewards.xp > 0) {
        auto* prog = reg.get<progression::Progression>(playerEntity);
        if (prog) {
            u32 gained = 0;
            prog->addXP(q.rewards.xp, gained);
        }
    }

    // Репутация
    if (q.rewards.reputationDelta != 0 &&
        q.rewards.reputationFaction != factions::FactionId::None)
    {
        auto* rep = reg.get<factions::Reputation>(playerEntity);
        if (rep) {
            rep->add(q.rewards.reputationFaction,
                     q.rewards.reputationDelta);
        }
    }

    // Золото и предметы — Phase 12 (инвентарь).
    // Пока только XP и репутация.

    return true;
}

} // namespace quests
