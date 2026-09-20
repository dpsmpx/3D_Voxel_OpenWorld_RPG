/**
 * @file spawner.h
 * @brief Мобы: определения, конечный автомат ИИ, спавн, боссы.
 */
#pragma once
#include "../core/types.h"
#include "../ecs/registry.h"
#include "../world/chunk_manager.h"
#include "../world/terrain.h"
#include "../world/day_cycle.h"
#include "../world/features.h"
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <glm/glm.hpp>

namespace mobs {

/// Кто водится в биоме.
///
/// Свободная функция, а не метод спавнера: таблица «биом → зверь» —
/// это факт о мире, и спросить её должно быть можно, не заводя
/// спавнера с его таймерами и счётчиками.
u16 mobIdForBiome(world::BiomeId biome, bool night, u32 rng);

/// Создать тварь в точке.
///
/// Свободная функция, а не метод спавнера: «как устроена тварь» — это
/// факт о мире, и поставить одну на проверочную площадку должно быть
/// можно, не заводя спавнера с его таймерами, счётчиками по чанкам и
/// списком сработавших засад.
///
/// Возвращает пустую сущность, если тело в это место не помещается:
/// место ищется по ВСЕМУ телу, а не по двум блокам воздуха.
ecs::Entity spawnMob(world::ChunkManager& world,
                     ecs::Registry& reg,
                     u16 mobId,
                     const glm::vec3& pos);

class Spawner {
public:
    /// day — игровые сутки: от них зависит, какие мобы появятся.
    void update(world::ChunkManager& world,
                ecs::Registry& reg,
                const glm::vec3& playerPos,
                const world::DayCycle& day,
                u64 worldSeed,
                f32 dt);

    u32 mobCount() const { return mobCount_; }

    /// Сколько засад уже сработало. Спрашивают проверки: «сработала
    /// ли» — это и есть всё поведение засады.
    u32 sprungAmbushCount() const { return (u32)sprung_.size(); }

private:
    /// Лимиты
    static constexpr u32 MAX_MOBS_TOTAL    = 60;
    static constexpr u32 MAX_MOBS_PER_CHUNK = 3;
    /// Предел в логове. Область, по которой изредка пробегает волк,
    /// волчьим логовом не выглядит.
    static constexpr u32 MAX_MOBS_PER_LAIR_CHUNK = 6;
    static constexpr f32 SPAWN_RADIUS       = 40.f;
    static constexpr f32 SPAWN_RADIUS_MIN   = 16.f;
    static constexpr f32 DESPAWN_RADIUS     = 64.f;

    /// ---- Засады ----
    ///
    /// Обычный спавн высыпает мобов по одному и подальше от игрока —
    /// встретить их можно, нарваться нельзя. Засада — противоположное:
    /// четверо разом и вплотную, в заранее известном месте, один раз.
    ///
    /// Места берутся на дорогах между деревнями: засада имеет смысл
    /// там, где ходят.
    void updateAmbushes(world::ChunkManager& world, ecs::Registry& reg,
                        const glm::vec3& playerPos, u64 worldSeed);

    f32 ambushTimer_ = 0.f;
    /// Сработавшие: второй раз в том же месте засады не бывает.
    std::vector<u64> sprung_;

    f32 spawnTimer_ = 0.f;
    f32 despawnTimer_ = 0.f;
    f32 bossTimer_ = 0.f;
    u32 mobCount_ = 0;

    /// Подземелья, в которых босс уже поставлен: второй раз не спавним.
    /// Сколько тварей стоит в одной постройке.
    ///
    /// Шесть: меньше — и подземелье проходится насквозь без
    /// единой встречи, больше — и в колодце дерева не
    /// развернуться.
    static constexpr u32 GARRISON_PER_SITE = 6;

    std::unordered_set<u64> bossPlaced_;
    /// Ячейки, чей гарнизон уже поставлен: второй раз он
    /// набивал бы дерево тварями до отказа.
    std::unordered_set<u64> garrisonPlaced_;
    f32 garrisonTimer_ = 0.f;

    /// Track per-chunk mob count (координаты чанка → счётчик)
    std::unordered_map<world::ChunkCoord, u8, world::ChunkCoordHash> perChunk_;


    /// Кто водится в логове такого рода.
    ///
    /// Соответствие живёт здесь, а не в world: генератор мира про
    /// список мобов не знает и знать не должен — он отдаёт род
    /// логова, а кто им соответствует, решает спавнер.
    static u16 lairMobId(world::LairKind kind);

    /// Ставит боссов в подземельях рядом с игроком. Босс появляется
    /// один раз на подземелье и не участвует в обычном спавне —
    /// требование ТЗ 4.5.
    /// Гарнизон подземелья: обычные твари внутри построек.
    ///
    /// Обычный спавн до них не добирается и не доберётся: он выбирает
    /// колонку наугад по чанку, а внутренность исполинского дерева —
    /// это сотня колонок на полсотни тысяч. Подземелье из-за этого
    /// стояло пустым колодцем с одним боссом наверху.
    void updateGarrison(world::ChunkManager& world, ecs::Registry& reg,
                        const glm::vec3& playerPos, u64 worldSeed, f32 dt);

    void updateBosses(world::ChunkManager& world, ecs::Registry& reg,
                      const glm::vec3& playerPos, u64 worldSeed, f32 dt);

    /// Освещённость точки, [0, 1]: небесный свет, если над точкой
    /// открытое небо, иначе темнота пещеры. Враждебные мобы
    /// появляются только в темноте — требование ТЗ 4.5.
    static f32 lightAt(world::ChunkManager& world, const world::DayCycle& day,
                       i32 x, i32 y, i32 z);
};

} // namespace mobs
