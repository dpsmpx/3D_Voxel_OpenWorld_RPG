/**
 * @file instanced_renderer.cpp
 * @brief Рендер: меширование чанков, отсечение, инстансинг, камера.
 */
#include "instanced_renderer.h"
#include "grass_pipeline.h"
#include "../core/log.h"
#include "../world/block.h"
#include "../world/debug_scene.h"
#include "../config/settings.h"
#include <glm/gtc/matrix_transform.hpp>
#include <random>
#include <cmath>

namespace render {


namespace {


} // namespace

bool InstancedRenderer::init(vk::Context& ctx, AAssetManager* mgr, VkDescriptorSetLayout descLayout) {
    dev_ = ctx.device();
    instances_.init(dev_, ctx.physicalDevice());
    shaders_.init(dev_, mgr);

    vk::PipelineDesc d = grassPipelineDesc(ctx.renderPass(), descLayout,
                                           ctx.depthFormat());
    if (!pipeline_.create(dev_, shaders_, d)) return false;

    // VBO
    u32 crossVertCount = 0, crossIdxCount = 0;
    const GrassVertex* CROSS_VERTS = grassVerts(crossVertCount);
    const u32* CROSS_INDICES = grassIndices(crossIdxCount);
    u64 vbBytes = (u64)crossVertCount * sizeof(GrassVertex);
    if (!vbo_.create(dev_, ctx.physicalDevice(), vbBytes, vk::BufferUsage::Vertex, false)) return false;
    auto* sVb = new vk::Buffer();
    sVb->create(dev_, ctx.physicalDevice(), vbBytes, vk::BufferUsage::Staging, true);
    sVb->write(CROSS_VERTS, vbBytes);
    ctx.submitOneShot([&](VkCommandBuffer cmd) {
        VkBufferCopy c{0, 0, vbBytes};
        vkCmdCopyBuffer(cmd, sVb->handle(), vbo_.handle(), 1, &c);
    });
    sVb->destroy(); delete sVb;

    // IBO
    u64 ibBytes = (u64)crossIdxCount * sizeof(u32);
    if (!ibo_.create(dev_, ctx.physicalDevice(), ibBytes, vk::BufferUsage::Index, false)) return false;
    auto* sIb = new vk::Buffer();
    sIb->create(dev_, ctx.physicalDevice(), ibBytes, vk::BufferUsage::Staging, true);
    sIb->write(CROSS_INDICES, ibBytes);
    ctx.submitOneShot([&](VkCommandBuffer cmd) {
        VkBufferCopy c{0, 0, ibBytes};
        vkCmdCopyBuffer(cmd, sIb->handle(), ibo_.handle(), 1, &c);
    });
    sIb->destroy(); delete sIb;

    indexCount_ = crossIdxCount;
    LOGI("InstancedRenderer готов");
    return true;
}

void InstancedRenderer::populateGrass(const world::ChunkManager& world,
                                      const glm::vec3& playerPos, f32 radius,
                                      f32 pixelsPerUnit)
{
    cpuInstances_.clear();
    const i32 R = (i32)(radius / (f32)world::CHUNK_SIZE) + 1;
    const i32 pcx = (i32)std::floor(playerPos.x / world::CHUNK_SIZE);
    const i32 pcz = (i32)std::floor(playerPos.z / world::CHUNK_SIZE);

    static world::SimplexNoise noise(0xBEEF1234);
    // Проверки грунта идут по соседним колонкам одного чанка —
    // курсор вместо поиска чанка на каждое чтение.
    world::VoxelReader rd(world);

    // Детерминированный PRNG по (x,z) — чтобы трава не "прыгала"
    auto hash = [](i32 x, i32 z) -> u32 {
        u32 h = (u32)x * 374761393u + (u32)z * 668265263u;
        h = (h ^ (h >> 13)) * 1274126177u;
        return h ^ (h >> 16);
    };

    for (i32 dz = -R; dz <= R; ++dz) {
        for (i32 dx = -R; dx <= R; ++dx) {
            i32 cx = pcx + dx, cz = pcz + dz;

            // Плотность травы на чанк
            constexpr i32 GRASS_PER_CHUNK = 24;
            for (i32 i = 0; i < GRASS_PER_CHUNK; ++i) {
                u32 h = hash(cx * 100 + i, cz * 31 + i * 7);
                i32 lx = (i32)(h % world::CHUNK_SIZE);
                i32 lz = (i32)((h >> 8) % world::CHUNK_SIZE);

                i32 wx = cx * world::CHUNK_SIZE + lx;
                i32 wz = cz * world::CHUNK_SIZE + lz;

                // Проверить расстояние
                f32 ddx = (f32)wx - playerPos.x;
                f32 ddz = (f32)wz - playerPos.z;
                if (ddx*ddx + ddz*ddz > radius*radius) continue;

                // Высоту берём через курсор: она посчитана один раз
                // при генерации чанка и лежит рядом с вокселями,
                // которые этот же курсор сейчас прочитает. Пока она
                // бралась у генератора, шесть сотен проб за
                // пересборку стоили по полмикросекунды каждая — почти
                // триста микросекунд в одном кадре, и всё ради чисел,
                // которые уже посчитаны.
                //
                // В минимальной сцене рельеф задан сценой, а не
                // генератором: высота обязана совпасть с той, что
                // подставляет tools/vkcheck, иначе трава разойдётся.
                const i32 surf = config::settingsConst().debugScene
                               ? world::SCENE_GROUND_Y
                               : rd.surfaceAt(wx, wz);

                // Трава растёт только на реальном грунте и только если
                // над ним воздух: getVoxel учитывает пещеры, воду и
                // постройки игрока, в отличие от высоты из генератора.
                const u16 ground = rd.at(wx, surf - 1, wz);
                if (ground != world::GRASS && ground != world::SAND) continue;
                if (rd.at(wx, surf, wz) != world::AIR) continue;

                const i32 sy = surf;

                // Оттенок пляшет вокруг цвета того блока, на котором
                // трава растёт: на песке она выгоревшая, на земле
                // сочная. Ровный один цвет на всю поляну выглядит
                // покрашенным.
                const world::BlockColor gc = world::blocks().get(ground).colorTop;
                const i32 jitter = (i32)(h % 39) - 19;
                auto ch = [&](u32 shift, f32 k) {
                    const i32 v = (i32)((gc >> shift) & 0xFFu);
                    const i32 r = (i32)((f32)v * k) + jitter;
                    return (u8)(r < 0 ? 0 : (r > 255 ? 255 : r));
                };

                // Пучок сходит на нет к краю радиуса.
                //
                // Без этого трава обрывается ступенькой, а у самой
                // границы каждый пучок занимает считанные пиксели:
                // непрозрачная геометрия меньше пикселя не
                // сглаживается ничем и превращается в облако мерцающих
                // точек — ровно то, на что жаловались. Уменьшая пучок
                // до нуля, мы убираем и ступеньку, и точки, и лишние
                // треугольники разом.
                const f32 dist = std::sqrt(ddx * ddx + ddz * ddz);
                const f32 fade = (radius - dist) / (radius * GRASS_FADE);
                const f32 k = fade < 0.f ? 0.f : (fade > 1.f ? 1.f : fade);
                if (k <= 0.f) continue;

                const f32 scale = (0.6f + (f32)(h % 40) / 100.f) * k;

                // Настоящий порог, а не «почти ноль».
                //
                // Затухание по расстоянию уменьшает пучок, но само по
                // себе не решает, когда он перестаёт быть виден:
                // отсечка по k отбрасывала пучки «где-то у края», а
                // экранный размер зависит ещё и от поля зрения, и от
                // разрешения, и от высоты самого пучка. Считаем его
                // прямо: высота в мире, делённая на расстояние до
                // камеры, умноженная на пиксели на единицу.
                //
                // Так порог одинаково верен и на телефоне, и в
                // офлайн-проверке, и при любой дальности прорисовки.
                const f32 eyeDx = (f32)wx + 0.5f - playerPos.x;
                const f32 eyeDy = (f32)sy - playerPos.y;
                const f32 eyeDz = (f32)wz + 0.5f - playerPos.z;
                const f32 eyeDist = std::sqrt(eyeDx * eyeDx + eyeDy * eyeDy
                                            + eyeDz * eyeDz);
                if (eyeDist > 0.001f &&
                    scale * pixelsPerUnit / eyeDist < GRASS_MIN_PIXELS)
                    continue;

                GrassInstance inst{};
                inst.pos = { (f32)wx + 0.5f, (f32)sy, (f32)wz + 0.5f };
                inst.scale = scale;
                inst.r = ch(24, 0.82f);
                inst.g = ch(16, 1.04f);
                inst.b = ch( 8, 0.72f);
                inst.a = 255;
                inst.yaw = (f32)(h % 628) / 100.f;  // 0..2π

                cpuInstances_.push_back(inst);
            }
        }
    }
}

void InstancedRenderer::upload(vk::Context& ctx) {
    (void)ctx;
    instanceCount_ = (u32)cpuInstances_.size();
    if (instanceCount_ == 0) return;
    // Пишем прямо в память, видимую процессору: ни временного буфера,
    // ни отдельной отправки в очередь, ни ожидания GPU.
    if (!instances_.write(cpuInstances_.data(),
                          (u64)instanceCount_ * sizeof(GrassInstance)))
        instanceCount_ = 0;
}

void InstancedRenderer::render(vk::Context& ctx, VkDescriptorSet set, const math::Frustum&) {
    if (instanceCount_ == 0 || !instances_.handle() || !pipeline_.valid()) return;
    VkCommandBuffer cmd = ctx.currentCmd();

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_.handle());
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                            pipeline_.layout(), 0, 1, &set, 0, nullptr);
    VkBuffer vbs[2] = { vbo_.handle(), instances_.handle() };
    VkDeviceSize off[2] = { 0, 0 };
    vkCmdBindVertexBuffers(cmd, 0, 2, vbs, off);
    vkCmdBindIndexBuffer(cmd, ibo_.handle(), 0, VK_INDEX_TYPE_UINT32);
    vkCmdDrawIndexed(cmd, indexCount_, instanceCount_, 0, 0, 0);
}

void InstancedRenderer::destroy() {
    vbo_.destroy(); ibo_.destroy(); instances_.destroy();
    pipeline_.destroy();
    shaders_.destroyAll();
}

} // namespace render
