#include "instanced_renderer.h"
#include "../core/log.h"
#include "../world/block.h"
#include <glm/gtc/matrix_transform.hpp>
#include <random>

namespace render {

namespace {

// Cross-quad: две перпендикулярные плоскости, origin снизу.
struct Vtx { glm::vec3 p; glm::vec2 uv; };

const Vtx CROSS_VERTS[8] = {
    { {-0.5f, 0.f,  0.f}, {0.f, 1.f} },   // q1 bl
    { { 0.5f, 0.f,  0.f}, {1.f, 1.f} },   // q1 br
    { { 0.5f, 1.f,  0.f}, {1.f, 0.f} },   // q1 tr
    { {-0.5f, 1.f,  0.f}, {0.f, 0.f} },   // q1 tl
    { { 0.f, 0.f, -0.5f}, {0.f, 1.f} },   // q2 bl
    { { 0.f, 0.f,  0.5f}, {1.f, 1.f} },   // q2 br
    { { 0.f, 1.f,  0.5f}, {1.f, 0.f} },   // q2 tr
    { { 0.f, 1.f, -0.5f}, {0.f, 0.f} },   // q2 tl
};

const u32 CROSS_INDICES[12] = {
    0, 1, 2, 0, 2, 3,     // q1
    4, 5, 6, 4, 6, 7,     // q2
};

// UV тайла травы (например, atlasSlot 15,15)
constexpr glm::vec2 GRASS_UV{ 15.f / 16.f, 15.f / 16.f };

} // namespace

bool InstancedRenderer::init(vk::Context& ctx, AAssetManager* mgr, VkDescriptorSetLayout descLayout) {
    dev_ = ctx.device();
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
    d.blend       = true;                // альфа-текстура травы
    d.instanced   = true;
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
                const u16 ground = world.getVoxel(wx, surf - 1, wz);
                if (ground != world::GRASS && ground != world::SAND) continue;
                if (world.getVoxel(wx, surf, wz) != world::AIR) continue;

                const i32 sy = surf;

                GrassInstance inst{};
                inst.pos = { (f32)wx + 0.5f, (f32)sy, (f32)wz + 0.5f };
                inst.scale = 0.6f + (f32)(h % 40) / 100.f;
                inst.uvOrigin = GRASS_UV;
                inst.r = 100; inst.g = 200; inst.b = 80; inst.a = 230;
                inst.yaw = (f32)(h % 628) / 100.f;  // 0..2π

                cpuInstances_.push_back(inst);
            }
        }
    }
}

void InstancedRenderer::upload(vk::Context& ctx) {
    instanceCount_ = (u32)cpuInstances_.size();
    if (instanceCount_ == 0) return;

    u64 bytes = instanceCount_ * sizeof(GrassInstance);

    if (!instanceGpu_.handle() || instanceCapacityBytes_ < bytes) {
        if (instanceGpu_.handle()) instanceGpu_.destroy();
        if (!instanceGpu_.create(dev_, ctx.physicalDevice(), bytes,
                                 vk::BufferUsage::Vertex, false)) return;
        instanceCapacityBytes_ = bytes;
    }

    auto* s = new vk::Buffer();
    s->create(dev_, ctx.physicalDevice(), bytes, vk::BufferUsage::Staging, true);
    s->write(cpuInstances_.data(), bytes);
    ctx.submitOneShot([&](VkCommandBuffer cmd) {
        VkBufferCopy c{0, 0, bytes};
        vkCmdCopyBuffer(cmd, s->handle(), instanceGpu_.handle(), 1, &c);
        VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
        mb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        mb.dstAccessMask = VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_PIPELINE_STAGE_VERTEX_INPUT_BIT, 0, 1, &mb, 0, nullptr, 0, nullptr);
    });
    s->destroy(); delete s;
}

void InstancedRenderer::render(vk::Context& ctx, const math::Frustum&) {
    if (instanceCount_ == 0 || !instanceGpu_.handle()) return;
    VkCommandBuffer cmd = ctx.currentCmd();

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_.handle());
    VkBuffer vbs[2] = { vbo_.handle(), instanceGpu_.handle() };
    VkDeviceSize off[2] = { 0, 0 };
    vkCmdBindVertexBuffers(cmd, 0, 2, vbs, off);
    vkCmdBindIndexBuffer(cmd, ibo_.handle(), 0, VK_INDEX_TYPE_UINT32);
    vkCmdDrawIndexed(cmd, indexCount_, instanceCount_, 0, 0, 0);
}

void InstancedRenderer::destroy() {
    vbo_.destroy(); ibo_.destroy(); instanceGpu_.destroy();
    pipeline_.destroy();
    shaders_.destroyAll();
}

} // namespace render
