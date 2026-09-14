/**
 * @file lod.h
 * @brief Выбор уровня детализации: одна формула на весь проект.
 */
#pragma once
#include "../core/types.h"
#include "chunk.h"
#include <cmath>
#include <glm/glm.hpp>

namespace world {

/// Границы уровней в блоках.
struct LodBands {
    f32 lod0 = 64.f, lod1 = 160.f, lod2 = 320.f;

    /// Границы привязаны к дальности прорисовки, а не зашиты числами.
    ///
    /// Последний уровень уведён ЗА туман. Туман кончается на 0.94
    /// дальности, а третий уровень начинался на 0.90 — то есть девять
    /// блоков самой грубой геометрии оказывались на виду, у самого
    /// горизонта, где её ничем не скрыть. Теперь третий уровень
    /// начинается позже, чем кончается туман, и увидеть его нельзя.
    void fromViewDistance(f32 blocks) {
        lod0 = blocks * 0.35f < 64.f  ? 64.f  : blocks * 0.35f;
        lod1 = blocks * 0.60f < 140.f ? 140.f : blocks * 0.60f;
        lod2 = blocks * 0.98f;
        if (lod1 < lod0 * 1.2f) lod1 = lod0 * 1.2f;
        if (lod2 < lod1 * 1.2f) lod2 = lod1 * 1.2f;
    }

    f32 bound(u8 lod) const {
        return lod == 0 ? lod0 : (lod == 1 ? lod1 : lod2);
    }
};

/// Мёртвая зона у границы: пока чанк не отошёл от неё на восьмую
/// часть, уровень не меняется. Без неё шаг вперёд-назад на самой
/// границе перестраивает меш каждый кадр, и рельеф мерцает.
constexpr f32 LOD_HYSTERESIS = 0.125f;

/// Центр чанка в мире. Именно от него меряют расстояние ОБЕ стороны:
/// и мир, решающий, какой уровень строить, и рендер, решающий, какой
/// рисовать. Раньше мир мерил от координаты чанка до чанка игрока, а
/// рендер — от центра чанка до камеры. Величины разные, и у границы
/// уровня стороны бесконечно переспоривали друг друга: мир строил
/// один уровень, рендер тут же требовал другой.
inline glm::vec3 chunkCenter(i32 cx, i32 cz) {
    return { (f32)cx * (f32)CHUNK_SIZE + (f32)CHUNK_SIZE * 0.5f,
             (f32)CHUNK_SIZE_Y * 0.5f,
             (f32)cz * (f32)CHUNK_SIZE + (f32)CHUNK_SIZE * 0.5f };
}

inline u8 lodForDistanceSq(f32 distSq, const LodBands& b) {
    if (distSq < b.lod0 * b.lod0) return 0;
    if (distSq < b.lod1 * b.lod1) return 1;
    if (distSq < b.lod2 * b.lod2) return 2;
    return 3;
}

/// То же, но с мёртвой зоной вокруг текущего уровня.
inline u8 lodForDistanceSq(f32 distSq, const LodBands& b, u8 current) {
    const u8 want = lodForDistanceSq(distSq, b);
    if (want == current || current > 3) return want;
    const f32 edge = (want > current) ? b.bound(current) : b.bound(want);
    const f32 lo = edge * (1.f - LOD_HYSTERESIS);
    const f32 hi = edge * (1.f + LOD_HYSTERESIS);
    const f32 d = std::sqrt(distSq);
    return (d > lo && d < hi) ? current : want;
}

/// Уровень чанка по положению камеры. Единственная точка, где это
/// решается: и мир, и рендер зовут именно её.
inline u8 lodForChunk(i32 cx, i32 cz, const glm::vec3& cameraPos,
                      const LodBands& b, u8 current = 0xFF) {
    const glm::vec3 d = chunkCenter(cx, cz) - cameraPos;
    return lodForDistanceSq(glm::dot(d, d), b, current);
}

/// То же для любого типа с полями x и z. Шаблон, а не перегрузка по
/// ChunkCoord: тот объявлен в chunk_manager.h, который сам включает
/// этот заголовок, и ссылаться на него отсюда нельзя.
template<typename Coord>
inline u8 lodForChunk(const Coord& c, const glm::vec3& cameraPos,
                      const LodBands& b, u8 current = 0xFF) {
    return lodForChunk((i32)c.x, (i32)c.z, cameraPos, b, current);
}

} // namespace world
