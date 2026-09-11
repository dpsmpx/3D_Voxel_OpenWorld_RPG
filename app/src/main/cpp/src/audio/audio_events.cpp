/**
 * @file audio_events.cpp
 * @brief Звук: движок AAudio, процедурные эффекты, динамическая музыка.
 */
#include "audio_events.h"
#include "../world/block.h"
#include <algorithm>

namespace audio {

namespace {

// Подбор звука шага по типу блока.
SoundId footstepSoundForBlock(u16 id) {
    using namespace world;
    switch (id) {
        case GRASS:  return SOUND_FOOTSTEP_GRASS;
        case SAND:   return SOUND_FOOTSTEP_SAND;
        case SNOW:   return SOUND_FOOTSTEP_SAND;   // похоже
        case STONE:  return SOUND_FOOTSTEP_STONE;
        case BEDROCK:return SOUND_FOOTSTEP_STONE;
        case IRON_ORE:
        case GOLD_ORE: return SOUND_FOOTSTEP_STONE;
        case WOOD:   return SOUND_FOOTSTEP_WOOD;
        case LEAVES: return SOUND_FOOTSTEP_GRASS;
        case WATER:  return SOUND_FOOTSTEP_WATER;
        case DIRT:   return SOUND_FOOTSTEP_DIRT;
        case ICE:    return SOUND_FOOTSTEP_STONE;
        default:     return SOUND_FOOTSTEP_DIRT;
    }
}

SoundId hitSoundForBlock(u16 id) {
    using namespace world;
    switch (id) {
        case STONE:
        case BEDROCK:
        case IRON_ORE:
        case GOLD_ORE:
        case ICE:      return SOUND_HIT_STONE;
        case WOOD:
        case LEAVES:   return SOUND_HIT_WOOD;
        case SAND:
        case DIRT:
        case GRASS:    return SOUND_HIT_WOOD;   // глухой
        default:       return SOUND_HIT_WOOD;
    }
}

} // namespace

void AudioEvents::footstep(u16 blockId, const glm::vec3& worldPos) {
    if (!engine_) return;
    SoundId id = footstepSoundForBlock(blockId);
    engine_->play3D(id, worldPos, 0.7f);
}

void AudioEvents::jump(const glm::vec3& worldPos) {
    if (!engine_) return;
    engine_->play3D(SOUND_JUMP, worldPos, 0.8f);
}

void AudioEvents::land(const glm::vec3& worldPos, f32 impactVelocity) {
    if (!engine_) return;
    f32 gain = std::clamp(std::abs(impactVelocity) / 12.f, 0.3f, 1.2f);
    engine_->play3D(SOUND_LAND, worldPos, gain);
}

void AudioEvents::swingLight(const glm::vec3& worldPos) {
    if (!engine_) return;
    engine_->play3D(SOUND_SWING_LIGHT, worldPos, 0.7f);
}

void AudioEvents::swingHeavy(const glm::vec3& worldPos) {
    if (!engine_) return;
    engine_->play3D(SOUND_SWING_HEAVY, worldPos, 0.8f);
}

void AudioEvents::hitFlesh(const glm::vec3& worldPos) {
    if (!engine_) return;
    engine_->play3D(SOUND_HIT_FLESH, worldPos, 0.9f);
}

void AudioEvents::hitBlock(u16 blockId, const glm::vec3& worldPos) {
    if (!engine_) return;
    SoundId id = hitSoundForBlock(blockId);
    engine_->play3D(id, worldPos, 0.8f);
}

void AudioEvents::arrowShoot(const glm::vec3& worldPos) {
    if (!engine_) return;
    engine_->play3D(SOUND_ARROW_SHOOT, worldPos, 0.75f);
}

void AudioEvents::arrowHit(const glm::vec3& worldPos) {
    if (!engine_) return;
    engine_->play3D(SOUND_ARROW_HIT, worldPos, 0.75f);
}

void AudioEvents::spellCast(const glm::vec3& worldPos, u8 /*damageType*/) {
    if (!engine_) return;
    engine_->play3D(SOUND_SPELL_CAST, worldPos, 0.8f);
}

void AudioEvents::spellHit(const glm::vec3& worldPos, u8 /*damageType*/) {
    if (!engine_) return;
    engine_->play3D(SOUND_SPELL_HIT, worldPos, 0.9f);
}

void AudioEvents::pickupItem() {
    if (!engine_) return;
    engine_->play(SOUND_PICKUP_ITEM, 0.7f);
}

void AudioEvents::pickupCoin() {
    if (!engine_) return;
    engine_->play(SOUND_PICKUP_COIN, 0.7f);
}

void AudioEvents::dropItem() {
    if (!engine_) return;
    engine_->play(SOUND_DROP_ITEM, 0.6f);
}

void AudioEvents::uiClick() {
    if (!engine_) return;
    engine_->play(SOUND_UI_CLICK, 0.6f);
}

void AudioEvents::uiBack() {
    if (!engine_) return;
    engine_->play(SOUND_UI_BACK, 0.6f);
}

void AudioEvents::uiError() {
    if (!engine_) return;
    engine_->play(SOUND_UI_ERROR, 0.7f);
}

void AudioEvents::playerHurt() {
    if (!engine_) return;
    engine_->play(SOUND_PLAYER_HURT, 0.9f);
}

void AudioEvents::playerDeath() {
    if (!engine_) return;
    engine_->play(SOUND_PLAYER_DEATH, 1.0f);
}

void AudioEvents::levelUp() {
    if (!engine_) return;
    engine_->play(SOUND_LEVEL_UP, 1.0f);
}

void AudioEvents::mobHurt(const glm::vec3& worldPos) {
    if (!engine_) return;
    engine_->play3D(SOUND_MOB_HURT, worldPos, 0.8f);
}

void AudioEvents::mobDeath(const glm::vec3& worldPos) {
    if (!engine_) return;
    engine_->play3D(SOUND_MOB_DEATH, worldPos, 0.9f);
}

void AudioEvents::mobAttack(const glm::vec3& worldPos) {
    if (!engine_) return;
    engine_->play3D(SOUND_MOB_ATTACK, worldPos, 0.7f);
}

void AudioEvents::blockBreak(u16 blockId, const glm::vec3& worldPos) {
    if (!engine_) return;
    SoundId id = (blockId == world::STONE || blockId == world::IRON_ORE ||
                  blockId == world::GOLD_ORE)
        ? SOUND_HIT_STONE
        : SOUND_BLOCK_BREAK;
    engine_->play3D(id, worldPos, 0.8f);
}

void AudioEvents::blockPlace(u16 /*blockId*/, const glm::vec3& worldPos) {
    if (!engine_) return;
    engine_->play3D(SOUND_BLOCK_PLACE, worldPos, 0.7f);
}

void AudioEvents::craft() {
    if (!engine_) return;
    engine_->play(SOUND_CRAFT, 0.85f);
}

void AudioEvents::enchant() {
    if (!engine_) return;
    engine_->play(SOUND_ENCHANT, 0.9f);
}

} // namespace audio
