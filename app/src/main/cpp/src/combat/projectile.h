/**
 * @file projectile.h
 * @brief Бой: урон, оружие, зачарования, система «Резонанс», статусы.
 */
#pragma once
#include "../core/types.h"
#include "../ecs/registry.h"
#include "../world/chunk_manager.h"
#include "damage.h"
#include <glm/glm.hpp>

namespace combat {

/// Компонент снаряда.
struct Projectile {
    u16             weaponId    = 0;
    glm::vec3       velocity    { 0.f };
    f32             lifeTime    = 4.f;
    f32             lifeRemaining = 4.f;

    u32             ownerEntity = 0;
    u32             ownerFaction = 0;

    DamageInstance  damage{};
    DamageInstance  secondary{};
    f32             lifesteal = 0.f;

    bool            affectedByGravity = false;

    u32             colorRGBA = 0xFFFFFFFF;
    f32             scale     = 0.15f;
    bool            isSpell   = false;
    /// Тонкая игла вдоль полёта, а не сгусток: заклинание, у
    /// которого есть направление.
    bool            needle    = false;
    /// Цвет осколков, на которые снаряд разлетается при ударе;
    /// 0 — не разлетается. Ледяная игла бьётся о камень и о
    /// противника вдребезги, и это видно.
    u32             shardColor = 0;
};

/// Короткоживущий визуальный эффект попадания.
struct HitFx {
    f32 lifeTime       = 0.22f;
    f32 lifeRemaining  = 0.22f;
    f32 startScale     = 0.25f;
    f32 endScale       = 1.1f;
    u32 colorRGBA      = 0xFFFFFFFF;
};

/// Параметры создания снаряда.
struct ProjectileSpawnParams {
    u16             weaponId      = 0;
    glm::vec3       origin        { 0.f };
    glm::vec3       direction     { 0.f, 0.f, 1.f };
    u32             ownerEntity   = 0;
    u32             ownerFaction  = 0;

    DamageInstance  damage{};
    DamageInstance  secondary{};
    f32             lifesteal     = 0.f;

    f32             speed         = 25.f;
    f32             gravity       = 0.f;
    f32             lifeTime      = 4.f;
    u32             colorRGBA     = 0xFFFFFFFF;
    f32             scale         = 0.15f;
    bool            isSpell       = false;
    bool            needle        = false;
    u32             shardColor    = 0;
};

ecs::Entity spawnProjectile(ecs::Registry& reg,
                            const ProjectileSpawnParams& params);

/// Создать визуальный эффект попадания в мировой точке.
ecs::Entity spawnHitFx(ecs::Registry& reg,
                       const glm::vec3& position,
                       u32 colorRGBA,
                       f32 startScale,
                       f32 endScale,
                       f32 lifetime);

/// Покадровое обновление снарядов.
void updateProjectiles(world::ChunkManager& world,
                       ecs::Registry& reg,
                       f32 dt);

/// Покадровое обновление HitFx.
void updateHitFx(ecs::Registry& reg, f32 dt);

// Здесь объявлялась projectileCheckEntityHit(). Тело у неё было
// «return {}» — то есть «ни в кого не попал», всегда, — и звать её
// никто не звал: попадание снаряда в сущность давно считает
// updateProjectiles(), там же, где применяет урон.

} // namespace combat
