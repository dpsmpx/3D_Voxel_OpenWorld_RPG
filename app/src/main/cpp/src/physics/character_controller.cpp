#include "character_controller.h"
#include "../world/block.h"
#include <cmath>
#include <algorithm>

namespace physics {

void CharacterController::detectEnvironment(world::ChunkManager& world) {
    auto blockAt = [&](const glm::vec3& p) -> u16 {
        return world.getVoxel((i32)std::floor(p.x),
                              (i32)std::floor(p.y),
                              (i32)std::floor(p.z));
    };

    glm::vec3 feet = state_.position + glm::vec3(0, 0.1f, 0);
    glm::vec3 mid  = state_.position + glm::vec3(0, box.height * 0.5f, 0);
    glm::vec3 eye  = state_.position + glm::vec3(0, box.height - 0.1f, 0);

    u16 bF = blockAt(feet);
    u16 bM = blockAt(mid);
    u16 bE = blockAt(eye);

    state_.inWater  = (bF == world::WATER) || (bM == world::WATER);
    state_.inLava   = (bF == world::LAVA)  || (bM == world::LAVA);
    state_.headUnderwater = (bE == world::WATER);
}

// ============================================================
// Phase 15: step-up
//
// Идея: если игрок упёрся в блок, пытаемся найти "площадку" на
// высоте от 0.1 до stepHeight над текущей позицией, на которую
// можно встать, не задев стену. Если такая есть — поднимаем.
// ============================================================
bool CharacterController::tryStepUp(world::ChunkManager& world,
                                    const glm::vec3& moveDelta)
{
    if (!enableStepUp) return false;

    const f32 horizLen = glm::length(glm::vec2(moveDelta.x, moveDelta.z));
    if (horizLen < 1e-4f) return false;

    // Направление движения.
    glm::vec3 dir = glm::vec3(moveDelta.x, 0.f, moveDelta.z) / horizLen;

    auto& reg = world::blocks();

    // Проверяем несколько высот от 0.2 до stepHeight.
    constexpr int STEPS = 6;
    for (int s = 1; s <= STEPS; ++s) {
        f32 h = (f32)s / (f32)STEPS * stepHeight;
        glm::vec3 testPos = state_.position + glm::vec3(0, h, 0);

        // Тело на новой высоте должно помещаться.
        glm::vec3 bmin = box.min(testPos);
        glm::vec3 bmax = box.max(testPos);

        // Проверяем, что на новой высоте нет стен.
        bool blocked = false;
        i32 x0 = (i32)std::floor(bmin.x);
        i32 x1 = (i32)std::floor(bmax.x - 1e-4f);
        i32 y0 = (i32)std::floor(bmin.y);
        i32 y1 = (i32)std::floor(bmax.y - 1e-4f);
        i32 z0 = (i32)std::floor(bmin.z);
        i32 z1 = (i32)std::floor(bmax.z - 1e-4f);

        for (i32 y = y0; y <= y1 && !blocked; ++y) {
            for (i32 z = z0; z <= z1 && !blocked; ++z) {
                for (i32 x = x0; x <= x1; ++x) {
                    if (reg.isSolid(world.getVoxel(x, y, z))) {
                        blocked = true;
                        break;
                    }
                }
            }
        }
        if (blocked) continue;

        // Проверяем, что под игроком есть опора (верх блока).
        i32 fy = (i32)std::floor(testPos.y - 0.05f);
        i32 fx = (i32)std::floor(testPos.x);
        i32 fz = (i32)std::floor(testPos.z);
        bool groundBelow = reg.isSolid(world.getVoxel(fx, fy, fz));

        if (!groundBelow) {
            // Проверим вокруг (для широких ног).
            for (i32 dx = -1; dx <= 1 && !groundBelow; ++dx) {
                for (i32 dz = -1; dz <= 1; ++dz) {
                    if (reg.isSolid(world.getVoxel(fx + dx, fy, fz + dz))) {
                        groundBelow = true;
                        break;
                    }
                }
            }
        }
        if (!groundBelow) continue;

        // Проверяем, что по направлению движения есть куда двигаться
        // (свободное место не меньше размеров AABB).
        glm::vec3 aheadPos = testPos + dir * stepCheckDist;
        glm::vec3 amin = box.min(aheadPos);
        glm::vec3 amax = box.max(aheadPos);
        bool aheadBlocked = false;

        i32 ax0 = (i32)std::floor(amin.x);
        i32 ax1 = (i32)std::floor(amax.x - 1e-4f);
        i32 ay0 = (i32)std::floor(amin.y);
        i32 ay1 = (i32)std::floor(amax.y - 1e-4f);
        i32 az0 = (i32)std::floor(amin.z);
        i32 az1 = (i32)std::floor(amax.z - 1e-4f);

        for (i32 y = ay0; y <= ay1 && !aheadBlocked; ++y) {
            for (i32 z = az0; z <= az1 && !aheadBlocked; ++z) {
                for (i32 x = ax0; x <= ax1; ++x) {
                    if (reg.isSolid(world.getVoxel(x, y, z))) {
                        aheadBlocked = true;
                        break;
                    }
                }
            }
        }
        if (aheadBlocked) continue;

        // Успех: поднимаем игрока.
        state_.position.y += h;
        state_.onGround = true;
        state_.velocity.y = 0.f;
        return true;
    }

    return false;
}

// ============================================================
// Phase 15: snap-to-ground
//
// Если игрок в воздухе, падает медленно (не прыгнул), и в
// пределах snapDownDist под ним есть земля — притягиваем вниз.
// ============================================================
void CharacterController::snapDown(world::ChunkManager& world, f32 dt) {
    if (!enableSnapDown) return;
    if (state_.onGround) return;
    if (state_.inWater || state_.inLava) return;
    // Не снапим при движении вверх (прыжок).
    if (state_.velocity.y > 0.1f) return;
    // Снапим только при медленном падении.
    if (state_.velocity.y < -snapDownMaxVel * 8.f) return;

    auto& reg = world::blocks();

    // Проверяем, есть ли твёрдый блок в пределах snapDownDist.
    const f32 stepSize = 0.05f;
    for (f32 dist = 0.05f; dist <= snapDownDist; dist += stepSize) {
        glm::vec3 probe = state_.position - glm::vec3(0, dist, 0);
        i32 px = (i32)std::floor(probe.x);
        i32 py = (i32)std::floor(probe.y);
        i32 pz = (i32)std::floor(probe.z);

        if (reg.isSolid(world.getVoxel(px, py, pz))) {
            // Вычислим, где "пол" блока.
            f32 floorY = (f32)(py + 1);
            // Притягиваем к полу.
            state_.position.y = floorY + 0.0001f;
            state_.onGround = true;
            state_.velocity.y = 0.f;
            (void)dt;
            return;
        }
    }
}

void CharacterController::update(world::ChunkManager& world,
                                 const MoveInput& input, f32 dt)
{
    if (dt <= 0.f) return;
    if (dt > 0.1f) dt = 0.1f;

    detectEnvironment(world);

    // --- Таймеры ---
    state_.coyoteTimer     = std::max(0.f, state_.coyoteTimer - dt);
    state_.jumpBufferTimer = std::max(0.f, state_.jumpBufferTimer - dt);
    if (input.jumpPressed) state_.jumpBufferTimer = jumpBufferTime;

    // --- Желаемая горизонтальная скорость ---
    f32 wishSpeed = state_.inWater ? swimSpeed
                  : (input.sprint ? sprintSpeed : walkSpeed);
    glm::vec3 wishVel = glm::vec3(input.wishDir.x, 0.f, input.wishDir.y) * wishSpeed;

    glm::vec3 curXZ{ state_.velocity.x, 0.f, state_.velocity.z };
    glm::vec3 dv = wishVel - curXZ;
    f32 dvLen = glm::length(dv);

    if (dvLen > 1e-5f) {
        f32 accel = accelRate;
        if (!state_.onGround && !state_.inWater) accel *= airAccelFactor;
        if (state_.inWater) accel *= 0.6f;

        f32 maxDelta = accel * dt;
        if (dvLen > maxDelta) dv = dv / dvLen * maxDelta;
        state_.velocity.x += dv.x;
        state_.velocity.z += dv.z;
    }

    // --- Трение ---
    if (state_.inWater) {
        f32 drag = std::exp(-waterDrag * dt);
        state_.velocity.x *= drag;
        state_.velocity.z *= drag;
        state_.velocity.y *= drag;

        if (!input.jumpHeld && !input.crouch) {
            state_.velocity.y += waterBuoyancy * dt;
        }
        if (input.jumpHeld) state_.velocity.y = 3.0f;
        if (input.crouch)   state_.velocity.y = -3.0f;
        if (state_.velocity.y > 3.5f)  state_.velocity.y = 3.5f;
        if (state_.velocity.y < -5.0f) state_.velocity.y = -5.0f;
    } else if (state_.onGround && dvLen < 1e-4f) {
        f32 f = std::exp(-groundFriction * dt);
        state_.velocity.x *= f;
        state_.velocity.z *= f;
    }

    // --- Прыжок ---
    const bool canJump = !state_.inWater &&
                        (state_.onGround || state_.coyoteTimer > 0.f);
    if (canJump && state_.jumpBufferTimer > 0.f) {
        state_.velocity.y = jumpVelocity;
        state_.coyoteTimer = 0.f;
        state_.jumpBufferTimer = 0.f;
        state_.onGround = false;
    }

    // --- Гравитация ---
    if (!state_.inWater) {
        state_.velocity.y += gravity * dt;
        if (state_.velocity.y < maxFallSpeed) state_.velocity.y = maxFallSpeed;
    }

    // --- Перемещение с коллизией ---
    const bool wasOnGround = state_.onGround;
    state_.onGround = false;

    glm::vec3 delta = state_.velocity * dt;
    glm::vec3 originalDelta = delta;

    CollisionFlags flags = resolveMovement(world, state_.position, delta, box);

    if (flags.onGround) state_.onGround = true;

    // ============================================================
    // Phase 15: step-up
    // Если игрок упёрся по X или Z и был на земле — попробуем
    // подняться на низкое препятствие.
    // ============================================================
    if (enableStepUp && wasOnGround && (flags.hitX || flags.hitZ) && !flags.onGround) {
        // Сначала откатим позицию назад на смещение по X/Z.
        // Простейший способ: запомним позицию до движения и попробуем
        // step-up из неё.
        // (state_.position уже изменён resolveMovement, поэтому
        // восстанавливаем исходную позицию и пробуем заново.)
        // Мы не храним «до», поэтому используем тот же delta повторно:
        // смещение игрока = velocity * dt. Откатим X/Z, оставив Y.
        // В реальности было бы правильно запоминать позицию до движения.

        // Дополнительная проверка: пробуем поднять.
        glm::vec3 moveIntent = originalDelta;
        moveIntent.y = 0.f;

        // Пробуем step-up только если есть намерение движения по X/Z.
        if (glm::length(moveIntent) > 1e-4f) {
            if (tryStepUp(world, moveIntent)) {
                // Очищаем блокирующие скорости.
                if (flags.hitX) state_.velocity.x = 0;
                if (flags.hitZ) state_.velocity.z = 0;
                // После подъёма — двигаем по XZ ещё раз без коллизий по стенам.
                glm::vec3 step = glm::vec3(state_.velocity.x, 0.f, state_.velocity.z) * dt;
                if (glm::length(step) > 1e-4f) {
                    glm::vec3 tmpDelta = step;
                    resolveMovement(world, state_.position, tmpDelta, box);
                }
            }
        }
    }

    if (wasOnGround && !state_.onGround) state_.coyoteTimer = coyoteTime;
    if (state_.onGround) state_.coyoteTimer = coyoteTime;

    if (flags.hitX) state_.velocity.x = 0;
    if (flags.hitY) state_.velocity.y = 0;
    if (flags.hitZ) state_.velocity.z = 0;

    // ============================================================
    // Phase 15: snap-to-ground
    // ============================================================
    snapDown(world, dt);

    // --- Rescue: если застряли в блоке ---
    if (overlapsSolid(world, box.min(state_.position), box.max(state_.position))) {
        state_.position.y += 2.5f * dt;
    }
}

} // namespace physics
