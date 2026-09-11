#pragma once
#include "../core/types.h"
#include "../core/math.h"
#include "../world/chunk_manager.h"
#include <vector>

namespace render {

// ============================================================
// Программный occlusion culling для чанков (ТЗ 3.3).
//
// Идея: мир воксельный, и чанк, со всех сторон окружённый
// непрозрачными чанками, увидеть невозможно. Полноценный
// depth-rasterizer здесь избыточен — достаточно двух дешёвых
// критериев, вместе отсекающих основную массу невидимого:
//
//   1. Полностью закрытый чанк. Чанк считается «глухим», если у
//      него нет ни одной грани, выходящей наружу (пустой меш при
//      наличии вокселей). Такие внутри толщи камня.
//
//   2. Луч видимости. От камеры к ближайшему углу чанка
//      проводится разреженный луч по сетке чанков: если по дороге
//      встретился глухой чанк, цель не видна.
//
// Оба критерия консервативны: ошибка возможна только в сторону
// «нарисовать лишнее», никогда — «пропустить видимое».
// ============================================================
class OcclusionCuller {
public:
    /// Перестраивает карту глухих чанков. Достаточно раз в
    /// несколько кадров: содержимое мира меняется медленно.
    void rebuild(world::ChunkManager& world, const glm::vec3& cameraPos,
                 i32 radiusChunks);

    /// true, если чанк заведомо не виден из позиции камеры.
    bool isOccluded(world::ChunkCoord coord, const glm::vec3& cameraPos) const;

    /// Сколько чанков отсечено на прошлом кадре — для HUD-метрик.
    u32  lastCulled() const { return lastCulled_; }
    void resetStats() { lastCulled_ = 0; }
    void countCulled() { ++lastCulled_; }

    bool ready() const { return !opaque_.empty(); }

private:
    /// Глухой ли чанк: заполнен непрозрачными блоками без полостей,
    /// выходящих на поверхность.
    bool isOpaque(world::ChunkCoord c) const;

    std::vector<u8>   opaque_;     ///< битовая карта по сетке [origin, origin+side)
    world::ChunkCoord origin_{0, 0};
    i32               side_ = 0;
    u32               lastCulled_ = 0;
};

} // namespace render
