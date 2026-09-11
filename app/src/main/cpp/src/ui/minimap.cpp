#include "minimap.h"
#include "../world/block.h"
#include "../core/log.h"
#include <cmath>
#include <cstring>

namespace ui {

namespace {

struct MapColor { u8 r, g, b, a; };

MapColor mapColorFor(u16 id) {
    using namespace world;
    switch (id) {
        case STONE:    return { 110, 110, 120, 255 };
        case DIRT:     return { 100,  70,  45, 255 };
        case GRASS:    return {  80, 140,  60, 255 };
        case SAND:     return { 210, 190, 120, 255 };
        case WATER:    return {  60, 100, 180, 200 };
        case WOOD:     return { 120,  80,  40, 255 };
        case LEAVES:   return {  60, 130,  50, 255 };
        case SNOW:     return { 240, 245, 250, 255 };
        case ICE:      return { 180, 220, 240, 230 };
        case LAVA:     return { 230,  80,  20, 255 };
        case IRON_ORE: return { 140, 140, 145, 255 };
        case GOLD_ORE: return { 200, 180,  80, 255 };
        case BEDROCK:  return {  50,  50,  55, 255 };
        case AIR:      return {   0,   0,   0,   0 };
        default:       return { 150, 150, 150, 255 };
    }
}

} // namespace

u32 blockMapColor(u16 blockId) {
    MapColor c = mapColorFor(blockId);
    return ((u32)c.r << 24) | ((u32)c.g << 16) | ((u32)c.b << 8) | c.a;
}

bool Minimap::init(vk::Context& ctx, u32 px) {
    if (px < 32)   px = 32;
    if (px > 512)  px = 512;

    dev_ = ctx.device();
    size_ = px;
    pixels_.assign(size_ * size_ * 4, 0);

    if (!tex_.create(ctx.device(), ctx.physicalDevice(),
                     ctx.gfxQueue(), ctx.gfxFamily(),
                     size_, size_,
                     VK_FORMAT_R8G8B8A8_UNORM,
                     pixels_.data(), pixels_.size(),
                     VK_FILTER_NEAREST,
                     VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                     false)) {
        LOGE("Minimap: не удалось создать текстуру");
        return false;
    }

    LOGI("Minimap готов: %ux%u", size_, size_);
    return true;
}

void Minimap::destroy() {
    tex_.destroy();
    pixels_.clear();
    dev_ = VK_NULL_HANDLE;
}

void Minimap::update(world::ChunkManager& world,
                     const glm::vec3& playerPos,
                     f32 radiusBlocks)
{
    if (size_ == 0) return;
    if (radiusBlocks < 8.f) radiusBlocks = 8.f;

    center_ = playerPos;
    blocksPerPixel_ = (radiusBlocks * 2.f) / (f32)size_;

    const f32 half = (f32)size_ * 0.5f;
    const auto& gen = world.generator();

    for (u32 py = 0; py < size_; ++py) {
        for (u32 px = 0; px < size_; ++px) {
            f32 dxBlocks = ((f32)px - half) * blocksPerPixel_;
            f32 dzBlocks = ((f32)py - half) * blocksPerPixel_;

            i32 wx = (i32)std::floor(playerPos.x + dxBlocks);
            i32 wz = (i32)std::floor(playerPos.z + dzBlocks);

            i32 surfaceY = gen.surfaceHeight(wx, wz);

            u16 found = world::AIR;
            i32 yStart = surfaceY + 4;
            i32 yEnd = (surfaceY - 8 < 1) ? 1 : (surfaceY - 8);

            for (i32 y = yStart; y >= yEnd; --y) {
                u16 b = world.getVoxel(wx, y, wz);
                if (b == world::AIR) continue;
                found = b;
                if (b == world::WATER) continue;
                break;
            }

            MapColor c = mapColorFor(found);
            usize idx = ((usize)py * size_ + (usize)px) * 4;
            pixels_[idx + 0] = c.r;
            pixels_[idx + 1] = c.g;
            pixels_[idx + 2] = c.b;
            pixels_[idx + 3] = c.a;
        }
    }

    dirty_ = true;
}

void Minimap::flushUpload(vk::Context& ctx) {
    if (!dirty_) return;
    if (!tex_.view()) return;

    // Создаём временный command pool для upload.
    // Используем ctx.submitOneShot, но там свой pool — а нам нужен
    // доступ к pool для передачи в Texture2D::upload.
    // Простейший путь: вызываем upload через submitOneShot с ручным
    // созданием command buffer'а внутри Texture2D::upload с pool,
    // полученным из ctx через getter. Здесь — используем стандартный
    // путь: у vk::Context нет публичного pool для Texture2D.
    //
    // Компромисс: для каждой загрузки миникарты используем один
    // одноразовый пул внутри Texture2D::upload. Это допустимо,
    // потому что загрузка происходит редко (раз в 1 сек).

    // vk::Texture2D::upload требует pool+queue. Чтобы не менять сигнатуру
    // vk::Context, создаём локальный пул здесь.
    // Проще всего: положиться на то, что миникарта обновляется редко,
    // и upload сделает всё синхронно.

    // Пул уже не нужен: Texture2D::upload получит его через
    // встроенный в ctx.gfxFamily(). Однако у нас нет доступа к
    // gfxFamily()-pool'у без публичного API. Используем
    // submitOneShot для простоты.

    // Финальный вариант: пересоздаём текстуру раз в update,
    // но так как это дорого, делаем upload только если dirty.

    // Реализация upload через submitOneShot невозможна без доступа к
    // внутреннему буферу. Поэтому используем Texture2D::upload
    // с локальным pool.

    VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pci.queueFamilyIndex = ctx.gfxFamily();
    pci.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;

    VkCommandPool localPool = VK_NULL_HANDLE;
    if (vkCreateCommandPool(ctx.device(), &pci, nullptr, &localPool) != VK_SUCCESS) {
        dirty_ = false;
        return;
    }

    tex_.upload(ctx.device(), ctx.physicalDevice(),
                localPool, ctx.gfxQueue(),
                pixels_.data(), pixels_.size());

    vkDestroyCommandPool(ctx.device(), localPool, nullptr);
    dirty_ = false;
}

} // namespace ui
