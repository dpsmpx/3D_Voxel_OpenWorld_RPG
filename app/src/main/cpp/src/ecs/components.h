/**
 * @file components.h
 * @brief Entity-Component-System: дескрипторы сущностей, sparse-set пулы, реестр.
 */
#pragma once
#include "../core/types.h"
#include "entity.h"
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <string>

namespace ecs {

// ============================================================
// Основные компоненты игрока, мобов, предметов.
// Все — POD по духу (простые структуры, тривиальное копирование).
// ============================================================

struct Transform {
    glm::vec3 position{0};
    glm::quat rotation{1,0,0,0};
    glm::vec3 scale{1};
};

struct Velocity {
    glm::vec3 linear{0};
    glm::vec3 angular{0};
};

struct Health {
    f32 current = 100.f;
    f32 max     = 100.f;
    f32 regen   = 1.f;    // hp/sec
    f32 invulnTime = 0.f;
};

struct Mana {
    f32 current = 50.f;
    f32 max     = 50.f;
    f32 regen   = 2.f;
};

struct Stamina {
    f32 current = 100.f;
    f32 max     = 100.f;
    f32 regen   = 10.f;
};

struct Attributes {
    i32 strength     = 10;
    i32 agility      = 10;
    i32 intelligence = 10;
    i32 endurance    = 10;
};

// Experience удалён: он добавлялся игроку и больше нигде не
// упоминался — ни чтения, ни записи, ни сохранения. Опыт и уровень
// живут в progression::Progression, там же таблица уровней. Два
// хранилища одного и того же — приглашение разойтись.

/// — Что за сущность
enum class EntityKind : u8 {
    Player, NPC, Mob, Projectile, Item, Structure, Effect, Unknown
};

struct Kind { EntityKind value = EntityKind::Unknown; };

/// — Теги
struct PlayerTag {};
struct EnemyTag  {};
struct NPCTag    {};

/// — Компонент для мобов/NPC
struct AIAgent {
    enum State : u8 { Idle, Patrol, Chase, Attack, Flee, Dead } state = Idle;
    Entity target{};
    glm::vec3 homePos{0};
    f32 stateTime = 0.f;
    f32 attackRange = 2.f;
    f32 aggroRange  = 12.f;
    f32 moveSpeed   = 3.f;
    f32 attackCooldown = 0.f;
};

/// — Коллайдер (для физики)
struct Collider {
    glm::vec3 halfExtents{0.4f, 0.9f, 0.4f};
    bool      isStatic = false;
};

// Здесь были Renderable (meshId/textureId/tint) и PersistentId: ни
// один из них никогда не добавлялся ни одной сущности и не читался ни
// откуда. Renderable к тому же описывал модели и текстуры, которых в
// проекте нет вовсе — рендер воксельный, материал задаётся цветом
// грани. Такие объявления не безобидны: по ним пытаются понять, как
// устроен рендер, и понимают неправильно.

} // namespace ecs
