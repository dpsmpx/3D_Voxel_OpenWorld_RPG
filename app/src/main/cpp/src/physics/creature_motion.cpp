/**
 * @file creature_motion.cpp
 * @brief Физика: движение существ — та же коллизия, что у игрока.
 */
#include "creature_motion.h"
#include "../world/block.h"
#include <cmath>
#include <algorithm>

namespace physics {

namespace {

/// Сколько существо тянут вверх, когда оно оказалось внутри блока.
constexpr f32 RESCUE_SPEED = 3.0f;
/// Насколько далеко притягиваем к земле при спуске.
constexpr f32 SNAP_DOWN    = 0.62f;
/// Страховка: подъём на ступень не может длиться дольше.
constexpr f32 STEP_TIMEOUT = 0.8f;
/// Насколько медленно существо должно двигаться, чтобы счесть его
/// застрявшим. Метр в секунду — это медленнее любой ходьбы.
constexpr f32 STUCK_SPEED  = 0.35f;

/// Пробы высоты подъёма: полблока и блок. Двух хватает — в мире всё
/// кратно блоку, а лишние пробы это лишние чтения вокселей.
constexpr f32 STEP_PROBES[2] = { 0.55f, 1.05f };

} // namespace

bool fits(world::ChunkManager& world, const glm::vec3& feet,
          const CreatureBody& b)
{
    const PlayerBox box = b.box();
    return !overlapsSolid(world, box.min(feet), box.max(feet));
}

bool grounded(world::ChunkManager& world, const glm::vec3& feet,
              const CreatureBody& b)
{
    const PlayerBox box = b.box();
    glm::vec3 bmin = box.min(feet);
    glm::vec3 bmax = box.max(feet);
    // Тонкий слой ПОД коробкой: опора — это то, на чём стоят, а не то,
    // что пересекают. Прежняя проверка читала один блок под центром и
    // объявляла существо стоящим, пока оно падало сквозь его верхнюю
    // половину: скорость обнулялась, и тварь зависала в воздухе.
    bmax.y = bmin.y;
    bmin.y -= 0.08f;
    return overlapsSolid(world, bmin, bmax);
}

bool spawnFooting(world::ChunkManager& world, const glm::vec3& feet,
                  const CreatureBody& b)
{
    const i32 x = (i32)std::floor(feet.x);
    const i32 y = (i32)std::floor(feet.y + 1e-3f);
    const i32 z = (i32)std::floor(feet.z);
    switch (world::footingOf(world.getVoxel(x, y - 1, z))) {
        case world::Footing::Ground: return true;
        case world::Footing::Never:  return false;
        case world::Footing::Built:  break;
    }
    // Постройка годится только изнутри: над головой должно быть
    // перекрытие. Кровля и верх стены стоят под открытым небом.
    auto& reg = world::blocks();
    for (i32 up = y + (i32)std::ceil(b.height); up < world::CHUNK_SIZE_Y; ++up)
        if (reg.isSolid(world.getVoxel(x, up, z))) return true;
    return false;
}

namespace {

/// Поиск высоты в одной колонке — см. settle.
bool settleColumn(world::ChunkManager& world, glm::vec3& feet,
                  const CreatureBody& b, i32 maxUp)
{
    // Уровни перебираются ЦЕЛЫЕ: стоят в воксельном мире на верхней
    // грани блока. Спавнеры дают то `y`, то `y + 0.5`, и перебор
    // «плюс единица» от дробной высоты не попадал на опору ни разу.
    const i32 y0 = (i32)std::floor(feet.y + 1e-3f);

    // Слой в один блок на уровне ступней, по площади тела.
    auto feetSolid = [&](i32 y) {
        const glm::vec3 lo{ feet.x - b.halfWidth, (f32)y + 0.02f,
                            feet.z - b.halfWidth };
        const glm::vec3 hi{ feet.x + b.halfWidth, (f32)y + 0.98f,
                            feet.z + b.halfWidth };
        return overlapsSolid(world, lo, hi);
    };

    // Выбираемся из толщи: пока ступни в камне, поднимаемся. Ровно до
    // ПЕРВОГО свободного уровня — и ни блоком выше.
    //
    // Раньше перебор шёл вверх до упора и брал любое подходящее
    // место. Для трёхметрового босса в двухблочном коридоре таким
    // местом оказывалась КРЫША: тело туда помещалось и опора была.
    // Лезть сквозь перекрытие ради свободного места — это ровно то,
    // от чего проверка и заведена.
    i32 y = y0;
    i32 up = 0;
    while (up <= maxUp && feetSolid(y)) { ++y; ++up; }
    if (up > maxUp) return false;

    // Тело обязано поместиться здесь — или на пару блоков ниже, если
    // точку дали чуть выше земли. И стоять на том, на чём рождаются.
    for (i32 down = 0; down <= 2; ++down) {
        const i32 yy = y - down;
        if (down > 0 && feetSolid(yy)) break;
        const glm::vec3 p{ feet.x, (f32)yy, feet.z };
        if (fits(world, p, b) && grounded(world, p, b) && spawnFooting(world, p, b)) {
            feet = p;
            return true;
        }
    }
    return false;
}

} // namespace

bool settle(world::ChunkManager& world, glm::vec3& feet,
            const CreatureBody& b, i32 maxUp)
{
    // Незагруженный чанк отвечает «камень» на любой вопрос — так
    // устроено чтение вокселей, и это правильно: мир там ещё не
    // построен, и пускать сквозь него нельзя. Но и судить по такому
    // ответу о месте нельзя тоже: проверка сказала бы «всюду камень»
    // и не дала бы родиться никому. Раз мир не может ответить —
    // оставляем точку как есть; за то, чтобы чанк был готов, отвечает
    // тот, кто заказывает рождение.
    auto loaded = [&](const glm::vec3& p) {
        return world.findChunk((i32)std::floor(p.x) >> 5,
                               (i32)std::floor(p.z) >> 5) != nullptr;
    };
    if (!loaded(feet)) return true;

    // Сначала сама точка, потом соседние клетки — ближние раньше
    // дальних. Точка у ствола или в стене дома — обычное дело: дома и
    // деревья ставит генератор, спавнер о них не знает. Лезть наверх
    // нельзя — там крона и кровля; шаг в сторону — можно.
    static constexpr i8 OFFSETS[][2] = {
        { 0, 0 },
        { 1, 0 }, { -1, 0 }, { 0, 1 }, { 0, -1 },
        { 1, 1 }, { -1, 1 }, { 1, -1 }, { -1, -1 },
        { 2, 0 }, { -2, 0 }, { 0, 2 }, { 0, -2 },
    };
    for (const auto& o : OFFSETS) {
        glm::vec3 p = feet + glm::vec3((f32)o[0], 0.f, (f32)o[1]);
        if (!loaded(p)) continue;
        if (settleColumn(world, p, b, maxUp)) { feet = p; return true; }
    }
    return false;
}

bool tryJump(CreatureMotion& m, glm::vec3& vel, const CreatureBody& b) {
    if (!m.onGround || m.stepping) return false;
    vel.y = b.jumpSpeed;
    m.onGround = false;
    return true;
}

namespace {

/// Назначить подъём на ступень, если впереди есть куда встать.
bool assignStep(world::ChunkManager& world,
                CreatureMotion& m,
                const glm::vec3& pos,
                const glm::vec2& dir,
                const CreatureBody& b)
{
    if (glm::dot(dir, dir) < 1e-6f) return false;
    const glm::vec2 d = glm::normalize(dir);
    // Щуп чуть дальше полуширины: ступень должна быть ВПЕРЕДИ, а не
    // под ногами.
    const f32 probe = b.halfWidth + 0.35f;

    for (f32 h : STEP_PROBES) {
        if (h > b.stepHeight + 1e-3f) continue;
        const glm::vec3 up{ pos.x, pos.y + h, pos.z };
        if (!fits(world, up, b)) continue;                 // самому подняться некуда
        const glm::vec3 ahead{ up.x + d.x * probe, up.y, up.z + d.y * probe };
        if (!fits(world, ahead, b)) continue;              // впереди всё равно стена
        // Под местом, куда встанем, должна быть опора: иначе это не
        // ступень, а край обрыва по ту сторону стены.
        bool support = false;
        for (f32 drop = 0.f; drop <= 1.05f && !support; drop += 0.5f) {
            glm::vec3 q = ahead;
            q.y -= drop;
            if (drop > 0.f && !fits(world, q, b)) break;
            support = grounded(world, q, b);
        }
        if (!support) continue;

        m.stepping    = true;
        m.stepTargetY = pos.y + h;
        m.stepTimer   = 0.f;
        return true;
    }
    return false;
}

/// Притянуть к земле при спуске: без этого шаг с блока вниз — это
/// маленький полёт, и существо съезжает по лестнице прыжками.
void snapDown(world::ChunkManager& world, glm::vec3& pos, glm::vec3& vel,
              const CreatureBody& b, CreatureMotion& m)
{
    for (f32 drop = 0.06f; drop <= SNAP_DOWN; drop += 0.06f) {
        glm::vec3 p = pos;
        p.y -= drop;
        if (!fits(world, p, b)) break;       // внутри блока — не наше дело
        if (!grounded(world, p, b)) continue;
        pos.y = p.y;
        vel.y = 0.f;
        m.onGround = true;
        return;
    }
}

} // namespace

void stepCreature(world::ChunkManager& world,
                  CreatureMotion& m,
                  glm::vec3& pos,
                  glm::vec3& vel,
                  const CreatureBody& b,
                  f32 dt,
                  bool wantsMove)
{
    if (dt <= 0.f) return;

    const PlayerBox box = b.box();

    // Вода определяется по середине тела, а не по блоку под ногами:
    // стоя по щиколотку в луже, существо не плывёт.
    {
        world::VoxelReader rd(world);
        const i32 mx = (i32)std::floor(pos.x);
        const i32 my = (i32)std::floor(pos.y + b.height * 0.5f);
        const i32 mz = (i32)std::floor(pos.z);
        m.inWater = (rd.at(mx, my, mz) == world::WATER);
    }

    // ---- Вертикаль ----
    if (m.stepping) {
        m.stepTimer += dt;
        if (pos.y >= m.stepTargetY - 1e-3f || m.stepTimer > STEP_TIMEOUT) {
            m.stepping = false;
            if (vel.y > 0.f) vel.y = 0.f;
        } else {
            vel.y = b.climbSpeed;
        }
    }
    if (!m.stepping) {
        if (m.inWater) {
            vel.y = b.swimRise;
        } else {
            vel.y += b.gravity * dt;
            if (vel.y < b.maxFall) vel.y = b.maxFall;
        }
    }

    const bool wasGround = m.onGround;
    const glm::vec3 before = pos;
    m.onGround = false;

    glm::vec3 delta = vel * dt;
    const glm::vec2 intent{ delta.x, delta.z };

    const CollisionFlags flags = resolveMovement(world, pos, delta, box);

    if (flags.onGround)  { m.onGround = true; m.stepping = false; }
    if (flags.onCeiling) { m.stepping = false; }
    m.hitWall = flags.hitX || flags.hitZ;

    if (flags.hitX) vel.x = 0.f;
    if (flags.hitZ) vel.z = 0.f;
    if (flags.hitY) vel.y = 0.f;

    // ---- Запрыгнуть на блок ----
    //
    // Упёрлись боком, стоя на земле и желая идти, — значит впереди
    // ступень. Если на неё есть куда встать, назначаем подъём.
    if (!m.stepping && m.hitWall && wantsMove && (wasGround || m.onGround))
        assignStep(world, m, pos, intent, b);

    // ---- Спрыгнуть с блока ----
    //
    // Притягиваем вниз только того, кто в прошлом кадре стоял на
    // земле и не прыгает: это спуск со ступени, а не отмена падения.
    if (!m.onGround && !m.inWater && !m.stepping && wasGround && vel.y <= 0.f)
        snapDown(world, pos, vel, b, m);

    // ---- Застревание ----
    const glm::vec2 moved{ pos.x - before.x, pos.z - before.z };
    if (wantsMove && glm::length(moved) < STUCK_SPEED * dt) m.stuckTime += dt;
    else                                                    m.stuckTime = 0.f;

    // ---- Спасение из блока ----
    //
    // Родились в стене, мир достроился вокруг, взрывом завалило —
    // тянем вверх, пока не выберется. Прежний вариант поднимал на
    // 0.05 за кадр безусловно, то есть выдавливал наверх и того, кто
    // просто стоял на полу.
    if (overlapsSolid(world, box.min(pos), box.max(pos))) {
        pos.y += RESCUE_SPEED * dt;
        vel.y = 0.f;
        m.stepping = false;
    }
}

} // namespace physics
