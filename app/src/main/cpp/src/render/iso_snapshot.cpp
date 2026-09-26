/**
 * @file iso_snapshot.cpp
 * @brief Изометрический снимок мира: подготовка, рендер, картинка.
 */
#include "iso_snapshot.h"
#include "flora_pipeline.h"
#include "mesh_builder.h"
#include "voxel_pipeline.h"
#include "../core/log.h"
#include "../core/math.h"
#include "../vk/vk_context.h"
#include "../world/block.h"
#include "../world/flora.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <shared_mutex>
#include <cstring>
#include <vector>

namespace render {

namespace {

constexpr VkFormat COLOR_FMT = VK_FORMAT_R8G8B8A8_UNORM;

/// Сколько кадров ждём генерацию, прежде чем сдаться.
///
/// Мир генерируется на воркерах, и сколько это займёт, зависит от
/// того, сколько чанков не хватало. Предел нужен не ради скорости, а
/// чтобы снимок не висел вечно, если планировщик задач встал.
constexpr u32 PREPARE_MAX_TICKS = 3600;      // около минуты при 60 к/с

/// Сколько чанков мешируем за один шаг. Больше — заметный провал
/// кадра; меньше — снимок готовится ощутимо дольше.
constexpr usize MESHES_PER_STEP = 2;

u32 memType(VkPhysicalDevice phys, u32 bits, VkMemoryPropertyFlags want) {
    VkPhysicalDeviceMemoryProperties mp;
    vkGetPhysicalDeviceMemoryProperties(phys, &mp);
    for (u32 i = 0; i < mp.memoryTypeCount; ++i)
        if ((bits & (1u << i)) &&
            (mp.memoryTypes[i].propertyFlags & want) == want) return i;
    return UINT32_MAX;
}

} // namespace

// ============================================================
// Закреплённый свет
// ============================================================
//
// Снимок не должен зависеть от того, когда игрок его сделал. Поэтому
// здесь не «текущее состояние мира с выключенной погодой», а
// собственный набор чисел, одинаковый всегда:
//
//   * солнце стоит над юго-западом, довольно высоко — так освещены
//     две грани куба из трёх видимых, и рельеф читается объёмом;
//   * погоды нет вовсе: ни туч, ни осадков, ни радуги;
//   * точечных источников нет: факел в руке не должен менять карту;
//   * тумана нет — его дальняя граница унесена за любой разумный
//     размер области. Иначе ортография дала бы разную плотность
//     тумана в разных углах картинки, и одинаковые блоки перестали
//     бы выглядеть одинаково;
//   * камера формы отодвинута далеко назад вдоль взгляда. На
//     проекцию это не влияет (она ортографическая), но шейдер берёт
//     из cameraPos направление на зрителя для блика на воде — а при
//     ортографии оно обязано быть одним на весь кадр.
CameraUbo IsoSnapshot::fixedLightUbo(const iso::Camera& cam, iso::View view,
                                     f32 tintSeed) {
    CameraUbo u{};

    u.viewProj    = cam.viewProj();
    u.invViewProj = glm::inverse(u.viewProj);

    // Солнце: фиксированное направление, одно на все четыре вида.
    // Не привязано к стороне обзора намеренно — иначе четыре снимка
    // одного места освещались бы по-разному, и сравнить их было бы
    // нельзя.
    const glm::vec3 sun = glm::normalize(glm::vec3(-0.45f, 0.78f, -0.44f));
    u.sunDir = glm::vec4(sun, 1.0f);

    // Туман унесён за горизонт: на снимке его нет.
    u.fogParams = glm::vec4(1.0e6f, 2.0e6f, 0.35f, 0.f);

    const glm::vec3 sky{ 0.53f, 0.71f, 0.93f };
    u.skyColor  = glm::vec4(sky, 1.f);
    u.skyLinear = glm::vec4(sky * sky, 1.f);            // sRGB -> линейное
    u.sunLight  = glm::vec4(1.0f, 0.97f, 0.90f, 1.f);
    u.ambLight  = glm::vec4(0.62f, 0.70f, 0.85f, 0.42f);

    u.weather   = glm::vec4(0.f);                        // ни туч, ни осадков
    // Ветра нет, зерно крапчатости есть: см. объявление.
    u.wind      = glm::vec4(0.f, 0.f, 0.f, tintSeed);
    u.lightInfo = glm::vec4(0.f);                        // ни одного факела
    for (u32 i = 0; i < MAX_GPU_LIGHTS; ++i) {
        u.lightPos[i]   = glm::vec4(0.f);
        u.lightColor[i] = glm::vec4(0.f);
    }

    u.screenSize = glm::vec4((f32)cam.width, (f32)cam.height, 0.f, 0.f);

    // Зритель — бесконечно далеко вдоль взгляда.
    const glm::vec3 dir = iso::viewDirection(view);
    const glm::mat4 invV = glm::inverse(cam.view);
    const glm::vec3 eye  = glm::vec3(invV[3]);
    u.cameraPos = glm::vec4(eye - dir * 100000.f, 1.f);
    return u;
}

// ============================================================
// Жизненный цикл
// ============================================================

bool IsoSnapshot::init(VkDevice dev, VkPhysicalDevice phys, VkQueue queue,
                       u32 queueFamily, VkFormat depthFormat,
                       AAssetManager* assets)
{
    dev_ = dev; phys_ = phys; queue_ = queue; family_ = queueFamily;
    depthFormat_ = depthFormat;

    shaders_.init(dev, assets);

    if (!descriptors_.create(dev, 1)) {
        LOGE("снимок: дескрипторы не созданы");
        return false;
    }
    if (!ubo_.create(dev, phys, sizeof(CameraUbo), vk::BufferUsage::Uniform, true)) {
        LOGE("снимок: буфер формы не создан");
        return false;
    }
    descriptors_.bindUbo(0, ubo_.handle(), sizeof(CameraUbo));

    // Тот же проход рендера, что у игры и у офлайн-проверки. Разница
    // ровно одна: конечная раскладка цвета — источник передачи, потому
    // что картинку мы не показываем, а читаем обратно.
    if (!vk::createVoxelRenderPass(dev, COLOR_FMT, depthFormat,
                                   VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                   &renderPass_)) {
        LOGE("снимок: проход рендера не создан");
        return false;
    }

    {
        vk::PipelineDesc d = voxelPipelineDesc(renderPass_, descriptors_.layout(),
                                               depthFormat);
        if (!opaquePipe_.create(dev, shaders_, d)) {
            LOGE("снимок: конвейер ландшафта не создан");
            return false;
        }
        makeVoxelBlendDesc(d);
        if (!blendPipe_.create(dev, shaders_, d)) {
            LOGE("снимок: конвейер воды не создан");
            return false;
        }
    }
    // Растения — тем же описанием, что у FloraRenderer, на свой проход.
    // Без них снимок всё равно годен: голый ландшафт лучше, чем никакого.
    if (!floraPipe_.create(dev, shaders_,
                           floraPipelineDesc(renderPass_, descriptors_.layout(), depthFormat)))
        LOGW("снимок: конвейер растений не создан — снимки будут без них");

    VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pci.queueFamilyIndex = queueFamily;
    pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    if (vkCreateCommandPool(dev, &pci, nullptr, &cmdPool_) != VK_SUCCESS) {
        LOGE("снимок: пул команд не создан");
        return false;
    }
    VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cai.commandPool = cmdPool_;
    cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cai.commandBufferCount = 1;
    if (vkAllocateCommandBuffers(dev, &cai, &cmd_) != VK_SUCCESS) return false;

    VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    if (vkCreateFence(dev, &fi, nullptr, &fence_) != VK_SUCCESS) return false;

    LOGI("снимок: подсистема готова");
    return true;
}

void IsoSnapshot::destroy() {
    if (dev_ == VK_NULL_HANDLE) return;
    vkDeviceWaitIdle(dev_);
    releaseMeshes();
    destroyTarget();
    if (fence_)   vkDestroyFence(dev_, fence_, nullptr);
    if (cmdPool_) vkDestroyCommandPool(dev_, cmdPool_, nullptr);
    floraMeshes_.destroy();
    floraInst_.destroy();
    floraPipe_.destroy();
    floraReady_ = false;
    blendPipe_.destroy();
    opaquePipe_.destroy();
    if (renderPass_) vkDestroyRenderPass(dev_, renderPass_, nullptr);
    ubo_.destroy();
    descriptors_.destroy();
    shaders_.destroyAll();
    fence_ = VK_NULL_HANDLE;
    cmdPool_ = VK_NULL_HANDLE;
    renderPass_ = VK_NULL_HANDLE;
    dev_ = VK_NULL_HANDLE;
}

void IsoSnapshot::releaseMeshes() {
    for (auto& m : meshes_) {
        if (!m.valid) continue;
        m.vb.destroy();
        m.ib.destroy();
        m.valid = false;
    }
    meshes_.clear();
    meshCursor_ = 0;
}

void IsoSnapshot::destroyTarget() {
    if (readMapped_) { vkUnmapMemory(dev_, readMem_); readMapped_ = nullptr; }
    if (readBuf_)  vkDestroyBuffer(dev_, readBuf_, nullptr);
    if (readMem_)  vkFreeMemory(dev_, readMem_, nullptr);
    if (fb_)        vkDestroyFramebuffer(dev_, fb_, nullptr);
    if (colorView_) vkDestroyImageView(dev_, colorView_, nullptr);
    if (depthView_) vkDestroyImageView(dev_, depthView_, nullptr);
    if (colorImg_)  vkDestroyImage(dev_, colorImg_, nullptr);
    if (depthImg_)  vkDestroyImage(dev_, depthImg_, nullptr);
    if (colorMem_)  vkFreeMemory(dev_, colorMem_, nullptr);
    if (depthMem_)  vkFreeMemory(dev_, depthMem_, nullptr);
    readBuf_ = VK_NULL_HANDLE; readMem_ = VK_NULL_HANDLE;
    fb_ = VK_NULL_HANDLE;
    colorView_ = depthView_ = VK_NULL_HANDLE;
    colorImg_ = depthImg_ = VK_NULL_HANDLE;
    colorMem_ = depthMem_ = VK_NULL_HANDLE;
    targetW_ = targetH_ = 0;
}

bool IsoSnapshot::ensureTarget(u32 w, u32 h) {
    if (targetW_ == w && targetH_ == h && fb_ != VK_NULL_HANDLE) return true;
    destroyTarget();

    auto makeImage = [&](VkFormat fmt, VkImageUsageFlags usage,
                         VkImageAspectFlags aspect, VkImage& img,
                         VkDeviceMemory& mem, VkImageView& view) -> bool {
        VkImageCreateInfo ii{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        ii.imageType = VK_IMAGE_TYPE_2D; ii.format = fmt;
        ii.extent = { w, h, 1 };
        ii.mipLevels = 1; ii.arrayLayers = 1;
        ii.samples = VK_SAMPLE_COUNT_1_BIT;
        ii.tiling = VK_IMAGE_TILING_OPTIMAL;
        ii.usage = usage; ii.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        if (vkCreateImage(dev_, &ii, nullptr, &img) != VK_SUCCESS) return false;
        VkMemoryRequirements req; vkGetImageMemoryRequirements(dev_, img, &req);
        const u32 type = memType(phys_, req.memoryTypeBits,
                                 VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (type == UINT32_MAX) return false;
        VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        ai.allocationSize = req.size; ai.memoryTypeIndex = type;
        if (vkAllocateMemory(dev_, &ai, nullptr, &mem) != VK_SUCCESS) return false;
        vkBindImageMemory(dev_, img, mem, 0);
        VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vi.image = img; vi.viewType = VK_IMAGE_VIEW_TYPE_2D; vi.format = fmt;
        vi.subresourceRange.aspectMask = aspect;
        vi.subresourceRange.levelCount = 1; vi.subresourceRange.layerCount = 1;
        return vkCreateImageView(dev_, &vi, nullptr, &view) == VK_SUCCESS;
    };

    if (!makeImage(COLOR_FMT,
                   VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                   VK_IMAGE_ASPECT_COLOR_BIT, colorImg_, colorMem_, colorView_) ||
        !makeImage(depthFormat_, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
                   VK_IMAGE_ASPECT_DEPTH_BIT, depthImg_, depthMem_, depthView_)) {
        LOGE("снимок: цель %ux%u не создана", w, h);
        destroyTarget();
        return false;
    }

    VkImageView atts[2] = { colorView_, depthView_ };
    VkFramebufferCreateInfo fbi{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
    fbi.renderPass = renderPass_; fbi.attachmentCount = 2; fbi.pAttachments = atts;
    fbi.width = w; fbi.height = h; fbi.layers = 1;
    if (vkCreateFramebuffer(dev_, &fbi, nullptr, &fb_) != VK_SUCCESS) {
        destroyTarget();
        return false;
    }

    const VkDeviceSize bytes = (VkDeviceSize)w * h * 4;
    VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bci.size = bytes; bci.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vkCreateBuffer(dev_, &bci, nullptr, &readBuf_) != VK_SUCCESS) {
        destroyTarget();
        return false;
    }
    VkMemoryRequirements br; vkGetBufferMemoryRequirements(dev_, readBuf_, &br);
    const u32 rt = memType(phys_, br.memoryTypeBits,
                           VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                           VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (rt == UINT32_MAX) { destroyTarget(); return false; }
    VkMemoryAllocateInfo bai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    bai.allocationSize = br.size; bai.memoryTypeIndex = rt;
    if (vkAllocateMemory(dev_, &bai, nullptr, &readMem_) != VK_SUCCESS) {
        destroyTarget();
        return false;
    }
    vkBindBufferMemory(dev_, readBuf_, readMem_, 0);
    if (vkMapMemory(dev_, readMem_, 0, bytes, 0, &readMapped_) != VK_SUCCESS) {
        readMapped_ = nullptr;
        destroyTarget();
        return false;
    }

    targetW_ = w; targetH_ = h;
    return true;
}

// ============================================================
// Начало и ход
// ============================================================

void IsoSnapshot::begin(const IsoRequest& req) {
    cancel();
    req_ = req;
    req_.area.size = std::clamp(req_.area.size, MIN_SIZE, MAX_SIZE);

    // Чанки территории плюс рамка в один чанк: без соседей мешер не
    // знает, что за границей, и на стыке вырастает лишняя стена.
    const iso::ChunkRange draw = iso::chunkRange(req_.area, world::CHUNK_SIZE, 0);
    const iso::ChunkRange need = iso::chunkRange(req_.area, world::CHUNK_SIZE, 1);

    drawChunks_.clear();
    needChunks_.clear();
    for (i32 cz = draw.z0; cz <= draw.z1; ++cz)
        for (i32 cx = draw.x0; cx <= draw.x1; ++cx)
            drawChunks_.push_back({ cx, cz });
    for (i32 cz = need.z0; cz <= need.z1; ++cz)
        for (i32 cx = need.x0; cx <= need.x1; ++cx)
            needChunks_.push_back({ cx, cz });

    meshes_.assign(drawChunks_.size(), ChunkMeshGpu{});
    meshCursor_ = 0;
    prepareTicks_ = 0;
    floraCount_ = 0;
    floraTop_ = 0.f;
    stage_ = IsoStage::Preparing;
    error_ = IsoError::None;
    pixels_.clear();
    outW_ = outH_ = 0;
}

void IsoSnapshot::cancel() {
    if (dev_ != VK_NULL_HANDLE && stage_ == IsoStage::Rendering) vkDeviceWaitIdle(dev_);
    releaseMeshes();
    needChunks_.clear();
    drawChunks_.clear();
    tiles_.clear();
    tileCursor_ = 0;
    pixels_.clear();
    outW_ = outH_ = 0;
    stage_ = IsoStage::Idle;
    error_ = IsoError::None;
}

f32 IsoSnapshot::progress() const {
    switch (stage_) {
        case IsoStage::Idle:      return 0.f;
        case IsoStage::Preparing: return 0.05f;
        case IsoStage::Meshing:
            return meshes_.empty() ? 0.1f
                 : 0.1f + 0.5f * (f32)meshCursor_ / (f32)meshes_.size();
        case IsoStage::Rendering:
            return tiles_.empty() ? 0.6f
                 : 0.6f + 0.4f * (f32)tileCursor_ / (f32)tiles_.size();
        case IsoStage::Ready:     return 1.f;
        case IsoStage::Failed:    return 0.f;
    }
    return 0.f;
}

void IsoSnapshot::scanHeights(world::ChunkManager& world) {
    world::VoxelReader rd(world);

    // Границы кадра по высоте считаются по ПОВЕРХНОСТИ, а не по
    // всему столбу вокселей.
    //
    // Соблазнительно взять «от самого нижнего непустого блока до
    // самого верхнего» — и получить от нуля до вершины горы, потому
    // что на нуле лежит коренная порода, и лежит она под КАЖДОЙ
    // колонкой. Замер на живом мире: высоты 0..109 при рельефе
    // 60..80, картинка 725x1131, и девять десятых её — камень,
    // которого никто никогда не увидит.
    //
    // Видно только поверхность, поэтому по ней и меряем: у каждой
    // колонки берём её верхний непустой блок (для моря это гладь
    // воды — она не воздух), а по области — размах этих высот.
    i32 lo = world::CHUNK_SIZE_Y, hi = -1;
    for (i32 z = req_.area.z0; z < req_.area.z1(); ++z) {
        for (i32 x = req_.area.x0; x < req_.area.x1(); ++x) {
            for (i32 y = world::CHUNK_SIZE_Y - 1; y >= 0; --y) {
                if (rd.at(x, y, z) == world::AIR) continue;
                if (y > hi) hi = y;
                if (y < lo) lo = y;
                break;
            }
        }
    }
    if (hi < 0) { lo = 0; hi = 1; }          // пустая область: хоть что-то

    // Запас снизу — не украшение. Камера смотрит сверху под углом, и
    // у ближнего края слоя видна его боковая стенка; срез ровно по
    // самой низкой поверхности обрубил бы её по линии.
    yMin_ = std::max(0, lo - SKIRT_BELOW);
    // Сверху хватает пары блоков: выше самой высокой точки ничего нет.
    yMax_ = std::min(world::CHUNK_SIZE_Y, hi + 2);
}

void IsoSnapshot::computeTiles() {
    tiles_.clear();
    tileCursor_ = 0;
    for (u32 y = 0; y < outH_; y += TILE) {
        for (u32 x = 0; x < outW_; x += TILE) {
            Tile t;
            t.x = x; t.y = y;
            t.w = std::min(TILE, outW_ - x);
            t.h = std::min(TILE, outH_ - y);
            tiles_.push_back(t);
        }
    }
}


// ============================================================
// Срез по краю области
// ============================================================
//
// Территория — квадрат, и по его краю мир обрезан. Мешер грань на
// этом месте не строит, и правильно делает: с той стороны стоит
// такой же камень, а между двумя камнями грани нет. Но соседний чанк
// мы НЕ РИСУЕМ, и на снимке вместо среза получалась дыра: сквозь
// землю видно фон. На виде с юга это выглядело как чёрный провал под
// всем рельефом.
//
// Поэтому срез строится явно — и строится из НАСТОЯЩИХ блоков:
// каждый воксель на кромке отдаёт свою грань со своим цветом. Это не
// «условная раскраска карты», а поперечный разрез той же земли,
// собранный тем же `buildChunkVertices` из тех же `world::Quad`.
//
// Оси квада берутся из той же таблицы FACES, что и в мешере:
//   +X/-X: u вдоль Y, v вдоль Z
//   +Z/-Z: u вдоль X, v вдоль Y
// ============================================================
// Меш — только в коробке снимка
// ============================================================
//
// Область не обязана быть выровнена по чанкам: сто блоков вокруг
// игрока — это четыре чанка в ширину, и из крайних в кадр попадает
// только часть. Чанк же мешируется целиком, и остаток за кромкой
// ложился поверх среза: на ближних сторонах — полоса земли перед
// разрезом, на дальних — ландшафт до самых краёв картинки. Снизу то
// же: под дном коробки (yMin) оставались пещеры и лавовые озёра и
// висели под срезом обрывками. С растениями, которые рисуются строго
// в области, это стало видно сразу: за кромкой — голая земля.
//
// Поэтому грани обрезаются по коробке. Грань — прямоугольник вдоль
// двух осей (FACES мешера: du и dv положительны, по одной оси каждый),
// и обрезать её — сузить диапазон клеток по двум осям. Затенение и
// открытость неба в углах слитой грани одинаковы у всех её клеток
// (с разными мешер не сливает), поэтому часть грани светится ровно
// так же, как целая. Внутри коробки не меняется ни одна грань.
void IsoSnapshot::clipQuadsToBox(const iso::Area& area, i32 yMin,
                                 world::ChunkCoord coord,
                                 std::vector<world::Quad>& quads)
{
    const i32 ox = coord.x * world::CHUNK_SIZE;
    const i32 oz = coord.z * world::CHUNK_SIZE;
    const f32 lo[3] = { (f32)(area.x0 - ox), (f32)yMin, (f32)(area.z0 - oz) };
    const f32 hi[3] = { (f32)(area.x1() - ox), 1e9f, (f32)(area.z1() - oz) };

    usize w = 0;
    for (usize i = 0; i < quads.size(); ++i) {
        world::Quad q = quads[i];
        const i32 normal = q.v0.face / 2;               // ось нормали: 0 — X, 1 — Y, 2 — Z
        const bool positive = (q.v0.face % 2) == 0;
        bool keep = true;
        for (i32 axis = 0; axis < 3 && keep; ++axis) {
            if (normal == axis) {
                // Грань поперёк оси: клетка — та, чья это грань.
                const f32 cell = positive ? q.v0.pos[axis] - 1.f : q.v0.pos[axis];
                if (cell < lo[axis] || cell + 1.f > hi[axis]) keep = false;
                continue;
            }
            glm::vec3& d = q.du[axis] != 0.f ? q.du : q.dv;
            const f32 a = std::max(q.v0.pos[axis], lo[axis]);
            const f32 b = std::min(q.v0.pos[axis] + d[axis], hi[axis]);
            if (a >= b) { keep = false; continue; }
            q.v0.pos[axis] = a;
            d[axis] = b - a;
        }
        if (keep) quads[w++] = q;
    }
    quads.resize(w);
}

void IsoSnapshot::appendAreaWalls(const world::Chunk& chunk,
                                  world::ChunkCoord coord,
                                  std::vector<world::Quad>& quads) const
{
    const i32 ox = coord.x * world::CHUNK_SIZE;
    const i32 oz = coord.z * world::CHUNK_SIZE;

    // Срез искусственный: затенения углов на нём нет, а небо берём
    // тусклое — это разрез породы, а не освещённый склон.
    constexpr u8 WALL_SKY = 3;
    constexpr u8 WALL_AO  = 3;

    // Ствол дерева в сетке мира — невидимый блок: его рисует модель
    // растения. С растениями на снимке срез по нему дал бы бурый
    // квадрат поверх ствола модели — или на месте дерева, которое за
    // кромку не поместилось и не рисуется.
    const bool skipInvisible = natureOn();
    auto hidden = [&](u16 b) {
        return b == world::AIR || (skipInvisible && world::blocks().get(b).isInvisible);
    };
    auto emit = [&](i32 lx, i32 ly, i32 lz, u8 face) {
        const u16 b = chunk.at(lx, ly, lz);
        if (hidden(b)) return;
        world::Quad q{};
        q.v0.block = b;
        q.v0.face  = face;
        q.v0.pos   = glm::vec3((f32)lx, (f32)ly, (f32)lz);
        switch (face) {
            case 0: q.v0.pos.x += 1.f; q.du = {0,1,0}; q.dv = {0,0,1}; break; // +X
            case 1:                    q.du = {0,1,0}; q.dv = {0,0,1}; break; // -X
            case 4: q.v0.pos.z += 1.f; q.du = {1,0,0}; q.dv = {0,1,0}; break; // +Z
            case 5:                    q.du = {1,0,0}; q.dv = {0,1,0}; break; // -Z
            default: return;
        }
        for (int i = 0; i < 4; ++i) { q.sky[i] = WALL_SKY; q.ao[i] = WALL_AO; }
        quads.push_back(q);
    };

    const i32 y0 = std::max(0, yMin_);
    const i32 y1 = std::min(world::CHUNK_SIZE_Y, yMax_);

    // Западная и восточная кромки.
    for (const auto& [wx, face] : { std::pair<i32, u8>{ req_.area.x0, (u8)1 },
                                    std::pair<i32, u8>{ req_.area.x1() - 1, (u8)0 } }) {
        const i32 lx = wx - ox;
        if (lx < 0 || lx >= world::CHUNK_SIZE) continue;
        for (i32 wz = req_.area.z0; wz < req_.area.z1(); ++wz) {
            const i32 lz = wz - oz;
            if (lz < 0 || lz >= world::CHUNK_SIZE) continue;
            for (i32 y = y0; y < y1; ++y) emit(lx, y, lz, face);
        }
    }
    // Северная и южная кромки.
    for (const auto& [wz, face] : { std::pair<i32, u8>{ req_.area.z0, (u8)5 },
                                    std::pair<i32, u8>{ req_.area.z1() - 1, (u8)4 } }) {
        const i32 lz = wz - oz;
        if (lz < 0 || lz >= world::CHUNK_SIZE) continue;
        for (i32 wx = req_.area.x0; wx < req_.area.x1(); ++wx) {
            const i32 lx = wx - ox;
            if (lx < 0 || lx >= world::CHUNK_SIZE) continue;
            for (i32 y = y0; y < y1; ++y) emit(lx, y, lz, face);
        }
    }

    // И дно: снизу область тоже обрезана.
    if (y0 > 0) {
        for (i32 wz = req_.area.z0; wz < req_.area.z1(); ++wz) {
            const i32 lz = wz - oz;
            if (lz < 0 || lz >= world::CHUNK_SIZE) continue;
            for (i32 wx = req_.area.x0; wx < req_.area.x1(); ++wx) {
                const i32 lx = wx - ox;
                if (lx < 0 || lx >= world::CHUNK_SIZE) continue;
                const u16 b = chunk.at(lx, y0, lz);
                if (hidden(b)) continue;
                world::Quad q{};
                q.v0.block = b;
                q.v0.face  = 3;                       // -Y
                q.v0.pos   = glm::vec3((f32)lx, (f32)y0, (f32)lz);
                q.du = {1,0,0}; q.dv = {0,0,1};       // у -Y: u вдоль X, v вдоль Z
                for (int i = 0; i < 4; ++i) { q.sky[i] = WALL_SKY; q.ao[i] = WALL_AO; }
                quads.push_back(q);
            }
        }
    }
}

bool IsoSnapshot::buildOneMesh(world::ChunkManager& world, usize index) {
    const world::ChunkCoord c = drawChunks_[index];
    auto self = world.findChunk(c.x, c.z);
    if (!self || !self->generated.load(std::memory_order_acquire)) return false;

    auto nx = world.findChunk(c.x - 1, c.z);
    auto px = world.findChunk(c.x + 1, c.z);
    auto nz = world.findChunk(c.x, c.z - 1);
    auto pz = world.findChunk(c.x, c.z + 1);

    // Согласованное чтение.
    //
    // Те же замки и в том же порядке, что берёт задача меширования
    // (`ChunkManager::jobMesh`): общий разделяемый замок на свой чанк
    // и на всех соседей, захват по возрастанию адреса. Порядок — не
    // осторожность, а обязательство: две задачи на соседних чанках
    // иначе встают в клинч.
    //
    // Пока замки держатся, воксели этих чанков не меняются, и грань
    // на стыке строится по одному и тому же состоянию с обеих
    // сторон — ровно то, что требуется от снимка.
    {
        std::array<const world::Chunk*, 5> locked{
            self.get(), nx.get(), px.get(), nz.get(), pz.get() };
        std::sort(locked.begin(), locked.end());
        std::array<std::shared_lock<std::shared_mutex>, 5> guards;
        for (usize i = 0; i < locked.size(); ++i) {
            if (!locked[i]) continue;
            if (i > 0 && locked[i] == locked[i - 1]) continue;
            guards[i] = std::shared_lock<std::shared_mutex>(locked[i]->voxelMutex);
        }

        world::ChunkNeighbors nb;
        nb.nx = nx && nx->generated.load(std::memory_order_acquire) ? nx.get() : nullptr;
        nb.px = px && px->generated.load(std::memory_order_acquire) ? px.get() : nullptr;
        nb.nz = nz && nz->generated.load(std::memory_order_acquire) ? nz.get() : nullptr;
        nb.pz = pz && pz->generated.load(std::memory_order_acquire) ? pz.get() : nullptr;

        world::buildGreedyMesh(*self, nb, scratchQuads_);
        clipQuadsToBox(req_.area, yMin_, c, scratchQuads_);
        appendAreaWalls(*self, c, scratchQuads_);
        meshes_[index].origin = glm::vec3((f32)(c.x * world::CHUNK_SIZE), 0.f,
                                          (f32)(c.z * world::CHUNK_SIZE));
        if (natureOn()) collectFlora(*self, meshes_[index]);
        buildChunkVertices(*self, scratchQuads_, scratchVerts_, scratchIdx_,
                           meshes_[index].opaqueIndices,
                           &meshes_[index].blendCenter);
    }

    ChunkMeshGpu& m = meshes_[index];
    m.origin = glm::vec3((f32)(c.x * world::CHUNK_SIZE), 0.f,
                         (f32)(c.z * world::CHUNK_SIZE));
    m.totalIndices = (u32)scratchIdx_.size();
    if (m.totalIndices == 0) { m.valid = false; return true; }

    const bool narrow = scratchVerts_.size() <= 65535;
    m.indexType = narrow ? VK_INDEX_TYPE_UINT16 : VK_INDEX_TYPE_UINT32;

    const void* idxSrc = scratchIdx_.data();
    u64 idxBytes = scratchIdx_.size() * sizeof(u32);
    if (narrow) {
        scratchIdx16_.resize(scratchIdx_.size());
        for (usize i = 0; i < scratchIdx_.size(); ++i)
            scratchIdx16_[i] = (u16)scratchIdx_[i];
        idxSrc = scratchIdx16_.data();
        idxBytes = scratchIdx16_.size() * sizeof(u16);
    }
    const u64 vbBytes = scratchVerts_.size() * sizeof(VoxelVertex);

    // Буферы, видимые процессору: снимок строится один раз, и гонять
    // ради него staging-пул незачем.
    if (!m.vb.create(dev_, phys_, vbBytes, vk::BufferUsage::Vertex, true) ||
        !m.ib.create(dev_, phys_, idxBytes, vk::BufferUsage::Index, true)) {
        LOGE("снимок: не хватило памяти на меш чанка %d,%d", c.x, c.z);
        error_ = IsoError::OutOfMemory;
        return false;
    }
    if (void* p = m.vb.map()) std::memcpy(p, scratchVerts_.data(), vbBytes);
    if (void* p = m.ib.map()) std::memcpy(p, idxSrc, idxBytes);
    m.valid = true;
    return true;
}

bool IsoSnapshot::step(world::ChunkManager& world) {
    // Зерно мира — отсюда: снимок обязан красить блоки ровно так же,
    // как игровой кадр, а крапчатость считается из зерна. Мир здесь
    // первый и единственный раз оказывается под рукой.
    tintSeed_ = tintSeedOf(world.seed());
    switch (stage_) {
        case IsoStage::Idle:
        case IsoStage::Ready:
        case IsoStage::Failed:
            return false;

        case IsoStage::Preparing: {
            // Настоящая генерация: getChunk заводит чанк, если его
            // нет, и ставит задачу — ту же самую, что и потоковая
            // загрузка. Никакого отдельного генератора для снимков.
            usize ready = 0;
            for (const auto& c : needChunks_) {
                auto ch = world.getChunk(c.x, c.z);
                if (ch && ch->generated.load(std::memory_order_acquire)) ++ready;
            }
            if (ready == needChunks_.size()) {
                scanHeights(world);
                stage_ = IsoStage::Meshing;
                return true;
            }
            if (++prepareTicks_ > PREPARE_MAX_TICKS) {
                LOGE("снимок: мир не выдал чанки (%zu из %zu)",
                     ready, needChunks_.size());
                error_ = IsoError::Generation;
                stage_ = IsoStage::Failed;
            }
            return true;
        }

        case IsoStage::Meshing: {
            // Модели растений — отдельным шагом: это сотня с лишним
            // мешей, и в один шаг с чанками они растянули бы кадр.
            if (req_.nature && !floraReady_ && floraPipe_.valid()) {
                ensureFloraModels();
                return true;
            }
            for (usize n = 0; n < MESHES_PER_STEP && meshCursor_ < meshes_.size(); ++n) {
                if (!buildOneMesh(world, meshCursor_)) {
                    if (error_ != IsoError::None) { stage_ = IsoStage::Failed; return true; }
                    return true;            // чанк ещё не готов — подождём
                }
                ++meshCursor_;
            }
            if (meshCursor_ >= meshes_.size()) {
                // Верх кадра — по растительности тоже: высокое дерево на
                // вершине иначе осталось бы без макушки.
                if (floraCount_ > 0)
                    yMax_ = std::max(yMax_, (i32)std::ceil(floraTop_) + 1);
                const iso::Box box{ req_.area, (f32)yMin_, (f32)yMax_ };
                cam_ = iso::makeCamera(box, req_.view, req_.pixelsPerBlock,
                                       req_.maxSide);
                outW_ = cam_.width; outH_ = cam_.height;
                pixels_.assign((usize)outW_ * outH_ * 3, 0);
                computeTiles();
                stage_ = IsoStage::Rendering;
            }
            return true;
        }

        case IsoStage::Rendering: {
            if (tileCursor_ >= tiles_.size()) { stage_ = IsoStage::Ready; return true; }
            if (!renderTile(tiles_[tileCursor_])) {
                error_ = IsoError::RenderTarget;
                stage_ = IsoStage::Failed;
                return true;
            }
            if (++tileCursor_ >= tiles_.size()) stage_ = IsoStage::Ready;
            return true;
        }
    }
    return false;
}

bool IsoSnapshot::rerender(const IsoRequest& req) {
    if (meshes_.empty() || meshCursor_ < meshes_.size()) return false;
    // Область та же — иначе пришлось бы пересобирать меши.
    if (req.area.x0 != req_.area.x0 || req.area.z0 != req_.area.z0 ||
        req.area.size != req_.area.size || req.nature != req_.nature) return false;

    req_ = req;
    const iso::Box box{ req_.area, (f32)yMin_, (f32)yMax_ };
    cam_ = iso::makeCamera(box, req_.view, req_.pixelsPerBlock, req_.maxSide);
    outW_ = cam_.width; outH_ = cam_.height;
    pixels_.assign((usize)outW_ * outH_ * 3, 0);
    computeTiles();
    stage_ = IsoStage::Rendering;
    error_ = IsoError::None;
    return true;
}

// ============================================================
// Растения
// ============================================================

bool IsoSnapshot::ensureFloraModels() {
    if (floraReady_) return true;
    const std::vector<VoxelMesh> meshes = buildFloraMeshes();
    floraQuads_.resize(meshes.size());
    for (usize i = 0; i < meshes.size(); ++i) floraQuads_[i] = meshes[i].quadCount();
    floraExtents_ = floraExtents(meshes);
    if (!floraMeshes_.setHostVisible(dev_, phys_, meshes)) {
        // Не вышло — снимок будет без растений, но будет.
        LOGW("снимок: модели растений не загружены");
        floraPipe_.destroy();
        return false;
    }
    floraReady_ = true;
    LOGI("снимок: моделей растений %u, мешей %zu", floraModelCount(), meshes.size());
    return true;
}

// Растения чанка — те же, что увидит игра: отбор по вокселям тем же
// floraStillStands, что и в задаче меширования чанка. Срубленное
// дерево, вскопанная трава и цветок под водой отсеиваются здесь.
//
// Своё правило одно: растение целиком внутри области. Кадр кроится по
// коробке области (iso::makeCamera), и крона, свешенная за кромку,
// была бы обрезана краем картинки; растение соседнего чанка за
// кромкой висело бы над срезом. Габарит — коробка модели на размер
// экземпляра плюс наибольший наклон макушки на ветру (flora.vert:
// направление 0.036, порыв до 1.25, растёт с квадратом высоты).
void IsoSnapshot::collectFlora(const world::Chunk& chunk, ChunkMeshGpu& m) {
    m.flora.clear();
    auto voxel = [&](i32 x, i32 y, i32 z) -> u16 {
        return (u32)y < (u32)world::CHUNK_SIZE_Y ? chunk.voxels[world::chunkIndex(x, y, z)]
                                                 : world::AIR;
    };
    const f32 x0 = (f32)req_.area.x0, x1 = (f32)req_.area.x1();
    const f32 z0 = (f32)req_.area.z0, z1 = (f32)req_.area.z1();
    constexpr f32 LEAN = 0.036f * 1.25f;
    glm::vec3 lo(1e30f), hi(-1e30f);
    for (const world::FloraInstance& f : chunk.flora) {
        if (!world::floraStillStands(f, voxel)) continue;
        const u32 model = floraModelIndex(f.kind, f.variant);
        if (model >= floraExtents_.size()) continue;
        const FloraExtent& e = floraExtents_[model];
        const f32 scale = std::min(world::floraScale(f), FLORA_MAX_SCALE);
        const f32 h = e.height * scale;
        const f32 r = e.radius * scale + LEAN * floraSway(f.kind) * h * h;
        const glm::vec3 b = FloraBatch::basePosition(m.origin, f);
        if (b.x - r < x0 || b.x + r > x1 || b.z - r < z0 || b.z + r > z1) continue;
        m.flora.push_back(f);
        lo = glm::min(lo, glm::vec3(b.x - r, b.y, b.z - r));
        hi = glm::max(hi, glm::vec3(b.x + r, b.y + h, b.z + r));
        floraTop_ = std::max(floraTop_, b.y + h);
    }
    m.floraLo = lo;
    m.floraHi = hi;
    floraCount_ += (u32)m.flora.size();
}

// Растения тайла: те чанки, чья коробка растений в тайл попадает. Не
// по коробке чанка: крона дерева у края чанка свешивается в соседний,
// и тайл, в который попадает только она, отбросил бы её вместе с
// чанком — на стыке тайлов крону срезало бы.
//
// Ступень — по размеру вокселя в пикселях, а не по расстоянию:
// FloraBatch::beginOrtho. Камеры из fixedLightUbo здесь нет вовсе —
// она унесена на сто тысяч блоков ради блика воды, и любое расстояние
// от неё отдало бы всё самой грубой ступени.
void IsoSnapshot::drawFlora(const math::Frustum& fr) {
    floraBatch_.beginOrtho(cam_.pixelsPerUnit);
    for (const ChunkMeshGpu& m : meshes_) {
        if (m.flora.empty()) continue;
        math::AABB bb; bb.expand(m.floraLo); bb.expand(m.floraHi);
        if (!fr.intersectsAABB(bb)) continue;
        floraBatch_.addChunk(m.origin, m.flora);
    }
    floraBatch_.finish(&floraQuads_);
    const auto& inst = floraBatch_.instances();
    if (inst.empty() || floraBatch_.draws().empty()) return;

    const u64 bytes = (u64)inst.size() * sizeof(FloraGpuInstance);
    if (!floraInst_.handle() || floraInst_.size() < bytes) {
        floraInst_.destroy();
        if (!floraInst_.create(dev_, phys_, bytes, vk::BufferUsage::Vertex, true)) {
            LOGE("снимок: буфер растений не создан");
            return;
        }
    }
    // Тайлы идут строго по очереди и каждый дожидается своей заборки,
    // поэтому один буфер на все тайлы переписывать можно.
    floraInst_.write(inst.data(), bytes);

    VkDescriptorSet ds = descriptors_.set(0);
    vkCmdBindPipeline(cmd_, VK_PIPELINE_BIND_POINT_GRAPHICS, floraPipe_.handle());
    vkCmdBindDescriptorSets(cmd_, VK_PIPELINE_BIND_POINT_GRAPHICS, floraPipe_.layout(),
                            0, 1, &ds, 0, nullptr);
    floraMeshes_.bind(cmd_, floraInst_.handle());
    for (const FloraBatch::Draw& d : floraBatch_.draws())
        floraMeshes_.draw(cmd_, d.mesh, d.count, d.first);
}

// ============================================================
// Рисование одного тайла
// ============================================================
//
// Тайл — это не другая камера, а ОКНО той же самой. Полный кадр в
// координатах отсечения занимает квадрат [-1,1]; тайлу соответствует
// его подпрямоугольник, и перевести один в другой можно линейно.
// Матрица окна домножается СЛЕВА к viewProj, поэтому проекция
// остаётся ровно той же ортографической: масштаб, углы осей и
// параллельность от разбиения на тайлы не меняются вовсе.
bool IsoSnapshot::renderTile(const Tile& t) {
    if (!ensureTarget(t.w, t.h)) return false;

    // Окно в координатах отсечения.
    const f32 ax = (f32)outW_ / (f32)t.w;
    const f32 ay = (f32)outH_ / (f32)t.h;
    const f32 bx = -1.f - 2.f * (f32)t.x / (f32)t.w + ax;
    const f32 by = -1.f - 2.f * (f32)t.y / (f32)t.h + ay;

    glm::mat4 win(1.f);
    win[0][0] = ax; win[1][1] = ay;
    win[3][0] = bx; win[3][1] = by;

    CameraUbo u = fixedLightUbo(cam_, req_.view, tintSeed_);
    u.viewProj    = win * cam_.viewProj();
    u.invViewProj = glm::inverse(u.viewProj);
    u.screenSize  = glm::vec4((f32)t.w, (f32)t.h, 0.f, 0.f);
    if (void* p = ubo_.map()) std::memcpy(p, &u, sizeof(u));

    const math::Frustum fr = math::Frustum::fromViewProj(u.viewProj);
    const glm::vec3 eye = glm::vec3(u.cameraPos);

    // Порядок тот же, что в игре: непрозрачное от ближнего к
    // дальнему (ранний тест глубины), вода — от дальнего к ближнему
    // (иначе смешивание не складывается).
    struct Item { const ChunkMeshGpu* m; f32 d; };
    std::vector<Item> opaque, blended;
    opaque.reserve(meshes_.size());
    for (const auto& m : meshes_) {
        if (!m.valid || m.totalIndices == 0) continue;
        const glm::vec3 lo = m.origin;
        const glm::vec3 hi = lo + glm::vec3((f32)world::CHUNK_SIZE,
                                            (f32)world::CHUNK_SIZE_Y,
                                            (f32)world::CHUNK_SIZE);
        math::AABB bb; bb.expand(lo); bb.expand(hi);
        if (!fr.intersectsAABB(bb)) continue;
        const glm::vec3 c = (lo + hi) * 0.5f;
        const f32 d = glm::length(c - eye);
        if (m.opaqueIndices > 0) opaque.push_back({ &m, d });
        if (m.totalIndices > m.opaqueIndices)
            blended.push_back({ &m, glm::length(lo + m.blendCenter - eye) });
    }
    std::sort(opaque.begin(), opaque.end(),
              [](const Item& a, const Item& b) { return a.d < b.d; });
    std::sort(blended.begin(), blended.end(),
              [](const Item& a, const Item& b) { return a.d > b.d; });

    vkResetCommandBuffer(cmd_, 0);
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (vkBeginCommandBuffer(cmd_, &bi) != VK_SUCCESS) return false;

    VkClearValue clears[2];
    // Фон — не небо: за границей территории ничего нет, и показывать
    // там небо значило бы обещать мир, которого в снимке нет.
    clears[0].color = { { 0.06f, 0.07f, 0.09f, 1.f } };
    clears[1].depthStencil = { 1.f, 0 };

    VkRenderPassBeginInfo rbi{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    rbi.renderPass = renderPass_;
    rbi.framebuffer = fb_;
    rbi.renderArea.extent = { t.w, t.h };
    rbi.clearValueCount = 2;
    rbi.pClearValues = clears;
    vkCmdBeginRenderPass(cmd_, &rbi, VK_SUBPASS_CONTENTS_INLINE);

    VkViewport vp{};
    vp.width = (f32)t.w; vp.height = (f32)t.h;
    vp.minDepth = 0.f; vp.maxDepth = 1.f;
    vkCmdSetViewport(cmd_, 0, 1, &vp);
    VkRect2D sc{}; sc.extent = { t.w, t.h };
    vkCmdSetScissor(cmd_, 0, 1, &sc);

    VkDescriptorSet ds = descriptors_.set(0);
    auto drawList = [&](const std::vector<Item>& list, VkPipeline pipe,
                        bool blendedPass) {
        if (list.empty()) return;
        vkCmdBindPipeline(cmd_, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
        vkCmdBindDescriptorSets(cmd_, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                opaquePipe_.layout(), 0, 1, &ds, 0, nullptr);
        for (const Item& it : list) {
            const ChunkMeshGpu& m = *it.m;
            const u32 first = blendedPass ? m.opaqueIndices : 0;
            const u32 count = blendedPass ? m.totalIndices - m.opaqueIndices
                                          : m.opaqueIndices;
            if (count == 0) continue;
            const ChunkPush push{ glm::vec4(m.origin, 0.f) };
            vkCmdPushConstants(cmd_, opaquePipe_.layout(),
                               VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(push), &push);
            const VkBuffer vb = m.vb.handle();
            VkDeviceSize off[] = { 0 };
            vkCmdBindVertexBuffers(cmd_, 0, 1, &vb, off);
            vkCmdBindIndexBuffer(cmd_, m.ib.handle(), 0, m.indexType);
            vkCmdDrawIndexed(cmd_, count, 1, first, 0, 0);
        }
    };
    // Порядок игрового кадра: ландшафт, растения, вода.
    drawList(opaque,  opaquePipe_.handle(), false);
    if (natureOn()) drawFlora(fr);
    drawList(blended, blendPipe_.handle(),  true);

    vkCmdEndRenderPass(cmd_);

    VkBufferImageCopy region{};
    region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.imageSubresource.layerCount = 1;
    region.imageExtent = { t.w, t.h, 1 };
    vkCmdCopyImageToBuffer(cmd_, colorImg_, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           readBuf_, 1, &region);
    if (vkEndCommandBuffer(cmd_) != VK_SUCCESS) return false;

    vkResetFences(dev_, 1, &fence_);
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = 1; si.pCommandBuffers = &cmd_;
    if (vkQueueSubmit(queue_, 1, &si, fence_) != VK_SUCCESS) return false;
    if (vkWaitForFences(dev_, 1, &fence_, VK_TRUE, UINT64_MAX) != VK_SUCCESS) return false;

    // RGBA из цели -> RGB в итоговую картинку, на своё место.
    const u8* src = (const u8*)readMapped_;
    for (u32 y = 0; y < t.h; ++y) {
        const u8* s = src + (usize)y * t.w * 4;
        u8* d = pixels_.data() + ((usize)(t.y + y) * outW_ + t.x) * 3;
        for (u32 x = 0; x < t.w; ++x) {
            d[x * 3 + 0] = s[x * 4 + 0];
            d[x * 3 + 1] = s[x * 4 + 1];
            d[x * 3 + 2] = s[x * 4 + 2];
        }
    }
    return true;
}

} // namespace render
