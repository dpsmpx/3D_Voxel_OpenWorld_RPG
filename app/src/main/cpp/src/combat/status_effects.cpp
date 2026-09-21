/**
 * @file status_effects.cpp
 * @brief Бой: урон, оружие, зачарования, система «Резонанс», статусы.
 */
#include "status_effects.h"
#include "projectile.h"
#include "guard.h"
#include "focus.h"
#include "hurt_marks.h"
#include "../ecs/components.h"
#include "../mobs/mob_def.h"
#include "../mobs/mob_ai.h"
#include "../progression/progression.h"
#include "../factions/faction.h"
#include "../npc/npc_def.h"
#include "../quests/quest.h"
#include "../audio/audio_events.h"
#include "../entity/creature_info.h"
#include "../world/particles.h"
#include <algorithm>

namespace combat {

void applyStatuses(StatusEffects& se, const DamageInstance& dmg) {
    if (dmg.burnTime > 0.f) {
        se.burnTime = std::min(10.f, se.burnTime + dmg.burnTime);
        se.burnDps = std::max(se.burnDps, dmg.amount * 0.10f);
    }

    if (dmg.slowDuration > 0.f) {
        se.slowTime = std::max(se.slowTime, dmg.slowDuration);
        se.slowAmount = std::max(se.slowAmount, dmg.slowAmount);
    }

    if (dmg.stunDuration > 0.f) {
        se.stunTime = std::max(se.stunTime, dmg.stunDuration);
    }

    if (dmg.poisonTime > 0.f) {
        se.poisonTime = std::max(se.poisonTime, dmg.poisonTime);
        se.poisonDps  = std::max(se.poisonDps, dmg.poisonDps);
    }

    if (dmg.sourceEntity != 0) {
        se.lastAttacker = dmg.sourceEntity;
    }

    se.flashTimer = std::max(se.flashTimer, 0.15f);
}

// Внутренний хелпер: награда XP при смерти.
static void onTargetDeath(ecs::Registry& reg,
                          ecs::Entity target,
                          u32 killerEntity)
{
    if (killerEntity == 0) return;

    auto* prog = reg.get<progression::Progression>(killerEntity);
    if (!prog) return;

    u64 xpReward = 0;
    if (auto* tag = reg.get<mobs::MobTag>(target)) {
        const auto& def = mobs::mobRegistry().get(tag->id);
        xpReward = def.xpReward;

        // Цели вида «убить N таких-то» отмечаются здесь же, где
        // начисляется опыт: это единственное место, которое знает и
        // убийцу, и вид убитого, и срабатывает ровно один раз.
        // Функция quests::notifyMobKilled существовала, но её никто
        // не вызывал — а на такие цели приходится большинство
        // выдаваемых квестов, и счётчик у них навсегда оставался в
        // нуле. Проверка на Progression выше заодно отсекает мобов,
        // убивающих друг друга: квесты считают только игрока.
        quests::notifyMobKilled(reg, killerEntity, tag->id);
    }

    if (xpReward > 0) {
        progression::rewardKillXP(reg, reg.fromId(killerEntity), xpReward);
    }

    // Убитый житель стоит репутации у своей фракции. Раньше не стоил
    // ничего: Reputation::add звали из одного места — награды за
    // квест, и только в плюс. Проверка на Progression выше отсекает
    // мобов и NPC, убивающих друг друга: платит только игрок.
    if (auto* npcTag = reg.get<npc::NpcTag>(target)) {
        const auto& ndef = npc::npcRegistry().get(npcTag->id);
        if (ndef.faction != factions::FactionId::None) {
            if (auto* rep = reg.get<factions::Reputation>(killerEntity)) {
                rep->add(ndef.faction, -factions::REP_MURDER_PENALTY);
            }
        }
    }
}

/// Гибель цели — одно место на все способы умереть.
///
/// Умереть можно от удара, от горения и от яда, и раньше каждый из
/// трёх путей сам выставлял состояние и сам звал награду. Три копии
/// одного правила — это три места, где можно забыть добавить
/// четвёртое; развязка стычки как раз и была таким забытым.
static void killTarget(ecs::Registry& reg, ecs::Entity target,
                       u32 killerEntity)
{
    auto* agent = reg.get<ecs::AIAgent>(target);
    const bool alreadyDead = agent && agent->state == ecs::AIAgent::Dead;
    if (agent) agent->state = ecs::AIAgent::Dead;

    // Умереть можно ОДИН раз, и всё, что ниже, полагается один раз.
    //
    // Труп лежит полторы секунды, прежде чем исчезнуть, и всё это
    // время остаётся целью для поиска попаданий: он не перестал быть
    // сущностью с телом и здоровьем. Второй удар по нему снова
    // доводил здоровье до нуля — и снова начислял опыт, снова двигал
    // счётчик квеста «убить N таких-то» и снова ронял репутацию за
    // убийство. Стая, молотящая павшего, выдавала за него награду
    // столько раз, сколько успевала ударить.
    //
    // Найдено мутацией: она сняла эту проверку, и ни один тест не
    // заметил — потому что тест бил по трупу, пока на нём ещё стояли
    // кадры неуязвимости от первого удара.
    if (alreadyDead) return;

    // Существо рассыпается СОБСТВЕННЫМ цветом: по горсти видно, кого
    // именно добили, — и это единственное, чем развязка стычки
    // отличалась от исчезновения полоски здоровья.
    //
    // Игрок исключён намеренно: его смерть показывает экран, а
    // горсть осколков в упор перед камерой — это помеха, а не
    // развязка.
    if (!reg.has<ecs::PlayerTag>(target)) {
        if (auto* tf = reg.get<ecs::Transform>(target)) {
            world::deathBurst(tf->position + glm::vec3(0.f, 0.6f, 0.f),
                              entity::creatureColor(reg, target));
        }
    }

    onTargetDeath(reg, target, killerEntity);
}

f32 applyDamage(ecs::Registry& reg, ecs::Entity target, const DamageInstance& dmg) {
    auto* h = reg.get<ecs::Health>(target);
    if (!h) return 0.f;

    if (h->invulnTime > 0.f) {
        if (dmg.burnTime <= 0.f && dmg.poisonTime <= 0.f) {
            return 0.f;
        }
    }

    // ---- Защита ----
    //
    // ДО уворота, и это не случайный порядок. Уворот — бросок
    // кости, парирование — решение игрока; если сперва катить
    // кость, точно отбитый удар иногда «уворачивался» бы вместо
    // этого — без оглушения бьющего и без резонанса, то есть
    // наградой за мастерство оказывалась бы её потеря.
    //
    // Ниже по тексту считается `incoming` — удар, каким он вышел
    // ИЗ-ПОД защиты. Правило простое и одно: парирование снимает всё
    // без остатка, блок урезает только урон, а статусы (поджог, яд)
    // проходят — укус, принятый на клинок, всё равно остаётся
    // укусом.
    DamageInstance incoming = dmg;
    if (resolveGuard(reg, target, incoming) == GuardResult::Parried)
        return 0.f;

    // Уклонение
    bool dodged = false;
    if (incoming.sourceEntity != 0) {
        const auto& derived = progression::derivedOf(reg, target);
        if (derived.dodgeChance > 0.f && rollCritical(derived.dodgeChance)) {
            dodged = true;
        }
    }

    if (dodged) {
        if (auto* tf = reg.get<ecs::Transform>(target)) {
            glm::vec3 pos = tf->position + glm::vec3(0.f, 0.9f, 0.f);
            spawnHitFx(reg, pos, 0xA0A0A0FF, 0.10f, 0.40f, 0.12f);
        }
        return 0.f;
    }

    // Сопротивления
    const DamageResistance* res = nullptr;
    auto* combatant = reg.get<Combatant>(target);
    DamageResistance defaultRes{};
    if (combatant) res = &combatant->resistance;
    else           res = &defaultRes;

    // Phase 9: производные характеристики цели дают доп. сопротивление
    const auto& targetDerived = progression::derivedOf(reg, target);
    DamageResistance effective = *res;
    if (targetDerived.damageResistPhysical > 0.f) {
        effective.physical = std::min(0.80f,
            effective.physical + targetDerived.damageResistPhysical);
    }
    if (targetDerived.damageResistMagic > 0.f) {
        effective.fire   = std::min(0.80f, effective.fire   + targetDerived.damageResistMagic);
        effective.frost  = std::min(0.80f, effective.frost  + targetDerived.damageResistMagic);
        effective.shock  = std::min(0.80f, effective.shock  + targetDerived.damageResistMagic);
        effective.arcane = std::min(0.80f, effective.arcane + targetDerived.damageResistMagic);
    }

    const f32 final = computeFinalDamage(incoming, effective);

    h->current -= final;
    if (final > 0.f) {
        h->invulnTime = std::max(h->invulnTime, 0.15f);
    }

    auto* se = reg.get<StatusEffects>(target);
    if (se) applyStatuses(*se, incoming);

    // Кого игрок бьёт и кто бьёт его — здесь, в общей воронке. Значит
    // полоса цели одинаково работает для меча, стрелы, заклинания,
    // яда и горения, и её не надо заводить заново на каждый источник
    // урона.
    if (final > 0.f) {
        // Доля здоровья ДО удара: из неё начинается след, если цель
        // для полосы новая. Считаем здесь, потому что здесь ещё
        // известно, сколько именно сняли.
        const f32 maxHp = h->max > 1.f ? h->max : 1.f;
        const f32 before = std::min(1.f, std::max(0.f,
                                    (h->current + final) / maxHp));
        noticeDamage(reg, target, incoming.sourceEntity, before);
        // И запоминаем, ОТКУДА ударили. Полоса цели отвечает на
        // «кого бью я», а этот вопрос — «кто бьёт меня»: на телефоне
        // обзор узкий, и удар со спины иначе читается только по
        // убывающей полоске здоровья.
        noticeHurt(reg, target, incoming.sourceEntity, final);
    }

    // Звук попадания и смерти. Эти события были написаны и
    // синтезировались при запуске, но их никто не проигрывал: бой шёл
    // молча — ни мобы, ни игрок никак не отзывались на урон.
    const bool killed = h->current <= 0.f;
    if (final > 0.f) {
        const bool isPlayer = reg.has<ecs::PlayerTag>(target);
        if (isPlayer) {
            if (killed) audio::events().playerDeath();
            else        audio::events().playerHurt();
        } else if (reg.has<mobs::MobTag>(target)) {
            if (auto* tf = reg.get<ecs::Transform>(target)) {
                if (killed) audio::events().mobDeath(tf->position);
                else        audio::events().mobHurt(tf->position);
            }
        }
    }

    if (killed) killTarget(reg, target, incoming.sourceEntity);

    if (final > 0.f) {
        if (auto* tf = reg.get<ecs::Transform>(target)) {
            u32 color = 0xFFFF80FF;
            switch (incoming.type) {
                case DamageType::Fire:   color = 0xFF8040FF; break;
                case DamageType::Frost:  color = 0x80D0FFFF; break;
                case DamageType::Shock:  color = 0xFFFF60FF; break;
                case DamageType::Poison: color = 0x80FF80FF; break;
                case DamageType::Arcane: color = 0xC080FFFF; break;
                default:                 color = 0xFFFFFFC0; break;
            }
            glm::vec3 pos = tf->position + glm::vec3(0.f, 0.8f, 0.f);
            spawnHitFx(reg, pos, color, 0.20f, 0.65f, 0.16f);

            // ---- Осколки в точке попадания ----
            //
            // Здесь, а не в игроке: через это место проходит ВЕСЬ
            // урон в игре — свой, чужой, от стрелы, от заклинания и
            // от волка, кусающего овцу. Оформи попадание в игроке —
            // и мир отвечал бы только на его удары.
            //
            // Летят ПРОЧЬ от бьющего: горсть, направленная от удара,
            // сама показывает, с какой стороны пришло.
            glm::vec3 away{ 0.f, 1.f, 0.f };
            if (incoming.sourceEntity != 0) {
                if (auto* src = reg.get<ecs::Transform>(incoming.sourceEntity)) {
                    glm::vec3 d = tf->position - src->position;
                    d.y = std::max(0.35f, d.y);
                    if (glm::dot(d, d) > 1e-4f) away = d;
                }
            }

            const f32 maxHp = std::max(1.f, h->max);
            world::woundBurst(pos, away,
                              entity::creatureColor(reg, target),
                              final / maxHp,
                              incoming.isCritical);
        }
    }

    return final;
}

void tickStatuses(ecs::Registry& reg, f32 dt) {
    auto& pool = reg.pool<StatusEffects>();
    const usize n = pool.size();

    for (usize i = 0; i < n; ++i) {
        ecs::Entity e = pool.entityAt((u32)i);
        auto* se = pool.get(e);
        auto* h  = reg.get<ecs::Health>(e);
        if (!se || !h) continue;

        // --- Burn DoT ---
        if (se->burnTime > 0.f) {
            const f32 amount = se->burnDps * dt;
            se->burnTime -= dt;
            if (se->burnTime < 0.f) se->burnTime = 0.f;
            h->current -= amount;
            if (h->current <= 0.f) killTarget(reg, e, se->lastAttacker);
        } else {
            se->burnDps = 0.f;
        }

        // --- Poison DoT ---
        if (se->poisonTime > 0.f) {
            const f32 amount = se->poisonDps * dt;
            se->poisonTime -= dt;
            if (se->poisonTime < 0.f) se->poisonTime = 0.f;
            h->current -= amount;
            if (h->current <= 0.f) killTarget(reg, e, se->lastAttacker);
        } else {
            se->poisonDps = 0.f;
        }

        // --- Slow ---
        if (se->slowTime > 0.f) {
            se->slowTime -= dt;
            if (se->slowTime <= 0.f) {
                se->slowTime = 0.f;
                se->slowAmount = 0.f;
            } else if (se->slowTime < 1.f) {
                se->slowAmount *= se->slowTime;
            }
        }

        // --- Stun ---
        if (se->stunTime > 0.f) {
            se->stunTime -= dt;
            if (se->stunTime < 0.f) se->stunTime = 0.f;
        }

        // --- Flash ---
        if (se->flashTimer > 0.f) {
            se->flashTimer -= dt;
            if (se->flashTimer < 0.f) se->flashTimer = 0.f;
        }
    }
}

} // namespace combat
