/**
 * @file player_rig.cpp
 * @brief Оснастка игрока: тот же двуногий, что у NPC.
 */
#include "player_rig.h"

namespace player {

const entity::Rig& rig() {
    static entity::Rig cached;
    static bool built = false;
    if (!built) {
        entity::HumanoidSpec spec;
        spec.height = PLAYER_HEIGHT;

        // Игрока надо узнавать со спины и с расстояния пяти метров —
        // именно оттуда на него и смотрят. Поэтому цвета выбраны на
        // контрасте с селянами, у которых серо-коричневая гамма:
        // тёплая охряная туника, тёмно-синие рукава и штаны.
        spec.bodyColor   = 0xD08A3CFFu;   // туника
        spec.headColor   = 0xE8C49AFFu;   // кожа
        spec.accentColor = 0x35406AFFu;   // рукава и штаны

        // Игрок чуть плечистее селянина: силуэт читается быстрее.
        spec.shoulderFrac = 0.36f;
        spec.limbFrac     = 0.11f;

        cached = entity::humanoidRig(spec);
        built = true;
    }
    return cached;
}

} // namespace player
