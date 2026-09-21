/**
 * @file focus.cpp
 * @brief Бой: с кем у игрока размен прямо сейчас.
 */
#include "focus.h"
#include "../ecs/components.h"
#include "../entity/creature_info.h"
#include "../mobs/mob_ai.h"
#include "../mobs/mob_def.h"
#include <algorithm>

namespace combat {

void noticeExchange(FocusTarget& f, u32 otherId, f32 otherFill) {
    if (otherId == 0) return;

    // Труп полосу себе не возвращает. Иначе стая, молотящая павшего,
    // держала бы пустую полосу на экране всё время, что лежит тело.
    if (f.id == otherId && f.dead) return;

    if (f.id != otherId) {
        // Новая цель — новая полоса, и начинается она С ТОГО, ЧТО У
        // ЦЕЛИ БЫЛО. Начни её с полной — и первый же удар по раненой
        // твари нарисовал бы след от края полосы, то есть соврал бы о
        // силе удара во столько раз, во сколько тварь была ранена.
        const f32 start = std::min(1.f, std::max(0.f, otherFill));
        f.id = otherId;
        f.fill = start;
        f.ghost = start;
        f.dead = false;
    }

    f.hold = FOCUS_HOLD_SEC;
    f.ghostDelay = FOCUS_GHOST_DELAY;
}

/// Доля здоровья существа прямо сейчас. Нет здоровья — считаем целым.
static f32 healthFill(ecs::Registry& reg, u32 id) {
    if (id == 0 || !reg.alive(id)) return 1.f;
    const auto* h = reg.get<ecs::Health>(id);
    if (!h) return 1.f;
    const f32 maxHp = h->max > 1.f ? h->max : 1.f;
    return std::min(1.f, std::max(0.f, h->current / maxHp));
}

void noticeDamage(ecs::Registry& reg, ecs::Entity target, u32 sourceId,
                  f32 targetFillBefore)
{
    // Полоса есть только у игрока. Волку, дерущемуся с овцой, вести
    // её незачем, и проверка здесь ровно затем, чтобы не заводить её
    // всем подряд.
    if (reg.has<ecs::PlayerTag>(target)) {
        // Бьют игрока: цель полосы — бьющий, и его здоровье этим
        // ударом не изменилось. Значит и следа за ним нет.
        if (auto* f = reg.get<FocusTarget>(target))
            noticeExchange(*f, sourceId, healthFill(reg, sourceId));
        return;
    }
    if (sourceId == 0 || !reg.alive(sourceId)) return;
    if (!reg.has<ecs::PlayerTag>(sourceId)) return;
    if (auto* f = reg.get<FocusTarget>(sourceId))
        noticeExchange(*f, (u32)target, targetFillBefore);
}

void tickFocus(ecs::Registry& reg, FocusTarget& f, f32 dt) {
    if (f.id == 0) return;

    if (f.hold > 0.f) {
        f.hold -= dt;
        if (f.hold < 0.f) f.hold = 0.f;
    }
    if (f.hold <= 0.f) { f = FocusTarget{}; return; }

    const auto* h = reg.alive(f.id) ? reg.get<ecs::Health>(f.id) : nullptr;
    if (!h) {
        // Тварь исчезла из мира — её полосе больше нечего показывать.
        // Это не смерть: смерть мы успеваем увидеть раньше, пока труп
        // ещё лежит.
        f = FocusTarget{};
        return;
    }

    const f32 maxHp = h->max > 1.f ? h->max : 1.f;
    f.fill = std::min(1.f, std::max(0.f, h->current / maxHp));

    if (f.fill <= 0.f && !f.dead) {
        f.dead = true;
        // Опустевшая полоса живёт ровно столько, чтобы её заметили.
        f.hold = std::min(f.hold, FOCUS_DEATH_SEC);
    }

    // След оседает не сразу: пауза, затем ровный спуск. Мгновенный
    // след совпал бы с основной заливкой и не показал бы ничего.
    if (f.ghost > f.fill) {
        if (f.ghostDelay > 0.f) {
            f.ghostDelay -= dt;
        } else {
            f.ghost = std::max(f.fill, f.ghost - FOCUS_GHOST_RATE * dt);
        }
    } else {
        // Лечение полосу не подделывает: след догоняет вверх сразу.
        f.ghost = f.fill;
    }
}

bool focusView(ecs::Registry& reg, const FocusTarget& f, FocusView& out) {
    out = FocusView{};
    if (f.id == 0 || f.hold <= 0.f) return false;
    if (!reg.alive(f.id)) return false;

    // Имя берёт общий ответчик: у мобов свой реестр, у жителей свой,
    // и полоса не обязана знать, в каком из них искать.
    out.name  = entity::creatureName(reg, reg.fromId(f.id));
    out.fill  = f.fill;
    out.ghost = std::max(f.ghost, f.fill);
    out.dead  = f.dead;
    out.alpha = (f.hold < FOCUS_FADE_SEC) ? (f.hold / FOCUS_FADE_SEC) : 1.f;

    // Фазы — у боссов. Пороги считает та же функция, по которой босс
    // их и переключает: деления на полосе обязаны стоять там же, где
    // тварь меняет поведение, иначе они врут.
    if (auto* tag = reg.get<mobs::MobTag>(f.id)) {
        const u8 phases = mobs::mobRegistry().get(tag->id).phaseCount;
        out.phases = phases > 0 ? phases : 1;
        out.phase  = mobs::bossPhaseFor(f.fill, out.phases);
    }
    return true;
}

} // namespace combat
