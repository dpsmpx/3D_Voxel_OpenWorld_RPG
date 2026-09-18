/**
 * @file player.h
 * @brief Игрок: ввод, движение, взаимодействие с миром и NPC.
 */
#pragma once
#include "../core/types.h"
#include "../ecs/registry.h"
#include "../physics/character_controller.h"
#include "../physics/raycast.h"
#include "../world/chunk_manager.h"
#include "../combat/components.h"
#include "../combat/weapon.h"
#include "../combat/resonance.h"
#include "../combat/combat_controller.h"
#include "../combat/spatial_hash.h"
#include "../progression/progression.h"
#include "../progression/skill_tree.h"
#include "../quests/quest.h"
#include "../factions/faction.h"
#include "../npc/dialogue.h"
#include "../items/inventory.h"
#include "../items/currency.h"
#include "../items/item_use.h"
#include <glm/glm.hpp>

namespace player {

enum class CameraMode : u8 {
    ThirdPerson,
    FirstPerson,
};

struct PlayerInput {
    glm::vec2 moveAxis{0};
    bool jumpPressed = false;
    bool jumpHeld    = false;
    bool sprint      = false;
    bool crouch      = false;

    bool attackPressed = false;
    bool attackHeld    = false;
    bool finisherInput = false;

    bool interactPressed = false;
};

class Player {
public:
    void init(ecs::Registry& reg, const glm::vec3& spawnPos);

    /// Базовый апдейт без spatial hash (fallback).
    void update(world::ChunkManager& world,
                const PlayerInput& input,
                f32 dt,
                f32 cameraYaw,
                f32 cameraPitch);

    /// Phase 15: апдейт с spatial hash — используется в main.
    /// hash может быть nullptr — тогда падаем на полный перебор.
    void updateWithHash(world::ChunkManager& world,
                        const combat::SpatialHash* hash,
                        const PlayerInput& input,
                        f32 dt,
                        f32 cameraYaw,
                        f32 cameraPitch);

    glm::vec3 eyePosition() const;
    glm::vec3 centerPosition() const;

    physics::RayHit targetBlock(world::ChunkManager& world, f32 reach = 5.5f) const;
    glm::vec3 aimDir() const { return aimDir_; }

    /// Ломает блок под прицелом и роняет из него предмет.
    bool tryBreakBlock(world::ChunkManager& world);
    /// Ставит блок и списывает его из активной ячейки пояса.
    bool tryPlaceBlock(world::ChunkManager& world, u16 blockType);

    ecs::Entity entity() const { return entity_; }

    /// Прямой доступ к реестру — нужен UI и другим системам.
    ecs::Registry* registryHandle() const { return reg_; }

    progression::Progression* progression();
    progression::SkillTree* skillTree();
    ecs::Attributes* attributes();
    quests::QuestLog* questLog();
    factions::Reputation* reputation();
    npc::ActiveDialogue* activeDialogue();

    items::Inventory* inventory();
    items::Wallet* wallet();

    const progression::DerivedStats& derived() const;

    void tryInteract(ecs::Registry& reg, world::ChunkManager& world);

    items::UseResult useItem(u32 slotIndex);
    bool dropItem(u32 slotIndex);

    void setActiveHotbar(u8 idx);
    u16 selectedBlock() const;

    /// Камера
    CameraMode cameraMode = CameraMode::ThirdPerson;
    f32 thirdPersonDistance = 5.5f;
    f32 thirdPersonHeight   = 1.10f;
    f32 firstPersonEye      = 1.62f;

    f32 bobPhase  = 0.f;
    f32 bobAmount = 0.f;

    physics::CharacterController controller;

    combat::EquippedWeapon   equipped;
    combat::WeaponState      weaponState;
    combat::ResonanceState   resonance;
    combat::StatusEffects    statuses;
    combat::Combatant        combatant;
    combat::CombatAction     lastAction;

    bool pendingLevelUpNotification = false;
    f32  levelUpFlashTimer = 0.f;
    u32  lastLevelGained   = 1;

    bool reputationFlashActive = false;
    f32  reputationFlashTimer  = 0.f;
    factions::FactionId lastRepFaction = factions::FactionId::None;
    factions::ReputationTier lastRepTier = factions::ReputationTier::Neutral;

    u16 legacySelectedBlock = world::STONE;

    // Phase 15: счётчик времени для разных таймеров (footstep и пр.).
    // Хранится здесь, потому что main уже отслеживает его отдельно,
    // но для внутренних событий (например, звуков удара) нужен
    // доступ к самому факту изменения состояния.
    // Пока не используется — оставлено для расширения.

private:
    /// Общая реализация для update / updateWithHash.
    void updateImpl(world::ChunkManager& world,
                    const combat::SpatialHash* hash,
                    const PlayerInput& input,
                    f32 dt,
                    f32 cameraYaw,
                    f32 cameraPitch);

    /// Списать один поставленный блок из активной ячейки пояса.
    void consumePlacedBlock(u16 blockType);

    ecs::Registry* reg_ = nullptr;
    ecs::Entity    entity_{};
    glm::vec3      aimDir_{0, 0, -1};
};

} // namespace player
