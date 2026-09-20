/**
 * @file iso_projection.cpp
 * @brief Строгая ортографическая изометрия: область, камера, масштаб.
 */
#include "iso_projection.h"
#include <algorithm>
#include <cmath>

namespace render::iso {

Camera makeCamera(const Box& box, View v, f32 pixelsPerUnit, u32 maxSide) {
    Camera cam;

    // Отступ камеры назад: половина диагонали коробки плюс запас.
    // Число здесь ни на что не влияет — при ортографии картинка от
    // положения камеры вдоль взгляда не зависит вовсе, — но коробка
    // обязана целиком лечь между ближней и дальней плоскостями.
    const glm::vec3 ext{ (f32)box.area.size, box.yMax - box.yMin,
                         (f32)box.area.size };
    const f32 diag    = glm::length(ext);
    const f32 backOff = diag;

    cam.view = makeView(box, v, backOff);

    // Границы кадра — РАЗМАХ ВОСЬМИ УГЛОВ в пространстве вида, а не
    // подобранное число. Тогда область занимает кадр ровно один раз:
    // ни полей, ни обрезки, и это свойство не зависит ни от стороны
    // обзора, ни от размера области.
    glm::vec3 cs[8];
    box.corners(cs);
    f32 minX = 1e30f, maxX = -1e30f;
    f32 minY = 1e30f, maxY = -1e30f;
    f32 minZ = 1e30f, maxZ = -1e30f;
    for (const glm::vec3& c : cs) {
        const glm::vec3 p = glm::vec3(cam.view * glm::vec4(c, 1.f));
        minX = std::min(minX, p.x); maxX = std::max(maxX, p.x);
        minY = std::min(minY, p.y); maxY = std::max(maxY, p.y);
        // Взгляд смотрит в -Z пространства вида, поэтому глубина — это -z.
        minZ = std::min(minZ, -p.z); maxZ = std::max(maxZ, -p.z);
    }

    cam.spanX = maxX - minX;
    cam.spanY = maxY - minY;

    // Масштаб один на оба измерения: иначе блок в одном месте
    // картинки оказался бы шире, чем такой же блок в другом.
    f32 ppu = pixelsPerUnit > 0.f ? pixelsPerUnit : 1.f;
    if (maxSide > 0) {
        const f32 longest = std::max(cam.spanX, cam.spanY);
        if (longest > 0.f) {
            const f32 cap = (f32)maxSide / longest;
            if (ppu > cap) ppu = cap;
        }
    }
    cam.pixelsPerUnit = ppu;

    // Ширина и высота — целые, поэтому кадр чуть шире размаха.
    // Разницу забирает проекция, иначе пиксели стали бы
    // неквадратными, и одинаковые блоки поехали бы по одной оси.
    cam.width  = (u32)std::max(1.f, std::ceil(cam.spanX * ppu));
    cam.height = (u32)std::max(1.f, std::ceil(cam.spanY * ppu));

    const f32 halfW = (f32)cam.width  * 0.5f / ppu;
    const f32 halfH = (f32)cam.height * 0.5f / ppu;
    const f32 cx    = (minX + maxX) * 0.5f;
    const f32 cy    = (minY + maxY) * 0.5f;

    // Запас по глубине: коробка целиком между плоскостями.
    const f32 nearZ = minZ - 1.f;
    const f32 farZ  = maxZ + 1.f;

    cam.proj = glm::ortho(cx - halfW, cx + halfW,
                          cy - halfH, cy + halfH,
                          nearZ, farZ);
    // Как и в игровой камере: в координатах отсечения Vulkan ось Y
    // смотрит вниз. Одно и то же правило, записанное в двух местах, —
    // но правило это Vulkan'а, а не наше, и общего кода у матриц нет.
    cam.proj[1][1] *= -1.f;
    return cam;
}

glm::vec2 worldToImage(const Camera& cam, const glm::vec3& world) {
    const glm::vec4 clip = cam.viewProj() * glm::vec4(world, 1.f);
    // При ортографии w всегда 1: делить не на что, и это ровно то,
    // что отличает её от перспективы.
    const f32 ndcX = clip.x / clip.w;
    const f32 ndcY = clip.y / clip.w;
    return { (ndcX * 0.5f + 0.5f) * (f32)cam.width,
             (ndcY * 0.5f + 0.5f) * (f32)cam.height };
}

} // namespace render::iso
