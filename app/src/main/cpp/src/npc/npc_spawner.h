/**
 * @file npc_spawner.h
 * @brief NPC: роли, диалоги с ветвлением, поведение жителей.
 */
#pragma once
#include "../core/types.h"
#include "../ecs/registry.h"
#include "../world/chunk_manager.h"
#include <vector>

namespace npc {

/// Постоянный ключ жителя: super-chunk X, super-chunk Z и номер в
/// деревне. Одна функция на всех, кто про этот ключ спрашивает.
///
/// Ключей было два: один здесь, `u64`, второй в сохранении, `u32`, из
/// других полей. Ни один не доходил до другого, и список убитых
/// записывался одним ключом, а спрашивался — никогда.
u64 npcPersistentKey(i32 sx, i32 sz, u32 idx);

/// Один житель на своём месте.
struct NpcSpec {
    u16       typeId;
    glm::vec3 pos;
};

/// Кто живёт в деревне супер-чанка (sx, sz) и где стоит.
///
/// Вынесено наружу, потому что состав деревни — это факт о мире, а
/// не внутреннее дело спавнера: «в сторожевой стражи вдвое» иначе
/// проверить нечем, кроме как заселив полмира и пересчитав тела.
///
/// @return false, если деревни там нет
bool generateVillageNpcs(world::ChunkManager& world,
                         i32 sx, i32 sz, u64 worldSeed,
                         std::vector<NpcSpec>& out,
                         std::vector<u64>& keysOut);

/// Поселить NPC в точке.
///
/// Свободная функция, а не внутренность спавнера: «как устроен
/// житель» — это факт о мире, и поставить одного на проверочную
/// площадку должно быть можно, не заселяя полдеревни. Отсюда же её
/// зовут и сам спавнер, и посыльный: раньше состав компонентов был
/// выписан дважды и уже разошёлся — у посыльного не было ни оружия,
/// ни параметров хода.
///
/// `persistKey` — постоянный ключ особи (0 у тех, кто деревне не
/// принадлежит). От него же берётся облик.
///
/// Возвращает пустую сущность, если тело в это место не помещается.
ecs::Entity spawnNpc(world::ChunkManager& world,
                     ecs::Registry& reg,
                     u16 typeId,
                     const glm::vec3& at,
                     u64 persistKey);

/// Детерминированный спавнер NPC.
///
/// Идея: деревни генерируются в world::features::applyStructures по
/// super-chunk grid (8×8 чанков = 256×256 блоков). Раскладку каждой
/// спавнер СПРАШИВАЕТ у генератора (world::villageAt) — раньше он
/// повторял её правила у себя, с комментарием «совпадает с
/// features::structs::layoutFor». Две копии одного правила расходятся
/// молча: сдвинутый порог — и жители стоят в поле, где деревни нет.
///
/// NPC не хранятся в мире — они создаются/удаляются по мере
/// приближения/удаления игрока.
class NpcSpawner {
public:
    void update(world::ChunkManager& world,
                ecs::Registry& reg,
                const glm::vec3& playerPos,
                u64 worldSeed,
                f32 dt = 1.f / 60.f);

    u32 activeNpcCount() const { return activeCount_; }

    /// ---- Убитые ----
    ///
    /// Убитый житель удалялся через три секунды, а деревня считалась
    /// заселённой, пока жив хотя бы один её NPC. Отойдя на 260 метров
    /// и вернувшись, игрок заставал деревню в полном составе — вместе
    /// с теми, кого убил. Сохранение для этого не требовалось.
    bool isDead(u64 key) const;
    void markDead(u64 key);

    const std::vector<u64>& deadKeys() const { return deadKeys_; }
    /// Восстановление из сохранения. Список приводится к порядку:
    /// поиск по нему двоичный.
    void setDeadKeys(std::vector<u64> keys);

private:
    static constexpr i32 SUPER_CHUNKS = 8;
    static constexpr i32 SUPER_BLOCKS = SUPER_CHUNKS * 32;   // 256

    static constexpr f32 SPAWN_DIST   = 200.f;
    static constexpr f32 DESPAWN_DIST = 260.f;

    /// ---- Посыльные ----
    ///
    /// Спавнятся не в деревне, а НА ДОРОГЕ рядом с игроком: деревни
    /// стоят в двухстах пятидесяти блоках друг от друга, а деспавн
    /// срабатывает на двухстах шестидесяти. Посыльный, вышедший из
    /// деревни, исчез бы на полпути, и встретить его на дороге было
    /// бы нельзя — то есть незачем его и заводить.
    void updateCouriers(world::ChunkManager& world, ecs::Registry& reg,
                        const glm::vec3& playerPos, u64 worldSeed);

    f32 courierTimer_ = 0.f;
    f32 spawnTimer_ = 0.f;
    f32 despawnTimer_ = 0.f;
    u32 activeCount_ = 0;

    /// Ключи убитых, в возрастающем порядке.
    std::vector<u64> deadKeys_;
};

} // namespace npc