/**
 * @file save_player.cpp
 * @brief Сохранения: бинарный формат, сжатие, дельты мира, слоты.
 */
#include "save_player.h"
#include "save_inventory.h"
#include "../core/log.h"
#include "../ecs/components.h"
#include "../combat/components.h"
#include "../progression/progression.h"
#include "../progression/skill_tree.h"
#include "../quests/quest.h"
#include "../factions/faction.h"
#include <glm/glm.hpp>
#include <algorithm>

namespace save {

using namespace ecs;

namespace {

void writeQuat(ByteWriter& w, const glm::quat& q) {
    w.writeF32(q.x); w.writeF32(q.y); w.writeF32(q.z); w.writeF32(q.w);
}
void readQuat(ByteReader& r, glm::quat& q) {
    r.f32v(q.x); r.f32v(q.y); r.f32v(q.z); r.f32v(q.w);
}

void writeVec3(ByteWriter& w, const glm::vec3& v) {
    w.writeF32(v.x); w.writeF32(v.y); w.writeF32(v.z);
}
void readVec3(ByteReader& r, glm::vec3& v) {
    r.f32v(v.x); r.f32v(v.y); r.f32v(v.z);
}

// Phase 15: валидация атрибутов.
// Атрибуты не должны превышать ATTR_MAX (99) и не быть меньше ATTR_MIN (1).
void clampAttributes(ecs::Attributes& a) {
    constexpr i32 MIN_V = 1;
    constexpr i32 MAX_V = 99;
    a.strength     = std::clamp(a.strength,     MIN_V, MAX_V);
    a.agility      = std::clamp(a.agility,      MIN_V, MAX_V);
    a.intelligence = std::clamp(a.intelligence, MIN_V, MAX_V);
    a.endurance    = std::clamp(a.endurance,    MIN_V, MAX_V);
}

} // namespace

void serializePlayer(ByteWriter& w, ecs::Registry& reg, ecs::Entity player) {
    // ---------------- Transform ----------------
    if (auto* tf = reg.get<Transform>(player)) {
        w.writeU8(1);
        writeVec3(w, tf->position);
        writeQuat(w, tf->rotation);
        writeVec3(w, tf->scale);
    } else {
        w.writeU8(0);
    }

    if (auto* h = reg.get<Health>(player)) {
        w.writeU8(1);
        w.writeF32(h->current); w.writeF32(h->max); w.writeF32(h->regen);
    } else w.writeU8(0);

    if (auto* m = reg.get<Mana>(player)) {
        w.writeU8(1);
        w.writeF32(m->current); w.writeF32(m->max); w.writeF32(m->regen);
    } else w.writeU8(0);

    if (auto* s = reg.get<Stamina>(player)) {
        w.writeU8(1);
        w.writeF32(s->current); w.writeF32(s->max); w.writeF32(s->regen);
    } else w.writeU8(0);

    if (auto* a = reg.get<Attributes>(player)) {
        w.writeU8(1);
        w.writeI32(a->strength);
        w.writeI32(a->agility);
        w.writeI32(a->intelligence);
        w.writeI32(a->endurance);
    } else w.writeU8(0);

    if (auto* p = reg.get<progression::Progression>(player)) {
        w.writeU8(1);
        w.writeU64(p->xp);
        w.writeU32(p->level);
        w.writeI32(p->availableAttrPoints);
        w.writeU32(p->pendingLevelUps);
    } else w.writeU8(0);

    if (auto* t = reg.get<progression::SkillTree>(player)) {
        w.writeU8(1);
        w.writeI32(t->unspentPoints);
        w.writeI32(t->totalPointsEarned);
        const u16 n = (u16)progression::SkillNodeId::Count;
        w.varU32((u32)n);
        for (u16 i = 0; i < n; ++i) {
            w.writeU8(t->ranks[i]);
        }
    } else w.writeU8(0);

    if (auto* eq = reg.get<combat::EquippedWeapon>(player)) {
        w.writeU8(1);
        w.writeU16(eq->weaponId);
        w.writeU8((u8)eq->enchant.id);
        w.writeU8(eq->enchant.level);
    } else w.writeU8(0);

    if (auto* cmb = reg.get<combat::Combatant>(player)) {
        w.writeU8(1);
        w.writeU32(cmb->faction);
        w.writeF32(cmb->knockbackResist);
        w.writeF32(cmb->radius);
        w.writeF32(cmb->height);
        w.writeF32(cmb->resistance.physical);
        w.writeF32(cmb->resistance.fire);
        w.writeF32(cmb->resistance.frost);
        w.writeF32(cmb->resistance.shock);
        w.writeF32(cmb->resistance.poison);
        w.writeF32(cmb->resistance.arcane);
    } else w.writeU8(0);

    if (auto* rs = reg.get<combat::ResonanceState>(player)) {
        w.writeU8(1);
        w.writeF32(rs->value);
        w.writeI32(rs->stack);
    } else w.writeU8(0);

    if (auto* ql = reg.get<quests::QuestLog>(player)) {
        w.writeU8(1);
        w.varU32((u32)ql->activeQuests.size());
        for (auto qe : ql->activeQuests) {
            auto* q = reg.get<quests::Quest>(qe);
            if (!q) continue;
            w.writeU32(q->id);
            w.writeU8((u8)q->tmpl.type);
            w.writeU8((u8)q->tmpl.difficulty);
            w.writeU16(q->tmpl.targetMobId);
            w.writeU16(q->tmpl.targetBlockId);
            w.writeI32(q->tmpl.targetLocation.x);
            w.writeI32(q->tmpl.targetLocation.y);
            w.writeI32(q->tmpl.targetLocation.z);
            w.writeI32(q->tmpl.targetRadius);
            w.writeI32(q->tmpl.requiredCount);
            w.writeF32(q->tmpl.timeLimit);
            w.writeU8((u8)q->tmpl.giverFaction);

            w.writeU8((u8)q->state);
            w.writeI32(q->progress);
            w.writeF32(q->timeRemaining);
            w.writeU32(q->giverEntity);
            w.writeU32(q->ownerEntity);

            w.writeU64(q->rewards.xp);
            w.writeU32(q->rewards.gold);
            w.writeU16(q->rewards.itemBlockId);
            w.writeU8(q->rewards.itemCount);
            w.writeI32(q->rewards.reputationDelta);
            w.writeU8((u8)q->rewards.reputationFaction);

            w.cstr(q->title);
            w.cstr(q->description);
        }

        w.varU32((u32)ql->history.size());
        for (const auto& h : ql->history) {
            w.writeU32(h.questId);
            w.writeU8((u8)h.state);
            w.cstr(h.title);
        }
    } else w.writeU8(0);

    if (auto* rep = reg.get<factions::Reputation>(player)) {
        w.writeU8(1);
        const u8 n = (u8)factions::FactionId::Count;
        w.writeU8(n);
        for (u8 i = 0; i < n; ++i) {
            w.writeI32(rep->values[i]);
        }
    } else w.writeU8(0);

    serializeInventory(w, reg, player);
}

bool deserializePlayer(ByteReader& r, ecs::Registry& reg, ecs::Entity player) {
    {
        u8 has = 0;
        if (!r.u8v(has)) return false;
        if (has) {
            auto* tf = reg.get<Transform>(player);
            if (!tf) {
                Transform t;
                reg.add(player, t);
                tf = reg.get<Transform>(player);
            }
            if (!tf) return false;
            readVec3(r, tf->position);
            readQuat(r, tf->rotation);
            readVec3(r, tf->scale);
        }
    }

    {
        u8 has = 0;
        if (!r.u8v(has)) return false;
        if (has) {
            auto* h = reg.get<Health>(player);
            if (!h) { Health tmp; reg.add(player, tmp); h = reg.get<Health>(player); }
            if (h) { r.f32v(h->current); r.f32v(h->max); r.f32v(h->regen); }
        }
    }
    {
        u8 has = 0;
        if (!r.u8v(has)) return false;
        if (has) {
            auto* m = reg.get<Mana>(player);
            if (!m) { Mana tmp; reg.add(player, tmp); m = reg.get<Mana>(player); }
            if (m) { r.f32v(m->current); r.f32v(m->max); r.f32v(m->regen); }
        }
    }
    {
        u8 has = 0;
        if (!r.u8v(has)) return false;
        if (has) {
            auto* s = reg.get<Stamina>(player);
            if (!s) { Stamina tmp; reg.add(player, tmp); s = reg.get<Stamina>(player); }
            if (s) { r.f32v(s->current); r.f32v(s->max); r.f32v(s->regen); }
        }
    }

    {
        u8 has = 0;
        if (!r.u8v(has)) return false;
        if (has) {
            auto* a = reg.get<Attributes>(player);
            if (!a) { Attributes tmp; reg.add(player, tmp); a = reg.get<Attributes>(player); }
            if (a) {
                r.i32v(a->strength);
                r.i32v(a->agility);
                r.i32v(a->intelligence);
                r.i32v(a->endurance);
                // Phase 15: валидация
                clampAttributes(*a);
            }
        }
    }

    {
        u8 has = 0;
        if (!r.u8v(has)) return false;
        if (has) {
            auto* p = reg.get<progression::Progression>(player);
            if (!p) {
                progression::Progression tmp;
                reg.add(player, tmp);
                p = reg.get<progression::Progression>(player);
            }
            if (p) {
                r.u64v(p->xp);
                r.u32v(p->level);
                r.i32v(p->availableAttrPoints);
                r.u32v(p->pendingLevelUps);
                // Phase 15: валидация уровня
                if (p->level < 1) p->level = 1;
                if (p->level > progression::MAX_LEVEL) p->level = progression::MAX_LEVEL;
                p->derivedDirty = true;
            }
        }
    }

    {
        u8 has = 0;
        if (!r.u8v(has)) return false;
        if (has) {
            auto* t = reg.get<progression::SkillTree>(player);
            if (!t) {
                progression::SkillTree tmp;
                reg.add(player, tmp);
                t = reg.get<progression::SkillTree>(player);
            }
            if (t) {
                r.i32v(t->unspentPoints);
                r.i32v(t->totalPointsEarned);
                u32 n = 0;
                if (!r.varU32v(n)) return false;
                if (n > (u32)progression::SkillNodeId::Count) return false;
                for (u32 i = 0; i < n; ++i) {
                    u8 rank = 0;
                    if (!r.u8v(rank)) return false;
                    t->ranks[i] = rank;
                }
            }
        }
    }

    {
        u8 has = 0;
        if (!r.u8v(has)) return false;
        if (has) {
            auto* eq = reg.get<combat::EquippedWeapon>(player);
            if (!eq) {
                combat::EquippedWeapon tmp;
                reg.add(player, tmp);
                eq = reg.get<combat::EquippedWeapon>(player);
            }
            if (eq) {
                r.u16v(eq->weaponId);
                u8 enchId = 0, enchLvl = 0;
                r.u8v(enchId);
                r.u8v(enchLvl);
                eq->enchant.id = (combat::EnchantmentId)enchId;
                eq->enchant.level = enchLvl;
            }
        }
    }

    {
        u8 has = 0;
        if (!r.u8v(has)) return false;
        if (has) {
            auto* cmb = reg.get<combat::Combatant>(player);
            if (!cmb) {
                combat::Combatant tmp;
                reg.add(player, tmp);
                cmb = reg.get<combat::Combatant>(player);
            }
            if (cmb) {
                r.u32v(cmb->faction);
                r.f32v(cmb->knockbackResist);
                r.f32v(cmb->radius);
                r.f32v(cmb->height);
                r.f32v(cmb->resistance.physical);
                r.f32v(cmb->resistance.fire);
                r.f32v(cmb->resistance.frost);
                r.f32v(cmb->resistance.shock);
                r.f32v(cmb->resistance.poison);
                r.f32v(cmb->resistance.arcane);
            }
        }
    }

    {
        u8 has = 0;
        if (!r.u8v(has)) return false;
        if (has) {
            auto* rs = reg.get<combat::ResonanceState>(player);
            if (!rs) {
                combat::ResonanceState tmp;
                reg.add(player, tmp);
                rs = reg.get<combat::ResonanceState>(player);
            }
            if (rs) {
                r.f32v(rs->value);
                r.i32v(rs->stack);
            }
        }
    }

    {
        u8 has = 0;
        if (!r.u8v(has)) return false;
        if (has) {
            auto* ql = reg.get<quests::QuestLog>(player);
            if (!ql) {
                quests::QuestLog tmp;
                reg.add(player, tmp);
                ql = reg.get<quests::QuestLog>(player);
            }
            if (ql) {
                ql->activeQuests.clear();

                u32 activeCount = 0;
                if (!r.varU32v(activeCount)) return false;
                if (activeCount > 256) return false;

                for (u32 i = 0; i < activeCount; ++i) {
                    quests::Quest q{};
                    r.u32v(q.id);
                    u8 typeU = 0, diffU = 0;
                    r.u8v(typeU);
                    r.u8v(diffU);
                    q.tmpl.type = (quests::QuestType)typeU;
                    q.tmpl.difficulty = (quests::QuestDifficulty)diffU;
                    r.u16v(q.tmpl.targetMobId);
                    r.u16v(q.tmpl.targetBlockId);
                    r.i32v(q.tmpl.targetLocation.x);
                    r.i32v(q.tmpl.targetLocation.y);
                    r.i32v(q.tmpl.targetLocation.z);
                    r.i32v(q.tmpl.targetRadius);
                    r.i32v(q.tmpl.requiredCount);
                    r.f32v(q.tmpl.timeLimit);
                    u8 giverFaction = 0;
                    r.u8v(giverFaction);
                    q.tmpl.giverFaction = (factions::FactionId)giverFaction;

                    u8 stateU = 0;
                    r.u8v(stateU);
                    q.state = (quests::QuestState)stateU;
                    r.i32v(q.progress);
                    r.f32v(q.timeRemaining);
                    r.u32v(q.giverEntity);
                    r.u32v(q.ownerEntity);

                    r.u64v(q.rewards.xp);
                    r.u32v(q.rewards.gold);
                    r.u16v(q.rewards.itemBlockId);
                    r.u8v(q.rewards.itemCount);
                    r.i32v(q.rewards.reputationDelta);
                    u8 rewardFaction = 0;
                    r.u8v(rewardFaction);
                    q.rewards.reputationFaction = (factions::FactionId)rewardFaction;

                    std::string title, desc;
                    r.strv(title);
                    r.strv(desc);
                    std::snprintf(q.title, sizeof(q.title), "%s", title.c_str());
                    std::snprintf(q.description, sizeof(q.description), "%s", desc.c_str());

                    ecs::Entity qe = reg.create();
                    reg.add(qe, q);
                    reg.add(qe, ecs::Kind{ ecs::EntityKind::Item });
                    ql->activeQuests.push_back(qe);
                }

                ql->history.clear();
                u32 historyCount = 0;
                if (!r.varU32v(historyCount)) return false;
                if (historyCount > 256) return false;

                for (u32 i = 0; i < historyCount; ++i) {
                    quests::QuestLog::HistoryEntry h{};
                    r.u32v(h.questId);
                    u8 st = 0;
                    r.u8v(st);
                    h.state = (quests::QuestState)st;
                    std::string title;
                    r.strv(title);
                    std::snprintf(h.title, sizeof(h.title), "%s", title.c_str());
                    ql->history.push_back(h);
                }
            }
        }
    }

    {
        u8 has = 0;
        if (!r.u8v(has)) return false;
        if (has) {
            auto* rep = reg.get<factions::Reputation>(player);
            if (!rep) {
                factions::Reputation tmp;
                reg.add(player, tmp);
                rep = reg.get<factions::Reputation>(player);
            }
            if (rep) {
                u8 n = 0;
                if (!r.u8v(n)) return false;
                if (n > (u8)factions::FactionId::Count) return false;
                for (u8 i = 0; i < n; ++i) {
                    r.i32v(rep->values[i]);
                }
            }
        }
    }

    if (!deserializeInventory(r, reg, player)) return false;

    return r.ok();
}

} // namespace save
