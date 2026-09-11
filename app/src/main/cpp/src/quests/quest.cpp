/**
 * @file quest.cpp
 * @brief Квесты: шаблоны, процедурная генерация, журнал заданий.
 */
#include "quest.h"
#include "../progression/progression.h"
#include "../progression/resource_regen.h"
#include "../ecs/components.h"
#include "../core/log.h"
#include <atomic>
#include <cmath>
#include <cstring>

namespace quests {

namespace {
std::atomic<u32> gQuestIdCounter{ 1 };
}

u32 nextQuestId() {
    return gQuestIdCounter.fetch_add(1, std::memory_order_relaxed);
}

void QuestLog::addActive(ecs::Entity quest) {
    for (auto e : activeQuests) if (e == quest) return;
    activeQuests.push_back(quest);
}

void QuestLog::removeActive(ecs::Entity quest) {
    for (auto it = activeQuests.begin(); it != activeQuests.end(); ++it) {
        if (*it == quest) { activeQuests.erase(it); return; }
    }
}

void QuestLog::addHistory(u32 questId, QuestState state, const char* title) {
    HistoryEntry h{};
    h.questId = questId;
    h.state   = state;
    if (title) {
        std::strncpy(h.title, title, sizeof(h.title) - 1);
        h.title[sizeof(h.title) - 1] = '\0';
    }
    history.push_back(h);
    if (history.size() > 30) {
        history.erase(history.begin());
    }
}

void QuestLog::clear() {
    activeQuests.clear();
    history.clear();
}

// ============================================================
// Прогресс
// ============================================================

// Внутренний апдейт: если progress достиг requiredCount — Completed.
static void checkCompletion(Quest& q) {
    if (q.state != QuestState::Active) return;
    if (q.tmpl.requiredCount <= 0) return;
    if (q.progress >= q.tmpl.requiredCount) {
        q.state = QuestState::Completed;
    }
}

bool notifyMobKilled(ecs::Registry& reg, u32 playerEntity,
                     u16 mobId, i32 amount)
{
    auto* log = reg.get<QuestLog>(playerEntity);
    if (!log) return false;

    bool changed = false;

    for (ecs::Entity qe : log->activeQuests) {
        auto* q = reg.get<Quest>(qe);
        if (!q) continue;
        if (q->state != QuestState::Active) continue;
        if (q->tmpl.type != QuestType::Kill) continue;
        if (q->tmpl.targetMobId != mobId) continue;

        q->progress += amount;
        checkCompletion(*q);
        changed = true;
    }
    return changed;
}

bool notifyItemCollected(ecs::Registry& reg, u32 playerEntity,
                         u16 blockId, i32 amount)
{
    auto* log = reg.get<QuestLog>(playerEntity);
    if (!log) return false;

    bool changed = false;

    for (ecs::Entity qe : log->activeQuests) {
        auto* q = reg.get<Quest>(qe);
        if (!q) continue;
        if (q->state != QuestState::Active) continue;
        if (q->tmpl.type != QuestType::Collect) continue;
        if (q->tmpl.targetBlockId != blockId) continue;

        q->progress += amount;
        checkCompletion(*q);
        changed = true;
    }
    return changed;
}

bool notifyLocationReached(ecs::Registry& reg, u32 playerEntity,
                           const glm::ivec3& position)
{
    auto* log = reg.get<QuestLog>(playerEntity);
    if (!log) return false;

    bool changed = false;

    for (ecs::Entity qe : log->activeQuests) {
        auto* q = reg.get<Quest>(qe);
        if (!q) continue;
        if (q->state != QuestState::Active) continue;
        if (q->tmpl.type != QuestType::Explore) continue;

        const glm::ivec3& t = q->tmpl.targetLocation;
        i32 dx = position.x - t.x;
        i32 dz = position.z - t.z;
        i32 dy = position.y - t.y;
        f32 distSq = (f32)(dx*dx + dy*dy + dz*dz);
        f32 radius = (f32)q->tmpl.targetRadius;

        if (distSq <= radius * radius) {
            q->progress = q->tmpl.requiredCount;
            checkCompletion(*q);
            changed = true;
        }
    }
    return changed;
}

bool tickQuestTime(ecs::Registry& reg, u32 playerEntity, f32 dt) {
    auto* log = reg.get<QuestLog>(playerEntity);
    if (!log) return false;

    bool changed = false;

    for (ecs::Entity qe : log->activeQuests) {
        auto* q = reg.get<Quest>(qe);
        if (!q) continue;
        if (q->state != QuestState::Active) continue;
        if (q->tmpl.timeLimit <= 0.f) continue;

        q->timeRemaining -= dt;
        if (q->timeRemaining <= 0.f) {
            q->timeRemaining = 0.f;
            q->state = QuestState::Failed;
            changed = true;
        }
    }
    return changed;
}

bool isReadyToTurnIn(ecs::Registry& reg, ecs::Entity questEntity) {
    auto* q = reg.get<Quest>(questEntity);
    if (!q) return false;
    return q->state == QuestState::Completed;
}

} // namespace quests
