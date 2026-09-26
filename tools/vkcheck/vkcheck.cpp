/**
 * @file vkcheck.cpp
 * @brief Настоящий Vulkan на хосте: те же шейдеры и тот же конвейер,
 *        что в APK, только без устройства и без экрана.
 *
 * Зачем отдельный инструмент. tools/preview — программный растеризатор:
 * он проверяет мешер и ничего не знает ни про SPIR-V, ни про формат
 * вершины, ни про состояние конвейера. То есть ровно про ту половину
 * рендера, где ошибки и живут, он молчит. Здесь наоборот: берутся
 * настоящие .spv из assets, настоящий vk::GraphicsPipeline, настоящий
 * проход рендера и настоящая раскладка вершин, а кадр рисуется в
 * память через программную реализацию Vulkan (lavapipe).
 *
 * Слой проверки включён всегда: каждое его сообщение — провал.
 */
#include "vk/vk_context.h"
#include "vk/vk_buffer.h"
#include "vk/vk_pipeline.h"
#include "vk/vk_shader.h"
#include "vk/vk_descriptors.h"
#include "render/mesh_builder.h"
#include "render/camera.h"
#include "render/lights.h"
#include "render/voxel_pipeline.h"
#include "render/flora_models.h"
#include "render/flora_renderer.h"
#include "world/flora.h"
#include "world/day_cycle.h"
#include "world/debug_scene.h"
#include "world/chunk.h"
#include "world/block.h"
#include "world/terrain.h"
#include "world/features.h"

#include <cstdio>
#include <cstdarg>
#include <cstring>
#include <cmath>
#include <cstdlib>
#include <algorithm>
#include <map>
#include <memory>
#include <string>
#include <vector>

// ------------------------------------------------------------
// Ассеты: ShaderCache открывает их через AAssetManager. На хосте
// подменяем его чтением из каталога assets, чтобы путь загрузки
// шейдера был тот же самый, что в игре.
// ------------------------------------------------------------
struct AAsset { std::vector<char> data; };
static std::string g_assetRoot = "app/src/main/assets/";
static std::string g_debugRoot;   ///< шейдеры самого инструмента

extern "C" {
// Журнал Android на хосте: строки идут в stdout. Настоящие стволы
// hostcheck брать нельзя — там заглушен и весь Vulkan.
int __android_log_print(int, const char* tag, const char* fmt, ...) {
    va_list ap; va_start(ap, fmt);
    std::printf("  [%s] ", tag);
    std::vprintf(fmt, ap);
    std::printf("\n");
    va_end(ap);
    return 0;
}

AAsset* AAssetManager_open(AAssetManager*, const char* name, int) {
    std::FILE* f = std::fopen((g_assetRoot + name).c_str(), "rb");
    if (!f && !g_debugRoot.empty())
        f = std::fopen((g_debugRoot + name).c_str(), "rb");
    if (!f) return nullptr;
    std::fseek(f, 0, SEEK_END);
    const long n = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    auto* a = new AAsset{};
    a->data.resize((size_t)n);
    if (std::fread(a->data.data(), 1, (size_t)n, f) != (size_t)n) { /* пусто */ }
    std::fclose(f);
    return a;
}
off_t       AAsset_getLength(AAsset* a) { return (off_t)a->data.size(); }
long        AAsset_getLength64(AAsset* a) { return (long)a->data.size(); }
const void* AAsset_getBuffer(AAsset* a) { return a->data.data(); }
int         AAsset_read(AAsset*, void*, size_t) { return 0; }
void        AAsset_close(AAsset* a) { delete a; }
}

// ------------------------------------------------------------
namespace {

int g_validationErrors = 0;

VKAPI_ATTR VkBool32 VKAPI_CALL dbgCb(
        VkDebugUtilsMessageSeverityFlagBitsEXT sev,
        VkDebugUtilsMessageTypeFlagsEXT,
        const VkDebugUtilsMessengerCallbackDataEXT* d, void*)
{
    if (sev & (VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT |
               VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT)) {
        ++g_validationErrors;
        std::printf("  [слой] %s: %s\n",
                    d->pMessageIdName ? d->pMessageIdName : "?", d->pMessage);
    }
    return VK_FALSE;
}

#define VKOK(x) do { VkResult r_ = (x); if (r_ != VK_SUCCESS) { \
    std::printf("vkcheck: %s -> %d (%s:%d)\n", #x, (int)r_, __FILE__, __LINE__); \
    return 1; } } while (0)

u32 memType(VkPhysicalDevice p, u32 bits, VkMemoryPropertyFlags want) {
    VkPhysicalDeviceMemoryProperties mp;
    vkGetPhysicalDeviceMemoryProperties(p, &mp);
    for (u32 i = 0; i < mp.memoryTypeCount; ++i)
        if ((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & want) == want) return i;
    return 0xFFFFFFFFu;
}

} // namespace

int main(int argc, char** argv) {
    int   W = 900, H = 560;
    u64   seed = 12648430;
    float px = 33.6f, pz = -2.7f, height = 5.f, yaw = 1.2f, pitch = -0.10f;
    int   viewDist = 7;          // чанков, как в настройках игры
    int   debugMode = -1;
    const char* cull = "";
    int   oneTri = -1;
    bool  assertSolid = false;
    // Зерно крапчатости блоков. По умолчанию — зерно мира, как в
    // игре. Отдельным ключом его можно развести с зерном мира и
    // получить ТОТ ЖЕ рельеф с другой крапчатостью: иначе «цвет
    // зависит от зерна» не отличить от «мир стал другим».
    u64   tintSeed = 0;
    bool  tintGiven = false;
    // С чем сверить получившийся кадр. Утверждения вида «два кадра
    // совпадают» и «два кадра различаются чуть-чуть» — про пару
    // кадров, и проверяются только попиксельно.
    const char* diffPath = nullptr;
    // Мобильный GPU исполняет mediump как 16-битное число, а
    // программная реализация — как 32-битное. Ключ включает настоящую
    // половинную точность: шейдеры подсовываются пропущенные через
    // spirv-opt --convert-relaxed-to-half, а устройство должно уметь
    // shaderFloat16.
    bool  fp16 = false;
    // Тот же отладочный вид, что включается в игре ключом
    // debug_shading: номер едет в свободной компоненте screenSize.z,
    // и шейдер игры разбирает его сам.
    int   shading = 0;
    // Время суток, [0,1): 0 — полночь, 0.5 — полдень. Без ключа
    // солнце стоит там же, где стояло всегда, и прежние снимки
    // повторяются байт в байт.
    float timeOfDay = 0.f;
    bool  timeGiven = false;
    // Минимальная детерминированная сцена вместо генератора: та же
    // функция строит её и в игре, см. world/debug_scene.
    bool  minimalScene = false;
    bool  camGiven = false;
    int   bestTri = -1, bestMesh = -1;
    float bestArea = 0.f;
    u32   bestFace = 0;
    const char* out = "build/vkcheck/frame.ppm";
    float cloud = 0.f, rain = 0.f, rainbow = 0.f, windX = 0.f, windZ = 0.f;
    bool  weatherGiven = false;
    // Небо по умолчанию НЕ рисуется: фон заливается ровным цветом,
    // иначе счёт «пикселей, отличных от фона» потерял бы смысл. Но
    // сам шейдер неба — самый дорогой в кадре и единственный, где
    // живут тучи и радуга, и посмотреть на него было нечем.
    bool  drawSky = false;
    // Факел в руке. Светит из точки камеры; без ключа источников нет
    // вовсе, и прежние снимки повторяются байт в байт.
    float torch = 0.f;
    // Растения: модели, сборка кадра и шейдер — те же, что в игре.
    // Выключаются ключом --flora 0: сравнить «до» и «после».
    bool  drawFlora = true;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&]() -> const char* { return (i + 1 < argc) ? argv[++i] : "0"; };
        if      (a == "--size")   { W = atoi(next()); H = atoi(next()); }
        else if (a == "--seed")   seed = (u64)strtoull(next(), nullptr, 10);
        else if (a == "--pos")    { px = (float)atof(next()); pz = (float)atof(next()); camGiven = true; }
        else if (a == "--height") { height = (float)atof(next()); camGiven = true; }
        else if (a == "--yaw")    { yaw = (float)atof(next()); camGiven = true; }
        else if (a == "--pitch")  { pitch = (float)atof(next()); camGiven = true; }
        else if (a == "--viewdist") viewDist = atoi(next());
        else if (a == "--out")    out = next();
        else if (a == "--assets") g_assetRoot = std::string(next()) + "/";
        else if (a == "--debug-assets") g_debugRoot = std::string(next()) + "/";
        else if (a == "--debug") debugMode = atoi(next());
        else if (a == "--cull")  cull = next();
        else if (a == "--onetri") oneTri = atoi(next());
        else if (a == "--assert-solid") assertSolid = true;
        else if (a == "--tintseed") { tintSeed = (u64)strtoull(next(), nullptr, 10); tintGiven = true; }
        else if (a == "--diff")     diffPath = next();
        else if (a == "--fp16") fp16 = true;
        else if (a == "--shading") shading = atoi(next());
        else if (a == "--time")    { timeOfDay = (float)atof(next()); timeGiven = true; }
        // Погода: её видно только глазами, а глаза здесь — настоящий
        // кадр. Без этого тучи и радуга проверялись бы только тем,
        // что шейдер собрался.
        else if (a == "--sky")     drawSky = atoi(next()) != 0;
        else if (a == "--cloud")   { cloud = (float)atof(next()); weatherGiven = true; }
        else if (a == "--rain")    { rain = (float)atof(next()); weatherGiven = true; }
        else if (a == "--rainbow") { rainbow = (float)atof(next()); weatherGiven = true; }
        else if (a == "--wind")    { windX = (float)atof(next()); windZ = (float)atof(next()); weatherGiven = true; }
        // Свет от факела видно только глазами, а глаза здесь — кадр.
        else if (a == "--torch")   torch = (float)atof(next());
        else if (a == "--flora")   drawFlora = atoi(next()) != 0;
        else if (a == "--scene") {
            const std::string v = next();
            if (v == "minimal") minimalScene = true;
            else { std::printf("vkcheck: неизвестная сцена %s\n", v.c_str()); return 2; }
        }
        else { std::printf("vkcheck: неизвестный ключ %s\n", a.c_str()); return 2; }
    }

    // ---- инстанс со слоем проверки ----
    const char* layers[] = { "VK_LAYER_KHRONOS_validation" };
    const char* exts[]   = { VK_EXT_DEBUG_UTILS_EXTENSION_NAME };

    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "vkcheck";
    app.apiVersion       = VK_API_VERSION_1_1;   // та же версия, что просит игра

    VkDebugUtilsMessengerCreateInfoEXT dbg{
        VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
    dbg.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
                          VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
    dbg.messageType     = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                          VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                          VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
    dbg.pfnUserCallback = dbgCb;

    VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ici.pApplicationInfo        = &app;
    ici.enabledLayerCount       = 1;
    ici.ppEnabledLayerNames     = layers;
    ici.enabledExtensionCount   = 1;
    ici.ppEnabledExtensionNames = exts;
    ici.pNext                   = &dbg;

    VkInstance inst;
    VKOK(vkCreateInstance(&ici, nullptr, &inst));
    {
        auto mk = (PFN_vkCreateDebugUtilsMessengerEXT)vkGetInstanceProcAddr(
            inst, "vkCreateDebugUtilsMessengerEXT");
        VkDebugUtilsMessengerEXT m;
        if (mk) mk(inst, &dbg, nullptr, &m);
    }

    u32 n = 0;
    vkEnumeratePhysicalDevices(inst, &n, nullptr);
    if (!n) { std::printf("vkcheck: нет устройств Vulkan\n"); return 1; }
    std::vector<VkPhysicalDevice> devs(n);
    vkEnumeratePhysicalDevices(inst, &n, devs.data());
    VkPhysicalDevice phys = devs[0];
    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(phys, &props);
    std::printf("vkcheck: %s (API %u.%u)\n", props.deviceName,
                VK_VERSION_MAJOR(props.apiVersion), VK_VERSION_MINOR(props.apiVersion));

    u32 qn = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(phys, &qn, nullptr);
    std::vector<VkQueueFamilyProperties> qs(qn);
    vkGetPhysicalDeviceQueueFamilyProperties(phys, &qn, qs.data());
    u32 fam = 0;
    for (u32 i = 0; i < qn; ++i) if (qs[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) { fam = i; break; }

    float prio = 1.f;
    VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qci.queueFamilyIndex = fam; qci.queueCount = 1; qci.pQueuePriorities = &prio;
    VkPhysicalDeviceFeatures feats{};
    feats.samplerAnisotropy = VK_TRUE;
    feats.fillModeNonSolid  = VK_TRUE;
    VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    dci.queueCreateInfoCount = 1; dci.pQueueCreateInfos = &qci;
    dci.pEnabledFeatures = &feats;
    VkPhysicalDeviceShaderFloat16Int8FeaturesKHR f16{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_FLOAT16_INT8_FEATURES_KHR};
    const char* f16Exts[] = { VK_KHR_SHADER_FLOAT16_INT8_EXTENSION_NAME };
    if (fp16) {
        f16.shaderFloat16 = VK_TRUE;
        dci.pNext = &f16;
        dci.enabledExtensionCount   = 1;
        dci.ppEnabledExtensionNames = f16Exts;
    }
    VkDevice dev;
    VKOK(vkCreateDevice(phys, &dci, nullptr, &dev));
    VkQueue queue;
    vkGetDeviceQueue(dev, fam, 0, &queue);

    // ---- цели вывода: те же форматы, что на устройстве ----
    const VkFormat COLOR = VK_FORMAT_R8G8B8A8_UNORM;
    const VkFormat DEPTH = VK_FORMAT_D32_SFLOAT;

    auto makeImage = [&](VkFormat fmt, VkImageUsageFlags usage, VkImageAspectFlags aspect,
                         VkImage& img, VkDeviceMemory& mem, VkImageView& view) -> bool {
        VkImageCreateInfo ii{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        ii.imageType = VK_IMAGE_TYPE_2D; ii.format = fmt;
        ii.extent = { (u32)W, (u32)H, 1 };
        ii.mipLevels = 1; ii.arrayLayers = 1;
        ii.samples = VK_SAMPLE_COUNT_1_BIT; ii.tiling = VK_IMAGE_TILING_OPTIMAL;
        ii.usage = usage; ii.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        if (vkCreateImage(dev, &ii, nullptr, &img) != VK_SUCCESS) return false;
        VkMemoryRequirements req; vkGetImageMemoryRequirements(dev, img, &req);
        VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        ai.allocationSize = req.size;
        ai.memoryTypeIndex = memType(phys, req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (vkAllocateMemory(dev, &ai, nullptr, &mem) != VK_SUCCESS) return false;
        vkBindImageMemory(dev, img, mem, 0);
        VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vi.image = img; vi.viewType = VK_IMAGE_VIEW_TYPE_2D; vi.format = fmt;
        vi.subresourceRange.aspectMask = aspect;
        vi.subresourceRange.levelCount = 1; vi.subresourceRange.layerCount = 1;
        return vkCreateImageView(dev, &vi, nullptr, &view) == VK_SUCCESS;
    };

    VkImage colorImg, depthImg; VkDeviceMemory colorMem, depthMem;
    VkImageView colorView, depthView;
    if (!makeImage(COLOR, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                   VK_IMAGE_ASPECT_COLOR_BIT, colorImg, colorMem, colorView)) return 1;
    if (!makeImage(DEPTH, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
                   VK_IMAGE_ASPECT_DEPTH_BIT, depthImg, depthMem, depthView)) return 1;

    // ---- проход рендера: ровно тот, что строит vk::Context ----
    VkRenderPass rp;
    if (!vk::createVoxelRenderPass(dev, COLOR, DEPTH, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, &rp)) {
        std::printf("vkcheck: проход рендера не создан\n"); return 1;
    }

    VkImageView atts[2] = { colorView, depthView };
    VkFramebufferCreateInfo fbi{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
    fbi.renderPass = rp; fbi.attachmentCount = 2; fbi.pAttachments = atts;
    fbi.width = (u32)W; fbi.height = (u32)H; fbi.layers = 1;
    VkFramebuffer fb;
    VKOK(vkCreateFramebuffer(dev, &fbi, nullptr, &fb));

    // ---- конвейер: настоящие шейдеры и настоящее описание ----
    vk::ShaderCache shaders;
    shaders.init(dev, nullptr);
    vk::DescriptorSet desc;
    if (!desc.create(dev, 1)) { std::printf("vkcheck: дескрипторы\n"); return 1; }

    vk::Buffer ubo;
    if (!ubo.create(dev, phys, sizeof(render::CameraUbo), vk::BufferUsage::Uniform, true)) return 1;
    desc.bindUbo(0, ubo.handle(), sizeof(render::CameraUbo));

    vk::GraphicsPipeline opaquePipe, blendPipe;
    {
        vk::PipelineDesc d = render::voxelPipelineDesc(rp, desc.layout(), DEPTH);
        if (!std::strcmp(cull, "none"))  d.cullMode = VK_CULL_MODE_NONE;
        if (!std::strcmp(cull, "front")) d.cullMode = VK_CULL_MODE_FRONT_BIT;
        if (!std::strcmp(cull, "back"))  d.cullMode = VK_CULL_MODE_BACK_BIT;
        if (!std::strcmp(cull, "ccw"))   d.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
        char dbgName[64];
        if (debugMode >= 0) {
            std::snprintf(dbgName, sizeof(dbgName), "shaders/debug%d.frag.spv", debugMode);
            d.fragName = dbgName;   // вершинный шейдер остаётся игровым
        }
        if (!opaquePipe.create(dev, shaders, d)) { std::printf("vkcheck: конвейер\n"); return 1; }
        render::makeVoxelBlendDesc(d);
        if (!blendPipe.create(dev, shaders, d)) { std::printf("vkcheck: конвейер (вода)\n"); return 1; }
    }

    // Небо — тот же конвейер, что у render::Skybox: полноэкранный
    // треугольник, проверка глубины без записи.
    vk::GraphicsPipeline skyPipe;
    if (drawSky) {
        vk::PipelineDesc d{};
        d.renderPass   = rp;
        d.descLayout   = desc.layout();
        d.vertName     = "shaders/sky.vert.spv";
        d.fragName     = "shaders/sky.frag.spv";
        d.depthFormat  = DEPTH;
        d.cullMode     = VK_CULL_MODE_NONE;
        d.depthTest    = true;
        d.depthWrite   = false;
        d.blend        = false;
        d.bindings     = nullptr;
        d.bindingCount = 0;
        d.attrs        = nullptr;
        d.attrCount    = 0;
        if (!skyPipe.create(dev, shaders, d)) {
            std::printf("vkcheck: конвейер неба\n");
            return 1;
        }
    }

    // ---- мир: тот же генератор и тот же мешер ----
    world::blocks();
    world::TerrainGenerator gen(seed);
    // Радиус в чанках — как дальность прорисовки в игре.
    const int R = viewDist;
    struct Mesh { vk::Buffer vb, ib; u32 opaque = 0, total = 0; glm::vec3 origin{0}; };
    std::vector<Mesh> meshes;
    std::vector<std::shared_ptr<world::Chunk>> chunks;
    std::vector<world::TerrainGenerator::Column> cols;
    const i32 pcx = (i32)std::floor(px / (float)world::CHUNK_SIZE);
    const i32 pcz = (i32)std::floor(pz / (float)world::CHUNK_SIZE);
    for (i32 cz = pcz - R; cz <= pcz + R; ++cz)
        for (i32 cx = pcx - R; cx <= pcx + R; ++cx) {
            auto c = std::make_shared<world::Chunk>();
            c->coord = { cx, 0, cz };
            if (minimalScene) {
                world::buildMinimalScene(*c);
            } else {
                world::computeChunkColumns(gen, cx, cz, cols);
                world::generateChunkVoxels(*c, gen, cols.data(), seed);
            }
            c->generated.store(true);
            chunks.push_back(c);
        }
    auto findChunk = [&](i32 cx, i32 cz) -> const world::Chunk* {
        for (auto& c : chunks) if (c->coord.x == cx && c->coord.z == cz) return c.get();
        return nullptr;
    };

    std::vector<world::Quad> quads;
    std::vector<render::VoxelVertex> verts;
    std::vector<u32> idx;
    usize totalQuads = 0;
    // Статистика по ВСЕМ чанкам: то, что реально уходит на GPU.
    usize aoHist[4] = {}, skyHist[8] = {}, faceHist[8] = {}, vertTotal = 0;
    usize bySky[6][8] = {};
    std::map<u32, usize> colorHist;
    for (auto& c : chunks) {
        world::ChunkNeighbors nb{};
        nb.nx = findChunk(c->coord.x - 1, c->coord.z);
        nb.px = findChunk(c->coord.x + 1, c->coord.z);
        nb.nz = findChunk(c->coord.x, c->coord.z - 1);
        nb.pz = findChunk(c->coord.x, c->coord.z + 1);
        world::buildGreedyMesh(*c, nb, quads);
        u32 opaque = 0;
        render::buildChunkVertices(*c, quads, verts, idx, opaque);
        totalQuads += quads.size();
        for (const auto& v : verts) {
            ++aoHist[(v.packed >> 23) & 3u];
            ++skyHist[(v.packed >> 25) & 7u];
            ++faceHist[(v.packed >> 20) & 7u];
            ++bySky[(v.packed >> 20) & 7u][(v.packed >> 25) & 7u];
            ++colorHist[((u32)v.r << 16) | ((u32)v.g << 8) | v.b];
            ++vertTotal;
        }
        if (idx.empty()) continue;

        Mesh m;
        m.origin = { (float)c->coord.x * world::CHUNK_SIZE, 0.f,
                     (float)c->coord.z * world::CHUNK_SIZE };
        m.opaque = opaque;
        m.total  = (u32)idx.size();
        if (!m.vb.create(dev, phys, verts.size() * sizeof(render::VoxelVertex),
                         vk::BufferUsage::Vertex, true)) return 1;
        if (!m.ib.create(dev, phys, idx.size() * sizeof(u32),
                         vk::BufferUsage::Index, true)) return 1;
        m.vb.write(verts.data(), verts.size() * sizeof(render::VoxelVertex));
        m.ib.write(idx.data(), idx.size() * sizeof(u32));
        meshes.push_back(std::move(m));
    }
    std::printf("vkcheck: чанков %zu, квадов %zu, мешей %zu\n",
                chunks.size(), totalQuads, meshes.size());
    {
        std::printf("  вершин всего %zu\n", vertTotal);
        std::printf("  AO   :"); for (int i = 0; i < 4; ++i) std::printf(" %zu", aoHist[i]);
        std::printf("\n  небо :"); for (int i = 0; i < 8; ++i) std::printf(" %zu", skyHist[i]);
        std::printf("\n  грань:"); for (int i = 0; i < 6; ++i) std::printf(" %zu", faceHist[i]);
        std::printf("\n");
        static const char* FACE_NAME[6] = { "+X", "-X", "+Y", "-Y", "+Z", "-Z" };
        for (int f = 0; f < 6; ++f) {
            std::printf("  %s небо:", FACE_NAME[f]);
            for (int k = 0; k < 8; ++k) std::printf(" %zu", bySky[f][k]);
            std::printf("\n");
        }
        std::vector<std::pair<usize,u32>> top;
        for (auto& [c, k] : colorHist) top.push_back({ k, c });
        std::sort(top.rbegin(), top.rend());
        std::printf("  цвета вершин, по убыванию:\n");
        for (usize i = 0; i < top.size() && i < 8; ++i)
            std::printf("    #%06X  %zu (%.1f%%)\n", top[i].second, top[i].first,
                        100.0 * (double)top[i].first / (double)vertTotal);
    }

    // Положение глаз и пиксели на единицу нужны траве раньше, чем
    // настраивается камера: порог по экранному размеру считается при
    // наборе инстансов, как и в игре.
    const float eyeY = (minimalScene && !camGiven)
                     ? world::SCENE_EYE_Y
                     : (float)gen.surfaceHeight((i32)px, (i32)pz) + height;
    const float pxPerUnit = (float)H * 0.5f / std::tan(glm::radians(70.f) * 0.5f);

    // ---- камера: тот же класс, что в игре ----
    render::Camera cam;
    // То же, что делает игра каждый кадр: крапчатость блоков берётся
    // из зерна мира.
    cam.setWorldSeed(tintGiven ? tintSeed : seed);
    cam.setAspect((float)W / (float)H);
    cam.setViewport((u32)W, (u32)H);
    // Те же числа, что ставит RenderSystem::prepareFrame при дальности
    // прорисовки 7 чанков — иначе туман в проверке гуще, чем в игре.
    {
        // Дальность берётся из ключа, а не из вшитой семёрки: игра
        // считает туман по своей настройке, и они обязаны совпадать.
        const float vdBlocks = (float)viewDist * (float)world::CHUNK_SIZE;
        cam.setFog(vdBlocks * 0.55f, vdBlocks * 0.94f);
    }
    // Освещение — те же константы, что прибивает игра в отладочной
    // сцене. Вшитые здесь числа уже однажды разошлись бы молча.
    cam.setSunDir(glm::vec3(world::SCENE_SUN_X, world::SCENE_SUN_Y,
                            world::SCENE_SUN_Z));
    cam.setSky(glm::vec3(world::SCENE_SKY_R, world::SCENE_SKY_G,
                         world::SCENE_SKY_B),
               world::SCENE_SKY_LIGHT, world::SCENE_TIME_OF_DAY);
    // Камера ставится ТЕМ ЖЕ вызовом, что в игре.
    //
    // Раньше здесь матрица вида собиралась вручную рядом с классом
    // камеры — второй владелец на хосте, и разойтись с игрой они могли
    // молча. Теперь и там и там setDebugCamera, а матрицы считает
    // Camera::toUbo.
    //
    // В сцене глаз задан абсолютной высотой из debug_scene.h; в
    // обычном режиме --height по-прежнему отсчитывается от рельефа.
    glm::vec3 eye;
    if (minimalScene && !camGiven) {
        eye   = { world::SCENE_EYE_X, world::SCENE_EYE_Y, world::SCENE_EYE_Z };
        yaw   = world::SCENE_YAW;
        pitch = world::SCENE_PITCH;
    } else {
        eye = { px, (float)gen.surfaceHeight((i32)px, (i32)pz) + height, pz };
    }
    cam.setDebugCamera(eye, yaw, pitch);

    // Свет суток берётся из того же DayCycle, что у игры, а не из
    // своих чисел: иначе снимок инструмента освещён не так, как кадр
    // на устройстве, и сравнивать их бессмысленно.
    if (timeGiven) {
        world::DayCycle dc;
        dc.reset(timeOfDay, 0);
        cam.setSunDir(dc.sunDirection());
        cam.setSky(dc.skyColor(), dc.skyLight(), dc.timeOfDay());
        const glm::vec3 sd = dc.sunDirection();
        std::printf("vkcheck: время суток %.3f, солнце %.2f %.2f %.2f, "
                    "свет неба %.2f\n",
                    (double)dc.timeOfDay(), (double)sd.x, (double)sd.y,
                    (double)sd.z, (double)dc.skyLight());
    }

    if (weatherGiven) {
        cam.setWeather(cloud, rain, rainbow, 0.f, glm::vec2(windX, windZ));
        std::printf("vkcheck: погода — тучи %.2f, осадки %.2f, радуга %.2f, "
                    "ветер %.1f %.1f\n",
                    (double)cloud, (double)rain, (double)rainbow,
                    (double)windX, (double)windZ);
    }

    if (torch > 0.f) {
        // Ровно тот же источник, что даёт игре факел в руке: берётся
        // из определения блока, а не выписан числами заново.
        render::PointLight l = render::LightField::heldLight(
            world::TORCH, eye - glm::vec3(0.f, render::LightField::HAND_HEIGHT, 0.f));
        l.power *= torch;
        cam.setLights(&l, 1);
        std::printf("vkcheck: факел — сила %.2f, радиус %.1f, цвет %.2f %.2f %.2f\n",
                    (double)l.power, (double)l.radius,
                    (double)l.color.r, (double)l.color.g, (double)l.color.b);
    }

    render::CameraUbo u = cam.toUbo(world::SCENE_TIME_SEC);
    u.screenSize = glm::vec4((float)W, (float)H, (float)shading, 0.f);
    ubo.write(&u, sizeof(u));

    // ---- растения: те же модели, та же сборка кадра, тот же шейдер ----
    //
    // Меши здесь лежат в памяти, видимой процессору, — копировать через
    // промежуточный буфер инструменту незачем. Всё остальное — как в
    // render::FloraRenderer: отбор по вокселям (floraStillStands),
    // FloraBatch, один вызов на меш.
    vk::GraphicsPipeline floraPipe;
    vk::Buffer floraVb, floraIb, floraInst;
    struct FloraRange { u32 first = 0, count = 0; i32 vertexOffset = 0; };
    std::vector<FloraRange> floraRanges;
    render::FloraBatch floraBatch;
    if (drawFlora && !minimalScene) {
        const std::vector<render::VoxelMesh> fm = render::buildFloraMeshes();
        std::vector<render::VoxelModelVertex> fv;
        std::vector<u32> fi, fq;
        for (const auto& m : fm) {
            floraRanges.push_back({ (u32)fi.size(), (u32)m.indices.size(), (i32)fv.size() });
            fq.push_back(m.quadCount());
            fv.insert(fv.end(), m.vertices.begin(), m.vertices.end());
            fi.insert(fi.end(), m.indices.begin(), m.indices.end());
        }
        if (!floraVb.create(dev, phys, fv.size() * sizeof(render::VoxelModelVertex),
                            vk::BufferUsage::Vertex, true) ||
            !floraIb.create(dev, phys, fi.size() * sizeof(u32), vk::BufferUsage::Index, true))
            return 1;
        floraVb.write(fv.data(), fv.size() * sizeof(render::VoxelModelVertex));
        floraIb.write(fi.data(), fi.size() * sizeof(u32));

        vk::PipelineDesc d{};
        d.renderPass   = rp;
        d.descLayout   = desc.layout();
        d.vertName     = "shaders/flora.vert.spv";
        d.fragName     = "shaders/mob.frag.spv";
        d.depthFormat  = DEPTH;
        d.cullMode     = VK_CULL_MODE_BACK_BIT;
        d.depthTest    = true;
        d.depthWrite   = true;
        d.blend        = false;
        d.bindings     = render::FLORA_BINDINGS;
        d.bindingCount = 2;
        d.attrs        = render::FLORA_ATTRS;
        d.attrCount    = 5;
        if (!floraPipe.create(dev, shaders, d)) { std::printf("vkcheck: конвейер растений\n"); return 1; }

        usize records = 0;
        std::vector<world::FloraInstance> standing;
        floraBatch.begin(eye);
        for (auto& c : chunks) {
            records += c->flora.size();
            standing.clear();
            auto voxel = [&](i32 x, i32 y, i32 z) -> u16 {
                return (u32)y < (u32)world::CHUNK_SIZE_Y ? c->voxels[world::chunkIndex(x, y, z)]
                                                         : world::AIR;
            };
            for (const auto& f : c->flora)
                if (world::floraStillStands(f, voxel)) standing.push_back(f);
            floraBatch.addChunk({ (float)c->coord.x * world::CHUNK_SIZE, 0.f,
                                  (float)c->coord.z * world::CHUNK_SIZE }, standing);
        }
        floraBatch.finish(&fq);
        const auto& fin = floraBatch.instances();
        if (!fin.empty()) {
            if (!floraInst.create(dev, phys, fin.size() * sizeof(render::FloraGpuInstance),
                                  vk::BufferUsage::Vertex, true)) return 1;
            floraInst.write(fin.data(), fin.size() * sizeof(render::FloraGpuInstance));
        }
        {
            // Разбор по ступеням и видам: куда уходят квады.
            usize q[render::FLORA_LODS] = {}, n[render::FLORA_LODS] = {};
            std::map<std::string, std::pair<usize, usize>> byKind;
            for (const auto& dr : floraBatch.draws()) {
                const u32 lod = dr.mesh % render::FLORA_LODS;
                q[lod] += (usize)dr.count * fq[dr.mesh];
                n[lod] += dr.count;
                u32 model = dr.mesh / render::FLORA_LODS, kind = 0;
                for (u32 k = 0; k < (u32)world::FloraKind::Count; ++k)
                    if (render::floraModelIndex((world::FloraKind)k, 0) <= model) kind = k;
                auto& e = byKind[std::to_string(kind)];
                e.first += dr.count; e.second += (usize)dr.count * fq[dr.mesh];
            }
            std::printf("vkcheck: растения по ступеням (шт/квадов):");
            for (u32 l = 0; l < render::FLORA_LODS; ++l) std::printf(" %u — %zu/%zu", l, n[l], q[l]);
            std::printf("\n");
            for (auto& [k, e] : byKind)
                std::printf("  вид %s: %zu шт, %zu квадов\n", k.c_str(), e.first, e.second);
        }
        std::printf("vkcheck: растений в чанках %zu, в кадре %zu, вызовов %zu, квадов %u "
                    "(мешей %zu, вершин %zu)\n",
                    records, fin.size(), floraBatch.draws().size(), floraBatch.quads(),
                    fm.size(), fv.size());
    }

    if (minimalScene) {
        // Та же строка, что печатает игра: числа обязаны сойтись.
        std::printf("vkcheck: debug_scene=true | камера %.3f %.3f %.3f, "
                    "yaw %.4f pitch %.4f | время мира %.3f, шаг кадра %.4f | "
                    "мобы 0, NPC 0, предметы 0, снаряды 0 | "
                    "чанков загружено %zu, нарисовано %zu\n",
                    (double)eye.x, (double)eye.y, (double)eye.z,
                    (double)yaw, (double)pitch,
                    (double)world::SCENE_TIME_SEC, (double)world::SCENE_FIXED_DT,
                    chunks.size(), meshes.size());
    }

    // Знак площади в координатах кадра — на тех же матрицах и тех же
    // треугольниках, что уходят на GPU. По спецификации Vulkan
    // (Basic Polygon Rasterization) лицевой считается отрицательная
    // площадь при VK_FRONT_FACE_CLOCKWISE и положительная при
    // COUNTER_CLOCKWISE. Верхняя грань (+Y) при взгляде сверху вниз
    // обращена к камере наружу: её знак и есть знак лицевой грани.
    {
        usize pos[6] = {}, neg[6] = {};
        std::vector<world::Quad> q2;
        std::vector<render::VoxelVertex> v2;
        std::vector<u32> i2;
        u32 op2 = 0;
        for (auto& c : chunks) {
            world::ChunkNeighbors nb{};
            nb.nx = findChunk(c->coord.x - 1, c->coord.z);
            nb.px = findChunk(c->coord.x + 1, c->coord.z);
            nb.nz = findChunk(c->coord.x, c->coord.z - 1);
            nb.pz = findChunk(c->coord.x, c->coord.z + 1);
            world::buildGreedyMesh(*c, nb, q2);
            render::buildChunkVertices(*c, q2, v2, i2, op2);
            const glm::vec3 org{ (float)c->coord.x * world::CHUNK_SIZE, 0.f,
                                 (float)c->coord.z * world::CHUNK_SIZE };
            auto fb = [&](const render::VoxelVertex& v) {
                const u32 p = v.packed;
                const glm::vec3 w = org + glm::vec3((float)(p & 63u),
                                                    (float)((p >> 6) & 255u),
                                                    (float)((p >> 14) & 63u));
                const glm::vec4 cl = u.viewProj * glm::vec4(w, 1.f);
                const glm::vec3 nd = glm::vec3(cl) / cl.w;
                return glm::vec2{ (nd.x * 0.5f + 0.5f) * (float)W,
                                  (nd.y * 0.5f + 0.5f) * (float)H };
            };
            for (u32 k = 0; k + 2 < op2; k += 3) {
                const auto& A = v2[i2[k]]; const auto& B = v2[i2[k+1]];
                const auto& C = v2[i2[k+2]];
                const u32 face = (A.packed >> 20) & 7u;
                if (face > 5) continue;
                const glm::vec4 ca = u.viewProj * glm::vec4(org + glm::vec3(
                    (float)(A.packed & 63u), (float)((A.packed >> 6) & 255u),
                    (float)((A.packed >> 14) & 63u)), 1.f);
                if (ca.w <= 0.f) continue;   // за камерой — знак не определён
                const glm::vec2 a = fb(A), b = fb(B), d2 = fb(C);
                const float area = 0.5f * ((a.x*b.y - b.x*a.y) + (b.x*d2.y - d2.x*b.y)
                                         + (d2.x*a.y - a.x*d2.y));
                if (area > 0.f) ++pos[face]; else if (area < 0.f) ++neg[face];
            }
        }
        // Самый крупный треугольник, целиком попавший на экран: на нём
        // и проводим очную ставку спецификации с драйвером.
        {
            usize mi = 0;
            for (auto& c : chunks) {
                world::ChunkNeighbors nb0{};
                nb0.nx = findChunk(c->coord.x - 1, c->coord.z);
                nb0.px = findChunk(c->coord.x + 1, c->coord.z);
                nb0.nz = findChunk(c->coord.x, c->coord.z - 1);
                nb0.pz = findChunk(c->coord.x, c->coord.z + 1);
                world::buildGreedyMesh(*c, nb0, q2);
                render::buildChunkVertices(*c, q2, v2, i2, op2);
                if (i2.empty()) continue;
                const glm::vec3 org{ (float)c->coord.x * world::CHUNK_SIZE, 0.f,
                                     (float)c->coord.z * world::CHUNK_SIZE };
                for (u32 t = 0; (t * 3 + 2) < op2; ++t) {
                    glm::vec2 sp[3];
                    bool okTri = true;
                    for (int k = 0; k < 3; ++k) {
                        const u32 pk = v2[i2[t*3+k]].packed;
                        const glm::vec3 w = org + glm::vec3((float)(pk & 63u),
                                                            (float)((pk >> 6) & 255u),
                                                            (float)((pk >> 14) & 63u));
                        const glm::vec4 cl = u.viewProj * glm::vec4(w, 1.f);
                        if (cl.w <= 0.01f) { okTri = false; break; }
                        const glm::vec3 nd = glm::vec3(cl) / cl.w;
                        sp[k] = { (nd.x * 0.5f + 0.5f) * (float)W,
                                  (nd.y * 0.5f + 0.5f) * (float)H };
                        if (sp[k].x < 0 || sp[k].x > (float)W ||
                            sp[k].y < 0 || sp[k].y > (float)H) { okTri = false; break; }
                    }
                    if (!okTri) continue;
                    const float area = 0.5f *
                        ((sp[0].x*sp[1].y - sp[1].x*sp[0].y) +
                         (sp[1].x*sp[2].y - sp[2].x*sp[1].y) +
                         (sp[2].x*sp[0].y - sp[0].x*sp[2].y));
                    if (std::fabs(area) > std::fabs(bestArea)) {
                        bestArea = area; bestTri = (int)t; bestMesh = (int)mi;
                        bestFace = (v2[i2[t*3]].packed >> 20) & 7u;
                    }
                }
                ++mi;
            }
            std::printf("  самый крупный видимый треугольник: меш %d, номер %d, "
                        "грань %u, площадь кадра %.1f\n",
                        bestMesh, bestTri, bestFace, (double)bestArea);
        }
        static const char* NM[6] = { "+X","-X","+Y","-Y","+Z","-Z" };
        std::printf("  знак площади в кадре (та же матрица, что у GPU):\n");
        for (int f = 0; f < 6; ++f)
            std::printf("    %s: >0 %zu, <0 %zu\n", NM[f], pos[f], neg[f]);
    }

    // ---- запись и отправка кадра ----
    VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pci.queueFamilyIndex = fam;
    VkCommandPool pool; VKOK(vkCreateCommandPool(dev, &pci, nullptr, &pool));
    VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cai.commandPool = pool; cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; cai.commandBufferCount = 1;
    VkCommandBuffer cmd; VKOK(vkAllocateCommandBuffers(dev, &cai, &cmd));

    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    VKOK(vkBeginCommandBuffer(cmd, &bi));

    // Небо шейдером здесь НЕ рисуется: инструмент про геометрию и свет
    // террейна, и лишний полноэкранный проход только мешал бы считать
    // пиксели. Фон — заливка цветом неба этого времени суток, чтобы
    // ночной снимок не выглядел дневным.
    VkClearValue clears[2];
    clears[0].color = {{ 0.45f, 0.62f, 0.85f, 1.0f }};
    if (timeGiven) {
        const glm::vec3 sc = cam.skyColor();
        clears[0].color = {{ sc.x, sc.y, sc.z, 1.0f }};
    }
    clears[1].depthStencil = { 1.0f, 0 };
    VkRenderPassBeginInfo rbi{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    rbi.renderPass = rp; rbi.framebuffer = fb;
    rbi.renderArea.extent = { (u32)W, (u32)H };
    rbi.clearValueCount = 2; rbi.pClearValues = clears;
    vkCmdBeginRenderPass(cmd, &rbi, VK_SUBPASS_CONTENTS_INLINE);

    VkViewport vp{}; vp.width = (float)W; vp.height = (float)H; vp.maxDepth = 1.f;
    vkCmdSetViewport(cmd, 0, 1, &vp);
    VkRect2D sc{}; sc.extent = { (u32)W, (u32)H };
    vkCmdSetScissor(cmd, 0, 1, &sc);

    VkDescriptorSet set = desc.set(0);
    // Один треугольник с заранее посчитанным знаком площади: это и
    // есть очная ставка спецификации с драйвером. Если треугольник с
    // отрицательной площадью при VK_FRONT_FACE_CLOCKWISE и отсечении
    // задних граней не даёт ни одного пикселя, значит лицевой
    // считается не та сторона, на которую рассчитывал код.
    auto drawOne = [&](vk::GraphicsPipeline& pipe, int meshIdx, int tri) {
        if (meshIdx < 0 || (usize)meshIdx >= meshes.size()) return;
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe.handle());
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe.layout(),
                                0, 1, &set, 0, nullptr);
        auto& m = meshes[(usize)meshIdx];
        const render::ChunkPush push{ glm::vec4(m.origin, 0.f) };
        vkCmdPushConstants(cmd, pipe.layout(), VK_SHADER_STAGE_VERTEX_BIT,
                           0, sizeof(push), &push);
        VkBuffer vb = m.vb.handle(); VkDeviceSize off = 0;
        vkCmdBindVertexBuffers(cmd, 0, 1, &vb, &off);
        vkCmdBindIndexBuffer(cmd, m.ib.handle(), 0, VK_INDEX_TYPE_UINT32);
        vkCmdDrawIndexed(cmd, 3, 1, (u32)tri * 3, 0, 0);
    };

    auto drawAll = [&](vk::GraphicsPipeline& pipe, bool blended) {
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe.handle());
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe.layout(),
                                0, 1, &set, 0, nullptr);
        for (auto& m : meshes) {
            const u32 first = blended ? m.opaque : 0;
            const u32 count = blended ? m.total - m.opaque : m.opaque;
            if (!count) continue;
            const render::ChunkPush push{ glm::vec4(m.origin, 0.f) };
            vkCmdPushConstants(cmd, pipe.layout(), VK_SHADER_STAGE_VERTEX_BIT,
                               0, sizeof(push), &push);
            VkBuffer vb = m.vb.handle(); VkDeviceSize off = 0;
            vkCmdBindVertexBuffers(cmd, 0, 1, &vb, &off);
            vkCmdBindIndexBuffer(cmd, m.ib.handle(), 0, VK_INDEX_TYPE_UINT32);
            vkCmdDrawIndexed(cmd, count, 1, first, 0, 0);
        }
    };
    if (oneTri >= 0) {
        drawOne(opaquePipe, bestMesh, bestTri);
    } else {
        drawAll(opaquePipe, false);

        // Растения — сразу за ландшафтом, как в игре.
        if (floraPipe.valid() && floraInst.handle()) {
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, floraPipe.handle());
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, floraPipe.layout(),
                                    0, 1, &set, 0, nullptr);
            VkBuffer vbs[2] = { floraVb.handle(), floraInst.handle() };
            VkDeviceSize offs[2] = { 0, 0 };
            vkCmdBindVertexBuffers(cmd, 0, 2, vbs, offs);
            vkCmdBindIndexBuffer(cmd, floraIb.handle(), 0, VK_INDEX_TYPE_UINT32);
            for (const auto& dr : floraBatch.draws()) {
                const FloraRange& r = floraRanges[dr.mesh];
                vkCmdDrawIndexed(cmd, r.count, dr.count, r.first, r.vertexOffset, dr.first);
            }
        }

        // Небо — после непрозрачного и до воды, ровно как в игре.
        if (drawSky && skyPipe.valid()) {
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, skyPipe.handle());
            VkDescriptorSet ds0 = desc.set(0);
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                    skyPipe.layout(), 0, 1, &ds0, 0, nullptr);
            vkCmdDraw(cmd, 3, 1, 0, 0);
        }

        drawAll(blendPipe,  true);
    }

    vkCmdEndRenderPass(cmd);

    // Копия в буфер для чтения.
    const VkDeviceSize bytes = (VkDeviceSize)W * H * 4;
    VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bci.size = bytes; bci.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    VkBuffer readBuf; VKOK(vkCreateBuffer(dev, &bci, nullptr, &readBuf));
    VkMemoryRequirements br; vkGetBufferMemoryRequirements(dev, readBuf, &br);
    VkMemoryAllocateInfo bai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    bai.allocationSize = br.size;
    bai.memoryTypeIndex = memType(phys, br.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    VkDeviceMemory readMem; VKOK(vkAllocateMemory(dev, &bai, nullptr, &readMem));
    vkBindBufferMemory(dev, readBuf, readMem, 0);

    VkBufferImageCopy region{};
    region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.imageSubresource.layerCount = 1;
    region.imageExtent = { (u32)W, (u32)H, 1 };
    vkCmdCopyImageToBuffer(cmd, colorImg, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           readBuf, 1, &region);
    VKOK(vkEndCommandBuffer(cmd));

    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = 1; si.pCommandBuffers = &cmd;
    VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    VkFence fence; VKOK(vkCreateFence(dev, &fi, nullptr, &fence));
    VKOK(vkQueueSubmit(queue, 1, &si, fence));
    VKOK(vkWaitForFences(dev, 1, &fence, VK_TRUE, UINT64_MAX));

    void* mapped = nullptr;
    VKOK(vkMapMemory(dev, readMem, 0, bytes, 0, &mapped));
    const u8* pix = (const u8*)mapped;
    std::FILE* f = std::fopen(out, "wb");
    if (!f) { std::printf("vkcheck: не открыть %s\n", out); return 1; }
    std::fprintf(f, "P6\n%d %d\n255\n", W, H);
    for (int i = 0; i < W * H; ++i) std::fwrite(pix + (usize)i * 4, 1, 3, f);
    std::fclose(f);

    // Сверка с другим кадром: ключ --diff.
    //
    // «Один и тот же мир красится одинаково» и «другое зерно красит
    // иначе, но чуть-чуть» — утверждения о ПАРЕ кадров. Ни одно из
    // них нельзя проверить по одному кадру, поэтому сравнение живёт
    // здесь, рядом с пикселями, а не в скрипте поверх PNG.
    if (diffPath && *diffPath) {
        std::FILE* rf = std::fopen(diffPath, "rb");
        if (!rf) {
            std::printf("vkcheck: ПРОВАЛ — не открыть эталон %s\n", diffPath);
            return 1;
        }
        int rw = 0, rh = 0, rmax = 0;
        if (std::fscanf(rf, "P6 %d %d %d", &rw, &rh, &rmax) != 3 ||
            rw != W || rh != H) {
            std::printf("vkcheck: ПРОВАЛ — эталон %dx%d, а кадр %dx%d\n",
                        rw, rh, W, H);
            std::fclose(rf);
            return 1;
        }
        std::fgetc(rf);   // один разделитель после максимума
        usize diff = 0, total = (usize)W * (usize)H;
        double sumAbs = 0.0;
        int maxAbs = 0;
        for (usize i = 0; i < total; ++i) {
            u8 ref[3];
            if (std::fread(ref, 1, 3, rf) != 3) {
                std::printf("vkcheck: ПРОВАЛ — эталон %s оборван\n", diffPath);
                std::fclose(rf);
                return 1;
            }
            const u8* p2 = pix + i * 4;
            int d = 0;
            for (int c = 0; c < 3; ++c) {
                const int dc = (int)p2[c] - (int)ref[c];
                const int ad = dc < 0 ? -dc : dc;
                if (ad > d) d = ad;
            }
            if (d) ++diff;
            sumAbs += (double)d;
            if (d > maxAbs) maxAbs = d;
        }
        std::printf("vkcheck: сверка с %s: отличается пикселей %zu из %zu "
                    "(%.3f), среднее |д| %.4f, наибольшее |д| %d\n",
                    diffPath, diff, total,
                    total ? (double)diff / (double)total : 0.0,
                    total ? sumAbs / (double)total : 0.0, maxAbs);
    }

    // Признак вывернутого наизнанку мира — не дыры, а темнота.
    //
    // Отсекая наружные грани, мы показываем изнанку: у неё открытость
    // неба ноль, потому что она и правда закрыта землёй. Освещение
    // честно считает такую поверхность почти чёрной. Земля под ногами
    // при этом никуда не девается — она просто чёрная, — поэтому
    // «сколько видно фона» ничего не различает, а средняя яркость
    // нижней половины кадра различает с запасом в разы.
    double lumaBelow = 0.0;
    {
        usize drawn = 0, half = 0;
        double sum = 0.0;
        for (int y = 0; y < H; ++y)
            for (int x = 0; x < W; ++x) {
                const u8* p2 = pix + ((usize)y * (usize)W + (usize)x) * 4;
                if (p2[0] != 115 || p2[1] != 158 || p2[2] != 217) ++drawn;
                if (y >= H / 2) {
                    ++half;
                    sum += (0.2126 * p2[0] + 0.7152 * p2[1] + 0.0722 * p2[2]) / 255.0;
                }
            }
        lumaBelow = half ? sum / (double)half : 0.0;
        std::printf("vkcheck: пикселей отличных от фона: %zu; средняя яркость "
                    "нижней половины кадра: %.3f\n", drawn, lumaBelow);
    }
    // Отображение снимается здесь, после ПОСЛЕДНЕГО чтения пикселей.
    // Стояло выше, сразу за записью файла, — и вся статистика ниже
    // читала память по указателю, отданному драйверу обратно.
    vkUnmapMemory(dev, readMem);
    std::printf("vkcheck: кадр -> %s (%dx%d), сообщений слоя проверки: %d\n",
                out, W, H, g_validationErrors);
    vkDeviceWaitIdle(dev);

    // Порог с большим запасом: у целого мира в этом виде фона внизу
    // ноль, у вывернутого — больше половины.
    const bool hollow = assertSolid && lumaBelow < 0.25;
    if (hollow)
        std::printf("vkcheck: ПРОВАЛ — земля под ногами тёмная: видна изнанка мира, "
                    "наружные грани отсекаются\n");

    // Открытость неба у верхних граней.
    //
    // Множитель освещения в voxel.frag — 0.18 + 0.82 * небо/7, и
    // прямое солнце включается только с smoothstep(0.35, 0.85, небо/7).
    // Если верхние грани открытой земли уходят из семёрки, весь мир
    // темнеет втрое, а из сумрака торчат отдельные блоки, посчитанные
    // правильно. По картинке это читается как «облако точек на
    // поверхности», по числам — как перекос вот этой гистограммы.
    bool darkTop = false;
    if (assertSolid) {
        usize top = 0;
        for (int k = 0; k < 8; ++k) top += bySky[2][k];
        const double open = top ? (double)bySky[2][7] / (double)top : 0.0;
        std::printf("vkcheck: верхних граней %zu, доля с открытым небом %.3f\n",
                    top, open);
        darkTop = (top > 0 && open < 0.45);
        if (darkTop)
            std::printf("vkcheck: ПРОВАЛ — верхние грани почти не видят неба: "
                        "мир будет освещён втрое слабее положенного\n");
    }
    return (g_validationErrors || hollow || darkTop) ? 1 : 0;
}
