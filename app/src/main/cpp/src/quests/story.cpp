/**
 * @file story.cpp
 * @brief Квесты: сюжетная цепочка — главы, идущие одна за другой.
 */
#include "story.h"
#include "quest.h"
#include "../world/features.h"
#include "../mobs/spawner.h"
#include "../ecs/components.h"
#include "../core/log.h"
#include <cstdio>
#include <cmath>

namespace quests {

namespace {

const StoryChapter CHAPTERS[STORY_CHAPTERS] = {
    { "Дорога к соседям",
      "Старик просит дойти до соседней деревни по каменной дороге и "
      "узнать, отчего оттуда никто не приходит.",
      QuestType::Explore },
    { "Тень на западе",
      "Говорят, за дорогой начинается лес, где не поют птицы. "
      "Дойти и посмотреть своими глазами.",
      QuestType::Explore },
    { "Замок среди сухостоя",
      "В глубине Чёрного леса стоит замок. Кто его строил, не помнит "
      "никто — но ворота там есть, и они открыты.",
      QuestType::Explore },
    { "Что спрятали под руинами",
      "Старые развалины помнят больше людей. Под ними замуровано то, "
      "за чем и стоило идти.",
      QuestType::Treasure },
    { "Логово",
      "Осталось последнее: земля, где водится один-единственный "
      "зверь. Разобраться с её хозяевами.",
      QuestType::Kill },
};

/// Награда главы. Растёт с номером: последняя глава должна стоить
/// больше первой, иначе цепочка не читается как путь.
QuestRewards chapterRewards(u8 chapter) {
    QuestRewards r{};
    r.xp   = 300ull * (chapter + 1) * (chapter + 1);
    r.gold = 80u * (chapter + 1);
    r.reputationDelta  = 12 + chapter * 4;
    r.reputationFaction = factions::FactionId::Villagers;
    return r;
}

} // namespace

const StoryChapter& storyChapter(u8 index) {
    if (index >= STORY_CHAPTERS) index = STORY_CHAPTERS - 1;
    return CHAPTERS[index];
}

bool storyFinished(ecs::Registry& reg, ecs::Entity player) {
    auto* sp = reg.get<StoryProgress>(player);
    return sp && sp->chapter >= STORY_CHAPTERS;
}

ecs::Entity offerFirstSteps(ecs::Registry& reg,
                            world::ChunkManager& world,
                            ecs::Entity player,
                            const glm::ivec3& around)
{
    auto* log = reg.get<QuestLog>(player);
    if (!log) return {};
    // Журнал не пуст — игроку есть чем заняться, и подсказывать
    // незачем. Это же условие бережёт от повторной выдачи.
    if (!log->activeQuests.empty()) return {};
    if (auto* sp = reg.get<StoryProgress>(player))
        if (sp->chapter > 0 || sp->activeId != 0) return {};

    const auto& gen = world.generator();
    const u64 seed = world.seed();

    Quest q{};
    q.id          = nextQuestId();
    q.state       = QuestState::Active;   // принимать не у кого
    q.ownerEntity = (u32)player;
    q.tmpl.type   = QuestType::Explore;
    q.tmpl.difficulty   = QuestDifficulty::Easy;
    q.tmpl.giverFaction = factions::FactionId::Villagers;
    q.tmpl.requiredCount = 1;
    q.rewards.xp   = 40;
    q.rewards.gold = 25;

    // Ближайшая деревня — ЛЮБАЯ, включая ту, рядом с которой игрок
    // мог родиться: цель здесь «найти людей», а не «дойти до
    // соседей».
    const i32 sc0x = (i32)std::floor((f32)around.x / (f32)world::SUPER_CHUNK_BLOCKS);
    const i32 sc0z = (i32)std::floor((f32)around.z / (f32)world::SUPER_CHUNK_BLOCKS);
    i64 best = 900LL * 900LL;
    bool targeted = false;
    for (i32 dz = -3; dz <= 3; ++dz)
        for (i32 dx = -3; dx <= 3; ++dx) {
            const auto v = world::villageAt(sc0x + dx, sc0z + dz, seed, &gen);
            if (!v.exists) continue;
            const i64 ddx = v.center.x - around.x;
            const i64 ddz = v.center.z - around.z;
            const i64 d2 = ddx * ddx + ddz * ddz;
            if (d2 < best) { best = d2; q.tmpl.targetLocation = v.center;
                             targeted = true; }
        }

    if (targeted) {
        q.tmpl.targetRadius = 16;
        std::snprintf(q.title, sizeof(q.title), "%s", "Найти людей");
        std::snprintf(q.description, sizeof(q.description), "%s",
                      "Где-то рядом есть деревня. Дойти до неё и "
                      "поговорить с теми, кто там живёт.");
    } else {
        // Людей рядом нет — тогда первое, что можно сделать голыми
        // руками: набрать дерева. С него начинается всё остальное.
        q.tmpl.type = QuestType::Collect;
        q.tmpl.targetBlockId = world::WOOD;
        q.tmpl.requiredCount = 4;
        std::snprintf(q.title, sizeof(q.title), "%s", "Первое дерево");
        std::snprintf(q.description, sizeof(q.description), "%s",
                      "Людей поблизости не видно. Набрать дерева — "
                      "с него начинается и топор, и всё остальное.");
    }

    const ecs::Entity qe = reg.create();
    reg.add(qe, q);
    log->addActive(qe);
    LOGI("First steps: %s", q.title);
    return qe;
}

ecs::Entity offerStoryChapter(ecs::Registry& reg,
                              world::ChunkManager& world,
                              ecs::Entity player,
                              const glm::ivec3& around)
{
    auto* sp = reg.get<StoryProgress>(player);
    if (!sp) {
        reg.add(player, StoryProgress{});
        sp = reg.get<StoryProgress>(player);
    }
    if (!sp) return {};
    if (sp->chapter >= STORY_CHAPTERS) return {};   // всё пройдено
    if (sp->activeId != 0) return {};               // глава уже на руках

    const u8 ch = sp->chapter;
    const StoryChapter& def = CHAPTERS[ch];
    const auto& gen = world.generator();
    const u64 seed = world.seed();

    Quest q{};
    q.id           = nextQuestId();
    q.isStory      = true;
    q.storyChapter = ch;
    q.state        = QuestState::Available;
    q.ownerEntity  = (u32)player;
    q.tmpl.type    = def.type;
    q.tmpl.difficulty = (QuestDifficulty)(u8)((ch < 4) ? ch + 1 : 4);
    q.tmpl.giverFaction = factions::FactionId::Villagers;
    q.rewards      = chapterRewards(ch);

    const i32 sc0x = (i32)std::floor((f32)around.x / (f32)world::SUPER_CHUNK_BLOCKS);
    const i32 sc0z = (i32)std::floor((f32)around.z / (f32)world::SUPER_CHUNK_BLOCKS);

    // Цель ищется В МИРЕ. Не нашлась — главу не выдаём: отправлять
    // игрока туда, где ничего нет, хуже, чем не дать задания.
    bool targeted = false;
    switch (ch) {
        case 0: {
            // Ближайшая деревня, кроме той, в которой стоим.
            i64 best = 900LL * 900LL;
            for (i32 dz = -3; dz <= 3; ++dz)
                for (i32 dx = -3; dx <= 3; ++dx) {
                    const auto v = world::villageAt(sc0x + dx, sc0z + dz,
                                                    seed, &gen);
                    if (!v.exists) continue;
                    const i64 ddx = v.center.x - around.x;
                    const i64 ddz = v.center.z - around.z;
                    const i64 d2 = ddx * ddx + ddz * ddz;
                    // Своя деревня не считается: до неё уже дошли.
                    if (d2 < 80LL * 80LL) continue;
                    if (d2 < best) { best = d2; q.tmpl.targetLocation = v.center;
                                     targeted = true; }
                }
            q.tmpl.targetRadius = 12;
            break;
        }
        case 1: {
            // Ближайший Чёрный лес. Ищем по сетке пореже: пятно
            // порчи в сотню блоков не проскочишь шагом в тридцать
            // два.
            i64 best = 1400LL * 1400LL;
            for (i32 dz = -44; dz <= 44; ++dz)
                for (i32 dx = -44; dx <= 44; ++dx) {
                    const i32 wx = around.x + dx * 32;
                    const i32 wz = around.z + dz * 32;
                    if (gen.biomeAt(wx, wz) != world::Blight) continue;
                    const i64 ddx = wx - around.x, ddz = wz - around.z;
                    const i64 d2 = ddx * ddx + ddz * ddz;
                    if (d2 < best) {
                        best = d2;
                        q.tmpl.targetLocation = { wx, gen.surfaceHeight(wx, wz), wz };
                        targeted = true;
                    }
                }
            q.tmpl.targetRadius = 16;
            break;
        }
        case 2: {
            // Круг поиска шире, чем у прочих глав, и намеренно:
            // замок стоит в одной ячейке из четырёх сотен, и в
            // тесном круге его попросту не окажется — глава тогда
            // не выдавалась бы никогда. Дорога к нему длинная, но
            // это третья глава, а не первая.
            i64 best = 3200LL * 3200LL;
            for (i32 dz = -12; dz <= 12; ++dz)
                for (i32 dx = -12; dx <= 12; ++dx) {
                    const auto c = world::castleAt(sc0x + dx, sc0z + dz,
                                                   seed, &gen);
                    if (!c.exists) continue;
                    const i64 ddx = c.center.x - around.x;
                    const i64 ddz = c.center.z - around.z;
                    const i64 d2 = ddx * ddx + ddz * ddz;
                    if (d2 < best) { best = d2; q.tmpl.targetLocation = c.center;
                                     targeted = true; }
                }
            q.tmpl.targetRadius = 20;
            break;
        }
        case 3: {
            const auto t = world::nearestTreasure(around, seed, &gen, 1400);
            if (t.exists) {
                q.tmpl.targetLocation = t.center;
                q.tmpl.targetRadius = 3;
                targeted = true;
            }
            break;
        }
        default: {
            // Логово: бить надо того, кто в нём водится.
            i64 best = 1200LL * 1200LL;
            world::LairKind kind = world::LairKind::None;
            for (i32 dz = -5; dz <= 5; ++dz)
                for (i32 dx = -5; dx <= 5; ++dx) {
                    const auto l = world::lairAt(sc0x + dx, sc0z + dz,
                                                 seed, &gen);
                    if (!l.exists) continue;
                    const i64 ddx = l.center.x - around.x;
                    const i64 ddz = l.center.z - around.z;
                    const i64 d2 = ddx * ddx + ddz * ddz;
                    if (d2 < best) { best = d2; kind = l.kind;
                                     q.tmpl.targetLocation = l.center; }
                }
            if (kind != world::LairKind::None) {
                q.tmpl.targetMobId = mobs::mobIdForBiome(world::Plains, true, 0);
                switch (kind) {
                    case world::LairKind::Wolves:    q.tmpl.targetMobId = mobs::MOB_WOLF; break;
                    case world::LairKind::Skeletons: q.tmpl.targetMobId = mobs::MOB_SKELETON; break;
                    case world::LairKind::Goblins:   q.tmpl.targetMobId = mobs::MOB_GOBLIN; break;
                    case world::LairKind::Slimes:    q.tmpl.targetMobId = mobs::MOB_SLIME; break;
                    default: break;
                }
                q.tmpl.requiredCount = 8;
                targeted = true;
            }
            break;
        }
    }
    if (!targeted) return {};

    std::snprintf(q.title, sizeof(q.title), "%s", def.title);
    if (q.tmpl.type == QuestType::Kill) {
        std::snprintf(q.description, sizeof(q.description),
                      "%s Нужно %d.", def.text, q.tmpl.requiredCount);
    } else {
        std::snprintf(q.description, sizeof(q.description),
                      "%s Идти к (%d, %d).", def.text,
                      q.tmpl.targetLocation.x, q.tmpl.targetLocation.z);
    }

    const ecs::Entity e = reg.create();
    reg.add(e, q);
    reg.add(e, ecs::Kind{ ecs::EntityKind::Item });
    sp->activeId = q.id;
    LOGI("Сюжет: выдана глава %u — %s", (unsigned)(ch + 1), q.title);
    return e;
}

bool completeStoryChapter(ecs::Registry& reg, ecs::Entity player, u32 questId) {
    auto* sp = reg.get<StoryProgress>(player);
    if (!sp || sp->activeId != questId || questId == 0) return false;
    sp->activeId = 0;
    if (sp->chapter < STORY_CHAPTERS) ++sp->chapter;
    return true;
}

} // namespace quests
