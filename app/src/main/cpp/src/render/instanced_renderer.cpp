/**
 * @file instanced_renderer.cpp
 * @brief Рендер: меширование чанков, LOD, отсечение, инстансинг, камера.
 */
#include "instanced_renderer.h"
#include "../core/log.h"
#include "../world/block.h"
#include <glm/gtc/matrix_transform.hpp>
#include <random>
#include <cmath>

namespace render {

// Вершинный формат: перекрещенные трапеции + инстанс GrassInstance.
static const vk::VertexBinding kBindings[2] = {
    { 12,                      false },   // vec3 pos
    { sizeof(GrassInstance),   true  },
};
static const vk::VertexAttr kAttrs[5] = {
    { 0, 0, VK_FORMAT_R32G32B32_SFLOAT, 0  },   // inPos
    { 1, 1, VK_FORMAT_R32G32B32_SFLOAT, 0  },   // iPos
    { 2, 1, VK_FORMAT_R32_SFLOAT,       12 },   // iScale
    { 3, 1, VK_FORMAT_R8G8B8A8_UNORM,   16 },   // iColor
    { 4, 1, VK_FORMAT_R32_SFLOAT,       20 },   // iYaw
};

namespace {

// Пучок травы: две перекрещённые трапеции, широкие у земли и узкие
// кверху. Текстуры нет, поэтому форму должна задавать геометрия:
// прямоугольник без текстуры читается как торчащий из земли лист
// бумаги, а сужающийся клин — как трава.
struct Vtx { glm::vec3 p; };

constexpr f32 BASE = 0.5f;    // полуширина у земли
constexpr f32 TIP  = 0.07f;   // полуширина у верхушки

const Vtx CROSS_VERTS[8] = {
    { {-BASE, 0.f,  0.f  } },   // 1: низ слева
    { { BASE, 0.f,  0.f  } },   // 1: низ справа
    { { TIP,  1.f,  0.f  } },   // 1: верх справа
    { {-TIP,  1.f,  0.f  } },   // 1: верх слева
    { { 0.f,  0.f, -BASE } },   // 2: низ слева
    { { 0.f,  0.f,  BASE } },   // 2: низ справа
    { { 0.f,  1.f,  TIP  } },   // 2: верх справа
    { { 0.f,  1.f, -TIP  } },   // 2: верх слева
};

const u32 CROSS_INDICES[12] = {
    0, 1, 2, 0, 2, 3,     // трапеция 1
    4, 5, 6, 4, 6, 7,     // трапеция 2
};

} // namespace

bool InstancedRenderer::init(vk::Context& ctx, AAssetManager* mgr, VkDescriptorSetLayout descLayout) {
    dev_ = ctx.device();
    instances_.init(dev_, ctx.physicalDevice());
    shaders_.init(dev_, mgr);

    vk::PipelineDesc d{};
    d.renderPass  = ctx.renderPass();
    d.descLayout  = descLayout;
    d.vertName    = "shaders/grass.vert.spv";
    d.fragName    = "shaders/grass.frag.spv";
    d.depthFormat = ctx.depthFormat();
    d.cullMode    = VK_CULL_MODE_NONE;   // биллборд — рисуем с обеих сторон
    d.depthTest   = true;
    d.depthWrite  = true;
    // Текстуры с альфой больше нет, значит и смешивание не нужно:
    // трава пишет глубину как обычная геометрия, и её не приходится
    // сортировать — заодно пропал целый класс артефактов порядка.
    d.blend       = false;
    d.bindings     = kBindings;
    d.bindingCount = 2;
    d.attrs        = kAttrs;
    d.attrCount    = 5;
    if (!pipeline_.create(dev_, shaders_, d)) return false;

    // VBO
    u64 vbBytes = sizeof(CROSS_VERTS);
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
    u64 ibBytes = sizeof(CROSS_INDICES);
    if (!ibo_.create(dev_, ctx.physicalDevice(), ibBytes, vk::BufferUsage::Index, false)) return false;
    auto* sIb = new vk::Buffer();
    sIb->create(dev_, ctx.physicalDevice(), ibBytes, vk::BufferUsage::Staging, true);
    sIb->write(CROSS_INDICES, ibBytes);
    ctx.submitOneShot([&](VkCommandBuffer cmd) {
        VkBufferCopy c{0, 0, ibBytes};
        vkCmdCopyBuffer(cmd, sIb->handle(), ibo_.handle(), 1, &c);
    });
    sIb->destroy(); delete sIb;

    indexCount_ = 12;
    LOGI("InstancedRenderer готов");
    return true;
}

void InstancedRenderer::populateGrass(const world::ChunkManager& world,
                                      const glm::vec3& playerPos, f32 radius)
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

                const i32 surf = world.generator().surfaceHeight(wx, wz);

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

                GrassInstance inst{};
                inst.pos = { (f32)wx + 0.5f, (f32)sy, (f32)wz + 0.5f };
                inst.scale = 0.6f + (f32)(h % 40) / 100.f;
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
