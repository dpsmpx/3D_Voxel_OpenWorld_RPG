/**
 * @file npc_rig.cpp
 * @brief Оснастка NPC: двуногий с процедурной вариацией особей.
 */
#include "npc_rig.h"
#include <array>
#include <cmath>

namespace npc {

namespace {

/// Разбиение семени на независимые потоки.
///
/// Без него рост, телосложение и цвет рубахи оказались бы связаны:
/// все высокие селяне носили бы одно и то же.
u32 mix(u32 seed, u32 salt) {
    u64 h = (u64)seed * 0x9E3779B97F4A7C15ULL;
    h ^= (u64)salt * 0xC4CEB9FE1A85EC53ULL;
    h ^= h >> 33; h *= 0xFF51AFD7ED558CCDULL;
    h ^= h >> 33;
    return (u32)h;
}

/// Число из семени в [-1, 1].
f32 spread(u32 seed, u32 salt) {
    return (f32)(mix(seed, salt) & 0xFFFF) / 32767.5f - 1.f;
}

/// Подкрутить яркость цвета, не трогая тон.
///
/// Именно яркость, а не случайные каналы: иначе у одного селянина
/// рубаха окажется зелёной, у другого лиловой, и роль перестанет
/// читаться. Облик обязан менять особь, а не вид.
u32 shade(u32 rgba, f32 k) {
    auto ch = [&](int shift) {
        const f32 v = (f32)((rgba >> shift) & 0xFF) * k;
        return (u32)(v < 0.f ? 0.f : (v > 255.f ? 255.f : v));
    };
    return (ch(24) << 24) | (ch(16) << 16) | (ch(8) << 8) | (rgba & 0xFF);
}

/// Оттенки кожи. Дискретный набор, а не дрожание одного цвета:
/// случайный сдвиг каналов даёт болезненные оттенки, которых у людей
/// не бывает.
constexpr u32 SKIN[6] = {
    0xF2D3B0FFu, 0xE8C49AFFu, 0xD2A579FFu,
    0xA9764EFFu, 0x7E5333FFu, 0x543722FFu,
};

entity::Rig buildVariant(const NpcDef& def, u8 variant) {
    const u32 seed = mix(0xA11CE5u, variant);

    entity::HumanoidSpec spec;

    // Рост: ±8%. Больше — и селянин перестаёт быть человеком, меньше
    // — разница не читается с игровой дистанции.
    spec.height = def.bodyHeight * (1.f + spread(seed, 1) * 0.08f);

    // Телосложение. Плечи и толщина конечностей независимы: бывает и
    // высокий худой, и низкий плотный.
    spec.shoulderFrac = 0.34f + spread(seed, 2) * 0.035f;
    spec.limbFrac     = 0.100f + spread(seed, 3) * 0.014f;
    spec.depthFrac    = 0.20f + spread(seed, 4) * 0.02f;

    // Длина ног за счёт торса: общий рост уже задан, и сумма долей
    // обязана остаться единицей.
    const f32 legShift = spread(seed, 5) * 0.025f;
    spec.legFrac   = 0.46f + legShift;
    spec.torsoFrac = 0.30f - legShift;

    // Цвета: роль задаёт определение вида, облик — только оттенок.
    spec.bodyColor   = shade(def.bodyColor,   1.f + spread(seed, 6) * 0.14f);
    spec.accentColor = shade(def.accentColor, 1.f + spread(seed, 7) * 0.14f);
    spec.headColor   = SKIN[mix(seed, 8) % 6];

    return entity::humanoidRig(spec);
}

} // namespace

u8 variantOf(u32 seed) {
    return (u8)(mix(seed, 0x5EED) % NPC_VARIANTS);
}

const entity::Rig& rigFor(u16 npcId, u32 seed) {
    static std::array<entity::Rig, (usize)NPC_COUNT * NPC_VARIANTS> cache{};
    static std::array<bool, (usize)NPC_COUNT * NPC_VARIANTS> built{};

    const u16 id = (npcId < NPC_COUNT) ? npcId : (u16)NPC_NONE;
    const u8  v  = variantOf(seed);
    const usize slot = (usize)id * NPC_VARIANTS + v;

    if (!built[slot]) {
        cache[slot] = buildVariant(npcRegistry().get(id), v);
        built[slot] = true;
    }
    return cache[slot];
}

} // namespace npc
