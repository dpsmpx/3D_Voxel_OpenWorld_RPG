/**
 * @file flora_batch.h
 * @brief Рендер: сборка кадра растений — экземпляры по мешам, без видеокарты.
 *
 * Растение в чанке — запись в десять байт (world::FloraInstance), а
 * модель у вида одна на весь мир (render/flora_models.h). Кадр
 * собирается так: по видимым чанкам записи превращаются в экземпляры
 * по 16 байт, экземпляры раскладываются по мешам (модель × ступень
 * детальности), и каждый меш рисуется одним вызовом на все свои
 * экземпляры (render/flora_renderer.h).
 *
 * Мелочь видна вблизи, деревья — до тумана: у каждого класса своя
 * дальность, и за ней записи класса даже не перебираются (в чанке они
 * лежат по классам). У границы дальности трава и цветы не выскакивают
 * из земли разом, а вырастают — размер плавно идёт от нуля.
 */
#pragma once
#include "../core/types.h"
#include "../world/flora_types.h"
#include <glm/glm.hpp>
#include <vector>

namespace render {

/// Экземпляр растения на видеокарте: 16 байт.
struct FloraGpuInstance {
    glm::vec3 pos;      ///< основание модели в мире
    /// Байты в порядке чтения видеокартой: поворот (доля оборота),
    /// размер (доля FLORA_MAX_SCALE), гибкость на ветру, оттенок.
    u32       params;
};
static_assert(sizeof(FloraGpuInstance) == 16);

/// Наибольший размер экземпляра: байт размера — его доля. Совпадает
/// с MAX_SCALE в shaders/flora.vert.
constexpr f32 FLORA_MAX_SCALE = 1.5f;

/// Дальности по классам, в блоках по горизонтали: до какой видно и
/// где меняются ступени детальности.
struct FloraRange {
    f32 visible;
    f32 lod1;   ///< дальше — вторая ступень
    f32 lod2;   ///< дальше — третья
    f32 lod3;   ///< дальше — четвёртая
};
constexpr FloraRange FLORA_RANGES[(u32)world::FloraClass::Count] = {
    { 1e9f, 22.f, 60.f, 130.f },   // деревья — до тумана
    { 64.f, 24.f, 1e9f, 1e9f  },   // кусты, камни, коряги
    { 40.f, 18.f, 1e9f, 1e9f  },   // трава, цветы, камешки
};

/// Сборка кадра без видеокарты — отдельно, чтобы проверять.
class FloraBatch {
public:
    struct Draw { u32 mesh = 0, first = 0, count = 0; };

    /// Предел экземпляров на кадр: страховка от неожиданно густого
    /// леса, а не рабочая граница (обычный кадр — единицы тысяч).
    static constexpr u32 MAX_INSTANCES = 1u << 16;

    void begin(const glm::vec3& camera);
    /// Растения чанка с началом origin (мировые координаты угла).
    void addChunk(const glm::vec3& origin, const std::vector<world::FloraInstance>& list);
    /// Разложить по мешам. meshQuads — квадов в каждом меше (номер
    /// floraModelIndex * FLORA_LODS + ступень): пустые меши
    /// пропускаются, сумма идёт в сводку. nullptr — не пропускать.
    void finish(const std::vector<u32>* meshQuads);

    const std::vector<FloraGpuInstance>& instances() const { return sorted_; }
    const std::vector<Draw>& draws() const { return draws_; }
    u32 quads() const { return quads_; }

private:
    glm::vec3 camera_{0.f};
    struct Pending { u32 key; FloraGpuInstance inst; };
    std::vector<Pending> pending_;
    std::vector<u32> counts_;   ///< начало каждого ключа; последний — всего
    std::vector<u32> cursor_;
    std::vector<FloraGpuInstance> sorted_;
    std::vector<Draw> draws_;
    u32 quads_ = 0;
};

} // namespace render
