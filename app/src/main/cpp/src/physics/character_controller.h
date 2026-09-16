/**
 * @file character_controller.h
 * @brief Физика: AABB-коллизия с вокселями, raycast, контроллер персонажа.
 */
#pragma once
#include "../core/types.h"
#include "../world/chunk_manager.h"
#include "collision.h"
#include <glm/glm.hpp>

namespace physics {

struct MoveInput {
    glm::vec2 wishDir{0};
    bool      jumpPressed = false;
    bool      jumpHeld    = false;
    bool      sprint      = false;
    bool      crouch      = false;
};

struct CharacterState {
    glm::vec3 position{0};
    glm::vec3 velocity{0};
    bool onGround = false;
    bool inWater  = false;
    bool inLava   = false;
    bool headUnderwater = false;
    f32  coyoteTimer = 0.f;
    f32  jumpBufferTimer = 0.f;

    /// Подъём на ступень в процессе.
    ///
    /// Раньше подъёма «в процессе» не было: контроллер убеждался, что
    /// наверху свободно, и ОДНОЙ СТРОКОЙ переставлял игрока туда —
    /// `position.y += h`. С точки зрения физики верно, с точки зрения
    /// глаз — телепортация: камера скачком уезжала на полметра вверх.
    ///
    /// Теперь это состояние: игрок поднимается с ограниченной
    /// скоростью, кадр за кадром, пока не дойдёт до цели.
    bool stepping   = false;
    f32  stepTargetY = 0.f;
    f32  stepTimer   = 0.f;   ///< страховка от застревания в подъёме
};

class CharacterController {
public:
    CharacterController() = default;

    void update(world::ChunkManager& world, const MoveInput& input, f32 dt);

    CharacterState&       state()       { return state_; }
    const CharacterState& state() const { return state_; }

    void setPosition(const glm::vec3& p) { state_.position = p; state_.velocity = glm::vec3(0); }
    void addImpulse(const glm::vec3& v)  { state_.velocity += v; }

    /// Параметры движения
    f32 walkSpeed       = 4.5f;
    f32 sprintSpeed     = 7.5f;
    f32 swimSpeed       = 3.0f;
    f32 jumpVelocity    = 9.0f;
    f32 gravity         = -28.0f;
    f32 maxFallSpeed    = -60.0f;
    f32 waterBuoyancy   = 22.0f;
    f32 waterDrag       = 3.0f;
    f32 groundFriction  = 12.0f;
    f32 accelRate       = 45.0f;
    f32 airAccelFactor  = 0.35f;
    f32 coyoteTime      = 0.12f;
    f32 jumpBufferTime  = 0.15f;

    /// Phase 15
    /// Максимальная высота автоматического подъёма.
    ///
    /// Чуть больше блока: мир воксельный, и любая ступень ровно в
    /// один блок. Стояло 0.60 — то есть меньше ступени, и подъём не
    /// срабатывал никогда. Больше двух блоков ставить нельзя: игрок
    /// начнёт въезжать на стены.
    f32 stepHeight      = 1.10f;
    f32 stepCheckDist   = 0.30f;   // насколько вперёд проверяем при step-up
    /// Скорость подъёма на ступень, блоков в секунду.
    ///
    /// Полметра за одну восьмую секунды: быстрее выглядит рывком,
    /// медленнее — будто игрок залипает в стене.
    f32 stepClimbSpeed  = 4.5f;
    /// Сколько подъём может длиться, прежде чем его бросят. Без
    /// предела застрявший в подъёме игрок висел бы без гравитации.
    f32 stepMaxTime     = 0.40f;
    bool enableStepUp   = true;
    bool enableSnapDown = true;
    f32 snapDownDist    = 0.35f;   // максимальная просадка для snap-to-ground
    f32 snapDownMaxVel  = 0.6f;    // если падаем медленнее — снапимся

    PlayerBox box;

private:
    CharacterState state_;
    void detectEnvironment(world::ChunkManager& world);
    /// Phase 15: возвращает true, если удалось автоматически подняться
    /// на препятствие не выше stepHeight.
    bool tryStepUp(world::ChunkManager& world,
                   const glm::vec3& moveDelta);
    /// Phase 15: если игрок стоит близко к земле и падает медленно —
    /// притягиваем вниз.
    void snapDown(world::ChunkManager& world, f32 dt);
};

} // namespace physics
