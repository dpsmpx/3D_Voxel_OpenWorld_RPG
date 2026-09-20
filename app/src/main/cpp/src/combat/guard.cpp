/**
 * @file guard.cpp
 * @brief Защита: блок, парирование и пробитие.
 */
#include "guard.h"
#include "components.h"
#include "weapon.h"
#include "projectile.h"
#include "../ecs/components.h"
#include "../progression/progression.h"
#include "../progression/resource_regen.h"
#include "../audio/audio_events.h"
#include <algorithm>
#include <cmath>

namespace combat {

void GuardState::tick(f32 dt, bool want, const glm::vec3& dir) {
    if (want) {
        if (!up) {
            // Подъём. Здесь и решается, будет ли у этого подъёма окно
            // парирования: защита, только что опущенная и тут же
            // поднятая обратно, его не получает — иначе дробь по
            // кнопке парировала бы всё подряд.
            armed     = (downFor >= PARRY_REARM_TIME);
            raisedFor = 0.f;
            downFor   = 0.f;
        } else {
            // Счётчик идёт с МОМЕНТА ПОДЪЁМА, а не с нажатия кнопки:
            // именно он и решает, парирование это или блок.
            raisedFor += dt;
        }
        up = true;
        facing = dir;
        pose = std::min(1.f, pose + dt / GUARD_RAISE_TIME);
    } else {
        up = false;
        raisedFor = 0.f;
        armed = false;
        downFor += dt;
        pose = std::max(0.f, pose - dt / GUARD_RAISE_TIME);
    }

    if (flash > 0.f) flash = std::max(0.f, flash - dt * 4.f);
}

f32 guardFractionOf(ecs::Registry& reg, ecs::Entity e) {
    const auto* eq = reg.get<EquippedWeapon>(e);
    // Пустая рука тоже что-то держит, но немного: у WEAPON_NONE своя
    // доля в реестре, и отдельной ветки «если оружия нет» не нужно.
    return weapons().get(eq ? eq->weaponId : (u16)WEAPON_NONE).guardFraction;
}

namespace {

/// Оглушить. Без StatusEffects оглушать нечего — и это не ошибка:
/// не у всякой сущности он есть.
void stun(ecs::Registry& reg, ecs::Entity e, f32 seconds) {
    if (auto* se = reg.get<StatusEffects>(e)) {
        se->stunTime = std::max(se->stunTime, seconds);
    }
}

} // namespace

GuardResult resolveGuard(ecs::Registry& reg,
                         ecs::Entity target,
                         DamageInstance& dmg)
{
    auto* g = reg.get<GuardState>(target);
    if (!g || !g->up || dmg.amount <= 0.f) return GuardResult::None;

    // Защищаются ОТ КОГО-ТО. У падения с обрыва, утопления и лавы
    // источника нет — подставить под них щит нельзя, и проверка
    // источника отсекает их все разом, без перечня видов урона.
    if (dmg.sourceEntity == 0) return GuardResult::None;

    const auto* atf = reg.get<ecs::Transform>(reg.fromId(dmg.sourceEntity));
    const auto* ttf = reg.get<ecs::Transform>(target);
    if (!atf || !ttf) return GuardResult::None;

    // ---- Только спереди ----
    //
    // Направление считается по ПОЛОЖЕНИЮ бьющего. Для ближнего боя,
    // на который защита и рассчитана, это в точности верно. Для
    // стрелы — приблизительно: она прилетает оттуда, где стрелок был
    // в момент выстрела, а не где он сейчас. Носить направление в
    // DamageInstance ради этого случая дороже, чем он стоит.
    glm::vec3 to = atf->position - ttf->position;
    to.y = 0.f;
    const f32 toLen = glm::length(to);
    if (toLen < 1e-3f) return GuardResult::None;
    to /= toLen;

    glm::vec3 fwd = g->facing;
    fwd.y = 0.f;
    const f32 fwdLen = glm::length(fwd);
    if (fwdLen < 1e-3f) return GuardResult::None;
    fwd /= fwdLen;

    if (glm::dot(fwd, to) < std::cos(GUARD_ARC * 0.5f))
        return GuardResult::None;

    const glm::vec3 fxAt = ttf->position + glm::vec3(0.f, 1.1f, 0.f)
                         + to * 0.5f;

    // ---- Парирование ----
    //
    // Защита, поднятая в последний миг. Урон снимается целиком,
    // выносливость не тратится, а бьющий ОГЛУШАЕТСЯ — то есть
    // теряет и этот замах, и следующий.
    if (g->parryReady()) {
        dmg.amount = 0.f;
        dmg.burnTime = dmg.poisonTime = dmg.slowDuration = dmg.stunDuration = 0.f;

        stun(reg, reg.fromId(dmg.sourceEntity), PARRY_STAGGER);

        // Резонанс за точность — как за критический удар. Отбитый
        // удар не наносит урона, и без этого идеальная защита
        // оставляла бы игрока без единственного боевого ресурса.
        if (auto* res = reg.get<ResonanceState>(target)) {
            res->onHit(true, progression::derivedOf(reg, target).resonanceGainMult);
        }

        g->pending = GuardResult::Parried;
        g->flash   = 1.f;
        spawnHitFx(reg, fxAt, 0xFFF0A0FF, 0.35f, 1.5f, 0.30f);
        audio::events().parry(ttf->position);
        return GuardResult::Parried;
    }

    // ---- Блок ----
    const f32 absorbed = dmg.amount * guardFractionOf(reg, target);
    const f32 cost     = absorbed * GUARD_STAMINA_PER_DAMAGE;

    if (!progression::tryConsumeStamina(reg, target, cost)) {
        // ---- Пробитие ----
        //
        // Выносливости не хватило: удар проходит ЦЕЛИКОМ, защита
        // падает, защитник оглушён. Это и делает блок решением, а не
        // кнопкой «не получать урон»: держать его бесконечно нельзя.
        g->up        = false;
        g->raisedFor = 0.f;
        g->pending   = GuardResult::Broken;
        g->flash     = 1.f;
        stun(reg, target, GUARD_BREAK_STUN);
        spawnHitFx(reg, fxAt, 0xFF6040FF, 0.30f, 1.3f, 0.28f);
        audio::events().guardBreak(ttf->position);
        return GuardResult::Broken;
    }

    dmg.amount = std::max(0.f, dmg.amount - absorbed);
    g->pending = GuardResult::Blocked;
    g->flash   = std::max(g->flash, 0.6f);
    spawnHitFx(reg, fxAt, 0xC0D0E0FF, 0.25f, 0.9f, 0.20f);
    audio::events().guardHit(ttf->position);
    return GuardResult::Blocked;
}

} // namespace combat
