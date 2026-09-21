/**
 * @file quest.cpp
 * @brief Квесты: шаблоны, процедурная генерация, журнал заданий.
 */
#include "quest.h"
#include "../items/inventory.h"
#include "../items/item_def.h"
#include "../progression/progression.h"
#include "../progression/resource_regen.h"
#include "../ecs/components.h"
#include "../core/log.h"
#include <atomic>
#include <cmath>
#include <cstring>

#include <algorithm>

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

void QuestLog::addHistory(const Quest& q) {
    history.push_back(q);
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

/// Какой предмет соответствует цели «принеси этот блок».
static u16 carriedItemOf(const Quest& q) {
    if (q.tmpl.type != QuestType::Collect) return items::ITEM_NONE;
    return items::items().blockToItem(q.tmpl.targetBlockId);
}

bool syncCarriedProgress(ecs::Registry& reg, u32 playerEntity) {
    auto* log = reg.get<QuestLog>(playerEntity);
    const auto* inv = reg.get<items::Inventory>(playerEntity);
    if (!log || !inv) return false;

    bool changed = false;

    for (ecs::Entity qe : log->activeQuests) {
        auto* q = reg.get<Quest>(qe);
        if (!q) continue;
        if (q->tmpl.type != QuestType::Collect) continue;
        if (q->state != QuestState::Active &&
            q->state != QuestState::Completed) continue;

        const u16 itemId = carriedItemOf(*q);
        if (itemId == items::ITEM_NONE) continue;

        const i32 have = (i32)std::min<u32>((u32)q->tmpl.requiredCount,
                                            inv->countOf(itemId));
        if (q->progress == have) continue;

        q->progress = have;
        changed = true;

        // Состояние ходит в ОБЕ стороны. Потратил принесённое — цель
        // снова не выполнена, и журнал обязан это сказать, а не
        // держать зелёную галочку над пустой сумкой.
        if (have >= q->tmpl.requiredCount) {
            if (q->state == QuestState::Active) q->state = QuestState::Completed;
        } else if (q->state == QuestState::Completed) {
            q->state = QuestState::Active;
        }
    }
    return changed;
}

bool consumeCarried(ecs::Registry& reg, u32 playerEntity, const Quest& q) {
    if (q.tmpl.type != QuestType::Collect) return true;

    auto* inv = reg.get<items::Inventory>(playerEntity);
    if (!inv) return false;

    const u16 itemId = carriedItemOf(q);
    if (itemId == items::ITEM_NONE) return false;

    const u16 need = (u16)std::max(0, q.tmpl.requiredCount);
    if (inv->countOf(itemId) < need) return false;

    return inv->removeItem(itemId, need) == need;
}

bool notifyItemCollected(ecs::Registry& reg, u32 playerEntity,
                         u16 blockId, i32 amount)
{
    // Прогресс «принеси» считает syncCarriedProgress по содержимому
    // сумки — там единственный источник истины. Здесь осталась
    // только немедленная сверка, чтобы счётчик не ждал следующего
    // кадра; складывать события сюда больше нельзя.
    (void)blockId;
    (void)amount;
    return syncCarriedProgress(reg, playerEntity);
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
