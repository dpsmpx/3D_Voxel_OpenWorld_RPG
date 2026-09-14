/**
 * @file minimap.cpp
 * @brief Интерфейс: immediate-mode UI поверх Vulkan, HUD, меню, миникарта.
 */
#include "minimap.h"
#include "../world/block.h"
#include "../core/log.h"
#include <algorithm>
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
    pixels_.assign((usize)size_ * size_ * 4, 0);

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
    if (pool_ != VK_NULL_HANDLE && dev_ != VK_NULL_HANDLE) {
        vkDestroyCommandPool(dev_, pool_, nullptr);
        pool_ = VK_NULL_HANDLE;
    }
    tex_.destroy();
    pixels_.clear();
    dev_ = VK_NULL_HANDLE;
}

// ============================================================
// Перерисовка миникарты — по строкам, а не целиком.
//
// Полный проход по 128x128 — это шестнадцать тысяч точек. Дорога в
// них была высота поверхности: пока она бралась у генератора, каждая
// точка прогоняла все шумовые поля колонки, и проход стоил без
// малого восемь миллисекунд, из которых семь и восемь десятых —
// ровно высоты. Теперь высота берётся из уже сгенерированного чанка
// (Chunk::surfaceY), где она посчитана один раз при генерации.
//
// Проход всё равно разбит на порции строк и растянут на полтора
// десятка кадров: шестнадцать тысяч чтений вокселей — это не восемь
// миллисекунд, но и не ноль, а при движении игрока проходы идут
// подряд. Пока идёт проход, карта на экране показывает прошлый
// снимок целиком: рисуем в отдельный буфер и меняем местами в самом
// конце. Иначе половина карты была бы от старого положения игрока,
// половина от нового.
// ============================================================
void Minimap::update(world::ChunkManager& world,
                     const glm::vec3& playerPos,
                     f32 radiusBlocks,
                     f32 dt)
{
    if (size_ == 0) return;
    if (radiusBlocks < 8.f) radiusBlocks = 8.f;

    sincePass_ += dt;

    if (!passActive_) {
        const f32 dx = playerPos.x - passCenter_.x;
        const f32 dz = playerPos.z - passCenter_.z;
        const bool moved = dx * dx + dz * dz > MOVE_THRESHOLD * MOVE_THRESHOLD;
        const bool stale = sincePass_ >= REFRESH_SECONDS;
        if (!moved && !stale && !pixels_.empty() && everDrawn_) return;

        passActive_  = true;
        rowCursor_   = 0;
        sincePass_   = 0.f;
        passCenter_  = playerPos;
        passScale_   = (radiusBlocks * 2.f) / (f32)size_;
        scratch_.assign((usize)size_ * size_ * 4, 0);
    }

    const f32 half = (f32)size_ * 0.5f;
    // Курсор: вертикальный проход по каждой колонке читает соседние
    // воксели одного чанка, и строка карты идёт вдоль чанка. Через
    // него же берём высоту поверхности — она уже посчитана при
    // генерации чанка и лежит рядом с вокселями.
    world::VoxelReader rd(world);

    const u32 rowEnd = std::min(size_, rowCursor_ + ROWS_PER_CALL);
    for (; rowCursor_ < rowEnd; ++rowCursor_) {
        const u32 py = rowCursor_;
        for (u32 px = 0; px < size_; ++px) {
            const f32 dxBlocks = ((f32)px - half) * passScale_;
            const f32 dzBlocks = ((f32)py - half) * passScale_;

            const i32 wx = (i32)std::floor(passCenter_.x + dxBlocks);
            const i32 wz = (i32)std::floor(passCenter_.z + dzBlocks);

            const i32 surfaceY = rd.surfaceAt(wx, wz);

            u16 found = world::AIR;
            const i32 yStart = surfaceY + 4;
            const i32 yEnd = (surfaceY - 8 < 1) ? 1 : (surfaceY - 8);

            for (i32 y = yStart; y >= yEnd; --y) {
                const u16 b = rd.at(wx, y, wz);
                if (b == world::AIR) continue;
                found = b;
                if (b == world::WATER) continue;
                break;
            }

            const MapColor c = mapColorFor(found);
            const usize idx = ((usize)py * size_ + (usize)px) * 4;
            scratch_[idx + 0] = c.r;
            scratch_[idx + 1] = c.g;
            scratch_[idx + 2] = c.b;
            scratch_[idx + 3] = c.a;
        }
    }

    if (rowCursor_ < size_) return;   // проход ещё не закончен

    pixels_.swap(scratch_);
    center_          = passCenter_;
    blocksPerPixel_  = passScale_;
    passActive_      = false;
    everDrawn_       = true;
    dirty_           = true;
}

void Minimap::flushUpload(vk::Context& ctx) {
    if (!dirty_) return;
    if (!tex_.view()) return;

    // vk::Texture2D::upload принимает пул команд, а у vk::Context
    // своего публичного пула нет. Держим один собственный: раньше он
    // создавался и уничтожался на каждой заливке, то есть раз в
    // секунду на ровном месте.
    if (pool_ == VK_NULL_HANDLE) {
        VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        pci.queueFamilyIndex = ctx.gfxFamily();
        pci.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
        if (vkCreateCommandPool(ctx.device(), &pci, nullptr, &pool_) != VK_SUCCESS) {
            LOGE("миникарта: не создан пул команд");
            dirty_ = false;
            return;
        }
    }

    tex_.upload(ctx.device(), ctx.physicalDevice(),
                pool_, ctx.gfxQueue(),
                pixels_.data(), pixels_.size());
    dirty_ = false;
}

} // namespace ui
