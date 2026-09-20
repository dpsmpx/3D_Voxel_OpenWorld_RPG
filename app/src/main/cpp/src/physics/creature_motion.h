/**
 * @file creature_motion.h
 * @brief Физика: движение существ — та же коллизия, что у игрока.
 */
#pragma once
#include "../core/types.h"
#include "../world/chunk_manager.h"
#include "collision.h"
#include <glm/glm.hpp>

namespace physics {

/// Тело существа: то же, что PlayerBox, плюс способности ног.
///
/// Мобы и NPC двигались каждый своей копией «коллизии»: проба одного
/// вертикального столбца в центре, без ширины тела, без потолка, без
/// ступеней. Ширина в той пробе даже принималась параметром и не
/// использовалась ни разу — волк радиусом в полблока входил в стену
/// дома по плечи. Здесь тело одно и разрешение столкновений одно —
/// physics::resolveMovement, то самое, которым ходит игрок.
struct CreatureBody {
    f32 halfWidth  = 0.35f;
    f32 height     = 1.80f;
    /// На сколько существо поднимается без прыжка. Ровно один блок:
    /// «запрыгнуть на блок» — это и есть базовая способность.
    f32 stepHeight = 1.05f;
    f32 gravity    = -22.f;
    f32 maxFall    = -50.f;
    /// Скорость подъёма на ступень. Подъём НАЗНАЧАЕТСЯ и происходит
    /// за несколько кадров: мгновенная перестановка на метр вверх
    /// физически верна и на глаз выглядит телепортацией.
    f32 climbSpeed = 5.0f;
    /// Толчок прыжка — когда ступенью не обойтись (расщелина, обрыв).
    f32 jumpSpeed  = 7.5f;
    /// Всплытие в воде.
    f32 swimRise   = 1.5f;

    PlayerBox box() const { return PlayerBox{ halfWidth, height }; }
};

/// Состояние ног. Хранится у существа между кадрами.
struct CreatureMotion {
    bool onGround = false;
    bool inWater  = false;
    /// Поднимается на ступень прямо сейчас.
    bool stepping    = false;
    f32  stepTargetY = 0.f;
    f32  stepTimer   = 0.f;
    /// Упёрлось по горизонтали в этом кадре.
    bool hitWall = false;
    /// Сколько подряд существо хочет идти и не сдвигается с места.
    /// По нему ИИ понимает, что пора прыгать или искать путь заново.
    f32  stuckTime = 0.f;
};

/// Помещается ли тело ступнями в точку: ни один твёрдый воксель не
/// пересекает коробку.
bool fits(world::ChunkManager& world, const glm::vec3& feet,
          const CreatureBody& b);

/// Есть ли под ногами опора (в пределах тонкого слоя под коробкой).
bool grounded(world::ChunkManager& world, const glm::vec3& feet,
              const CreatureBody& b);

/// Поднять точку до ближайшей высоты, где тело помещается и стоит на
/// опоре. Возвращает false, если такой высоты нет в пределах maxUp.
///
/// Нужно при рождении: место под тварь искали по двум блокам воздуха
/// над твёрдым — и трёхметровый босс рождался по грудь в потолке
/// зала, а дальше его каждый кадр выдавливало вверх по полблока,
/// пока он не оказывался на крыше.
bool settle(world::ChunkManager& world, glm::vec3& feet,
            const CreatureBody& b, i32 maxUp = 6);

/// Один шаг движения существа.
///
/// wantsMove — есть ли у существа намерение идти. От него зависит и
/// подъём на ступень, и счёт застревания: стоящий на месте не застрял.
void stepCreature(world::ChunkManager& world,
                  CreatureMotion& m,
                  glm::vec3& pos,
                  glm::vec3& vel,
                  const CreatureBody& b,
                  f32 dt,
                  bool wantsMove);

/// Толчок вверх, если существо стоит на опоре. Возвращает false,
/// если прыгать не с чего.
bool tryJump(CreatureMotion& m, glm::vec3& vel, const CreatureBody& b);

} // namespace physics
