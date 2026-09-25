/**
 * @file character_controller.cpp
 * @brief Физика: AABB-коллизия с вокселями, raycast, контроллер персонажа.
 */
#include "character_controller.h"
#include "impact.h"
#include "../world/block.h"
#include <cmath>
#include <algorithm>

namespace physics {

void CharacterController::detectEnvironment(world::ChunkManager& world) {
    world::VoxelReader rd(world);
    auto blockAt = [&](const glm::vec3& p) -> u16 {
        return rd.at((i32)std::floor(p.x),
                     (i32)std::floor(p.y),
                     (i32)std::floor(p.z));
    };

    glm::vec3 feet = state_.position + glm::vec3(0, 0.1f, 0);
    glm::vec3 mid  = state_.position + glm::vec3(0, box.height * 0.5f, 0);
    // Уровень глаз. Раньше он читался в переменную, которую не
    // спрашивал никто, — и «под водой» в игре не значило ничего.
    // Теперь по нему решается и выталкивание, и дыхание.
    glm::vec3 eyes = state_.position + glm::vec3(0, box.height * 0.9f, 0);

    u16 bF = blockAt(feet);
    u16 bM = blockAt(mid);
    u16 bE = blockAt(eyes);

    state_.inWater   = (bF == world::WATER) || (bM == world::WATER);
    state_.inLava    = (bF == world::LAVA)  || (bM == world::LAVA);
    state_.waistDeep = (bM == world::WATER);
    state_.submerged = (bE == world::WATER);
    if (!state_.inWater) state_.surfacing = false;
}

// ============================================================
// Phase 15: step-up
//
// Идея: если игрок упёрся в блок, целимся в верх СЛЕДУЮЩЕГО блока и
// проверяем, что тело туда помещается и что под ним есть опора. Если
// да — назначаем подъём; поднимает игрока обычное движение, кадр за
// кадром.
// ============================================================
bool CharacterController::tryStepUp(world::ChunkManager& world,
                                    const glm::vec3& moveDelta)
{
    if (!enableStepUp) return false;

    const f32 horizLen = glm::length(glm::vec2(moveDelta.x, moveDelta.z));
    if (horizLen < 1e-4f) return false;

    // Направление движения.
    glm::vec3 dir = glm::vec3(moveDelta.x, 0.f, moveDelta.z) / horizLen;

    // Курсор на всю проверку: она перебирает несколько коробок
    // вокселей вокруг игрока, то есть почти всегда один и тот же чанк.
    auto& reg = world::blocks();
    world::VoxelReader rd(world);

    // Цель подъёма — ВЕРХ СЛЕДУЮЩЕГО БЛОКА, ровно один блок.
    //
    // Мир воксельный: любая ступень ровно в один блок высотой. Прежний
    // перебор шести дробных высот до 0.60 не мог дать ровно 1.0 ни на
    // одной итерации, поэтому подъём не срабатывал НИКОГДА — а то, что
    // выглядело как автоподъём, было ошибкой разрешения коллизий,
    // закидывавшей игрока на ступень целиком (см. collision.cpp).
    //
    // Вдобавок проверка опоры смотрит на блок под ступнями как
    // floor(y - 0.05): она верна только тогда, когда ступни встают
    // ровно на границу блока. Дробная высота её и ломала.
    {
        const f32 h = std::floor(state_.position.y) + 1.f - state_.position.y;
        glm::vec3 testPos = state_.position + glm::vec3(0, h, 0);
        if (h <= 0.01f) return false;

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
                    if (reg.isSolid(rd.at(x, y, z))) {
                        blocked = true;
                        break;
                    }
                }
            }
        }
        if (blocked) return false;

        // Проверяем, что под игроком есть опора (верх блока).
        i32 fy = (i32)std::floor(testPos.y - 0.05f);
        i32 fx = (i32)std::floor(testPos.x);
        i32 fz = (i32)std::floor(testPos.z);
        bool groundBelow = reg.isSolid(rd.at(fx, fy, fz));

        if (!groundBelow) {
            // Проверим вокруг (для широких ног).
            for (i32 dx = -1; dx <= 1 && !groundBelow; ++dx) {
                for (i32 dz = -1; dz <= 1; ++dz) {
                    if (reg.isSolid(rd.at(fx + dx, fy, fz + dz))) {
                        groundBelow = true;
                        break;
                    }
                }
            }
        }
        if (!groundBelow) return false;

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
                    if (reg.isSolid(rd.at(x, y, z))) {
                        aheadBlocked = true;
                        break;
                    }
                }
            }
        }
        if (aheadBlocked) return false;

        // Успех: НАЗНАЧАЕМ подъём, а не совершаем его.
        //
        // Здесь стояло `state_.position.y += h` — мгновенная
        // перестановка на полметра вверх. Физически верно, на глаз —
        // телепортация. Теперь запоминаем цель, а поднимает игрока
        // обычное движение, кадр за кадром, с ограниченной скоростью.
        state_.stepping   = true;
        state_.stepTargetY = state_.position.y + h;
        state_.stepTimer   = 0.f;
        return true;
    }

    return false;
}

// ============================================================
// Phase 15: snap-to-ground
//
// Если игрок в воздухе, падает медленно (не прыгнул), и в
// пределах snapDownDist под ним есть земля — притягиваем вниз.
//
// ВНИЗ, и только вниз. Это вторая половина «телепортации при
// подъёме», и она была здесь с самого начала.
//
// Проба идёт вниз от ступней и ищет первый твёрдый блок, после чего
// ставит игрока на ЕГО ВЕРХ: `position.y = py + 1`. Если игрок хоть
// немного погружён в блок, под ступнями оказывается тот самый блок,
// в котором он стоит, и его верх — почти на целый блок ВЫШЕ игрока.
// Функция с именем «притянуть к земле» подбрасывала его вверх на
// 0.92 блока в одном кадре. Замер на стенде по настоящему рельефу:
// 29.08 -> 30.00.
//
// Погружение брал на себя спасательный подъём из update(), который
// выталкивает застрявшего игрока вверх по чуть-чуть. Вместе это и
// давало подъём по стене дома до крыши: отбросило назад в геометрию
// (см. collision.cpp) -> вытолкнуло вверх -> snapDown поставил на
// верх блока -> и снова.
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
    world::VoxelReader rd(world);

    // Проверяем, есть ли твёрдый блок в пределах snapDownDist.
    const f32 stepSize = 0.05f;
    for (f32 dist = 0.05f; dist <= snapDownDist; dist += stepSize) {
        glm::vec3 probe = state_.position - glm::vec3(0, dist, 0);
        i32 px = (i32)std::floor(probe.x);
        i32 py = (i32)std::floor(probe.y);
        i32 pz = (i32)std::floor(probe.z);

        if (reg.isSolid(rd.at(px, py, pz))) {
            // Верх найденного блока.
            const f32 floorY = (f32)(py + 1) + 0.0001f;
            // Выше, чем игрок стоит сейчас, ставить нельзя: это не
            // притягивание к земле, а подброс. Так бывает, когда
            // ступни уже внутри блока — разбираться с этим положением
            // не здесь, для того есть спасательный подъём.
            if (floorY > state_.position.y) return;
            state_.position.y = floorY;
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

    const bool startedOnGround = state_.onGround;
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

    // --- Рывок ---
    //
    // Считается ПОСЛЕ разгона и ДО трения: рывок задаёт скорость
    // целиком, и разгонять или тормозить в эти сто восемьдесят
    // миллисекунд нечего.
    state_.dashCooldown = std::max(0.f, state_.dashCooldown - dt);
    if (input.dashPressed && state_.dashCooldown <= 0.f &&
        !state_.dashing() && !state_.inWater)
    {
        glm::vec2 d = input.wishDir;
        if (glm::dot(d, d) < 1e-4f) d = input.faceDir;
        const f32 len = glm::length(d);
        if (len > 1e-4f) {
            state_.dashDir      = d / len;
            state_.dashTimer    = dashTime;
            state_.dashCooldown = dashCooldownTime;
            state_.stepping     = false;   // рывок отменяет подъём
        }
    }
    if (state_.dashing()) {
        state_.dashTimer = std::max(0.f, state_.dashTimer - dt);
        state_.velocity.x = state_.dashDir.x * dashSpeed;
        state_.velocity.z = state_.dashDir.y * dashSpeed;
        // Рывок горизонтальный: падать во время него нельзя, иначе с
        // края обрыва он превращается в прыжок в пропасть.
        state_.velocity.y = 0.f;
    }

    // --- Трение ---
    if (state_.inWater && state_.velocity.y < -swimMaxFall) {
        // Нырок с высоты. Гасит его глубина, а не первый кадр в воде:
        // раньше скорость здесь резалась до пяти разом, и лужа по
        // колено ловила падение с любой высоты. Грести против такого
        // падения нельзя — тело несёт вниз, пока вода его не
        // остановит или пока оно не ударится о дно. Саму вертикаль
        // гасит settleFall — по глубине, пройденной за кадр.
        const f32 drag = std::exp(-waterDrag * dt);
        state_.velocity.x *= drag;
        state_.velocity.z *= drag;
        state_.surfacing = true;
    } else if (state_.inWater) {
        f32 drag = std::exp(-waterDrag * dt);
        state_.velocity.x *= drag;
        state_.velocity.z *= drag;
        state_.velocity.y *= drag;

        // Сам взялся грести — всплытие после нырка кончилось: дальше
        // он решает сам.
        if (input.jumpHeld || input.crouch || !state_.submerged)
            state_.surfacing = false;

        if (input.jumpHeld) {
            // Гребок вверх — всплытие.
            state_.velocity.y = swimRiseSpeed;
        } else if (input.crouch) {
            // Гребок вниз — погружение. Работает и у поверхности:
            // иначе нырнуть было бы нельзя, выталкивание пересилит.
            state_.velocity.y = -swimDiveSpeed;
        } else if (state_.surfacing) {
            // Ушёл под воду падением — выносит наверх.
            state_.velocity.y += waterBuoyancy * dt;
        } else if (!state_.waistDeep) {
            // Воды по щиколотку — это брод, а не плавание: тело
            // стоит на дне, и выталкивать нечего. Раньше хватало
            // мокрых ног, и зайдя в лужу игрок всплывал над ней.
            state_.velocity.y -= 9.8f * dt;
        } else if (!state_.submerged) {
            // По пояс, голова снаружи — держимся на плаву.
            // Выталкивает только то, что над водой.
            state_.velocity.y += waterBuoyancy * dt;
        } else {
            // Под водой и ничего не жмём — медленно тонем. Раньше
            // здесь работало то же выталкивание, и нырнувшего
            // выбрасывало наверх, стоило отпустить клавишу.
            state_.velocity.y -= waterSinkRate * dt;
        }
        if (state_.velocity.y >  swimRiseSpeed) state_.velocity.y =  swimRiseSpeed;
        if (state_.velocity.y < -swimMaxFall)   state_.velocity.y = -swimMaxFall;
    } else if (state_.onGround && dvLen < 1e-4f && !state_.dashing()) {
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

    // --- Подъём на ступень ---
    //
    // Пока он идёт, гравитации нет и игрок считается стоящим: он не
    // прыгнул и не падает, он взбирается. Иначе на эти три кадра поза
    // переключалась бы в прыжок.
    if (state_.stepping) {
        state_.stepTimer += dt;
        const bool reached = state_.position.y >= state_.stepTargetY - 1e-3f;
        if (reached || state_.stepTimer > stepMaxTime || state_.inWater) {
            state_.stepping = false;
            if (reached) state_.velocity.y = 0.f;
        } else {
            state_.velocity.y = stepClimbSpeed;
            state_.onGround = true;
        }
    }

    // Прыжок отменяет подъём: игрок решил иначе.
    if (state_.velocity.y > stepClimbSpeed + 1e-3f) state_.stepping = false;

    // --- Гравитация ---
    if (!state_.inWater && !state_.stepping && !state_.dashing()) {
        state_.velocity.y += gravity * dt;
        if (state_.velocity.y < maxFallSpeed) state_.velocity.y = maxFallSpeed;
    }

    // --- Перемещение с коллизией ---
    const bool wasOnGround = state_.onGround;
    state_.onGround = false;
    // С какой высоты и с какой скоростью тело шло в этот кадр: по
    // ним settleFall считает, сколько жидкости оно пересекло.
    const f32 yBeforeMove  = state_.position.y;
    const f32 vyBeforeMove = state_.velocity.y;

    glm::vec3 delta = state_.velocity * dt;
    glm::vec3 originalDelta = delta;

    CollisionFlags flags = resolveMovement(world, state_.position, delta, box);

    if (flags.onGround) state_.onGround = true;

    // ============================================================
    // Phase 15: step-up
    // Если игрок упёрся по X или Z и был на земле — попробуем
    // подняться на низкое препятствие.
    // ============================================================
    // Условие `!flags.onGround` отсюда убрано.
    //
    // При ходьбе по земле оно истинно НИКОГДА: гравитация каждый кадр
    // тянет вниз, разрешение по Y ставит игрока на пол и выставляет
    // onGround. То есть подъём на ступень не пробовали вовсе — вместе
    // с недостижимой высотой 0.60 это и делало механизм мёртвым.
    if (enableStepUp && wasOnGround && (flags.hitX || flags.hitZ) &&
        !state_.stepping) {
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
            // Подъём только НАЗНАЧАЕТСЯ: игрок поднимется за
            // несколько кадров. Досылать его вперёд прямо сейчас
            // нечем — он ещё стоит перед ступенью, и лишний рывок
            // вогнал бы его в неё.
            tryStepUp(world, moveIntent);
        }
    }

    if (wasOnGround && !state_.onGround) state_.coyoteTimer = coyoteTime;
    if (state_.onGround) state_.coyoteTimer = coyoteTime;

    if (flags.hitX) state_.velocity.x = 0;
    if (flags.hitY) state_.velocity.y = 0;
    // Подъём отменяет УДАР ГОЛОВОЙ, а не любое касание по вертикали:
    // hitY истинно при каждой посадке на землю, то есть каждый кадр
    // ходьбы, — и подъём сбрасывался в том же кадре, в котором его
    // назначили.
    if (flags.onCeiling) state_.stepping = false;
    if (flags.hitZ) state_.velocity.z = 0;

    // ============================================================
    // Phase 15: snap-to-ground
    // ============================================================
    snapDown(world, dt);

    // --- Rescue: если застряли в блоке ---
    if (overlapsSolid(world, box.min(state_.position), box.max(state_.position))) {
        state_.position.y += 2.5f * dt;
    }

    settleFall(world, startedOnGround, yBeforeMove, vyBeforeMove);
}

f32 CharacterController::liquidDepthCrossed(world::ChunkManager& world,
                                            f32 fromY, f32 toY) const
{
    // Столб под серединой ступней: в жидкость входят ногами.
    const i32 x = (i32)std::floor(state_.position.x);
    const i32 z = (i32)std::floor(state_.position.z);
    world::VoxelReader rd(world);
    auto& reg = world::blocks();

    f32 k = 0.f;
    for (i32 y = (i32)std::floor(toY); y <= (i32)std::floor(fromY); ++y) {
        const f32 part = std::min(fromY, (f32)y + 1.f) - std::max(toY, (f32)y);
        if (part <= 0.f) continue;
        const u16 b = rd.at(x, y, z);
        if (b == world::UNKNOWN) continue;
        k += reg.get(b).impact.drag * part;
    }
    return k;
}

bool CharacterController::supportUnder(world::ChunkManager& world,
                                       glm::ivec3& cell, u16& block) const
{
    // Ступни стоят ровно на границе блоков: опора — на пять
    // сантиметров ниже, как и у всех проб «под ногами» в игре.
    const glm::vec3 lo = box.min(state_.position);
    const glm::vec3 hi = box.max(state_.position);
    const i32 y = (i32)std::floor(state_.position.y - 0.05f);
    auto& reg = world::blocks();
    world::VoxelReader rd(world);

    f32 best = 0.f;
    bool found = false;
    for (i32 x = (i32)std::floor(lo.x); x <= (i32)std::floor(hi.x - 1e-4f); ++x)
        for (i32 z = (i32)std::floor(lo.z); z <= (i32)std::floor(hi.z - 1e-4f); ++z) {
            const u16 b = rd.at(x, y, z);
            if (b == world::UNKNOWN || !reg.isSolid(b)) continue;
            // Под ступнями бывает до четырёх блоков: берём тот, на
            // котором стоит бóльшая часть подошвы.
            const f32 ox = std::min(hi.x, (f32)x + 1.f) - std::max(lo.x, (f32)x);
            const f32 oz = std::min(hi.z, (f32)z + 1.f) - std::max(lo.z, (f32)z);
            const f32 area = ox * oz;
            if (area > best) {
                best = area;
                cell = { x, y, z };
                block = b;
                found = true;
            }
        }
    return found;
}

// ============================================================
// Счёт падения
//
// Высота, которой стоит падение, — fallTopY. В полёте это верх
// полёта: скорость упирается в предел падения и обнуляется рывком,
// а высоту не обманешь. В жидкости она опускается вслед за
// скоростью: что погасила глубина, того при ударе о дно уже нет. На
// посадке мягкая опора гасит своё, а проломленная уходит из-под ног,
// и тело летит дальше с остатком.
// ============================================================
void CharacterController::settleFall(world::ChunkManager& world, bool startedOnGround,
                                     f32 yBefore, f32 vyBefore)
{
    state_.landingDrop = 0.f;
    state_.brokeBlock  = 0;

    const f32 y = state_.position.y;
    f32 vDown = std::max(0.f, -state_.velocity.y);

    // ---- Жидкость гасит нырок по пройденной глубине ----
    //
    // По пути, а не по времени: на шестидесяти блоках в секунду тело
    // пересекает лужу по колено за два кадра, и гашение «за кадр
    // в воде» зависело бы от того, как кадры легли на лужу. По пути
    // скорость убывает как e^(−drag·глубина) — сколько жидкости
    // пройдено, столько и погашено, при любой частоте кадров. Ниже
    // скорости плавания жидкость не гасит: там своя физика.
    bool plunged = false;
    if (vyBefore < -swimMaxFall && y < yBefore) {
        const f32 k = liquidDepthCrossed(world, yBefore, y);
        if (k > 0.f) {
            const f32 vOut = plungeSpeed(-vyBefore, k, swimMaxFall);
            if (!state_.onGround) state_.velocity.y = -vOut;
            vDown = vOut;
            plunged = true;
        }
    }

    // Высота, которой стоит скорость прямо сейчас.
    const f32 energyY = y + dropForSpeed(vDown, gravity);
    const bool inLiquid = state_.inWater || state_.inLava || plunged;

    if (!state_.onGround) {
        if (inLiquid)
            state_.fallTopY = std::min(std::max(state_.fallTopY, y), energyY);
        else
            state_.fallTopY = std::max(state_.fallTopY, energyY);
        return;
    }

    // Ударился о дно в том же кадре, в котором пересёк воду: удар —
    // с тем, что вода успела погасить.
    if (plunged) state_.fallTopY = std::min(state_.fallTopY, energyY);

    if (!startedOnGround) {
        f32 drop = std::max(0.f, state_.fallTopY - y);
        glm::ivec3 cell{0};
        u16 block = 0;
        if (drop > 0.f && supportUnder(world, cell, block)) {
            const world::BlockImpact& m = world::blocks().get(block).impact;
            if (m.absorb > 0.f || m.giveWay > 0.f) {
                const ImpactOutcome o = resolveImpact(m, drop);
                if (o.gaveWay) {
                    // Опора уходит из-под ног: тело летит дальше с
                    // тем, что осталось от падения.
                    world.setVoxel(cell.x, cell.y, cell.z, world::AIR);
                    state_.brokeBlock = block;
                    state_.brokeCell  = cell;
                    state_.onGround   = false;
                    state_.coyoteTimer = 0.f;
                    state_.velocity.y = -speedForDrop(o.dropAfter, gravity);
                    state_.fallTopY   = y + o.dropAfter;
                    return;
                }
                drop = o.dropAfter;
            }
        }
        state_.landingDrop = drop;
    }
    state_.fallTopY = y;
}

} // namespace physics
