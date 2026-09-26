/**
 * @file isocheck.cpp
 * @brief Изометрический снимок на хосте: настоящий Vulkan, настоящий PNG.
 *
 * Тот же класс `render::IsoSnapshot`, что и в игре, те же шейдеры и
 * тот же мир. Разница ровно одна: Vulkan здесь программный
 * (lavapipe), а окна нет вовсе — снимку оно и не нужно.
 *
 * Нужен затем, что проверить изометрию числами можно на хосте
 * (tools/hostcheck), а посмотреть на неё ГЛАЗАМИ — только по
 * картинке. Устройства под рукой может не быть; программный Vulkan
 * есть всегда.
 *
 *   ./tools/isocheck/run.sh
 *   ./tools/isocheck/run.sh --size 100 --view 2 --px 12
 *   ./tools/isocheck/run.sh --all-views --no-nature
 *
 * Растения и мелкие природные вещи рисуются по умолчанию, тем же
 * путём, что в игре. --no-nature выключает их: такой снимок обязан
 * совпасть байт в байт с голым ландшафтом прежних версий — этим и
 * проверяется, что воксели и вода от растений не поменялись.
 */
#include "render/iso_snapshot.h"
#include "render/iso_png.h"
#include "render/flora_batch.h"
#include "world/flora.h"
#include "world/chunk_manager.h"
#include "world/block.h"
#include "core/job_system.h"
#include <cstdio>
#include <cstdarg>
#include <cstring>
#include <cstdlib>
#include <chrono>
#include <cmath>
#include <memory>
#include <thread>
#include <vector>
#include <vulkan/vulkan.h>
#include <string>

// ------------------------------------------------------------
// Ассеты и журнал на хосте — тем же способом, что в tools/vkcheck:
// ShaderCache обязан пройти ТОТ ЖЕ путь загрузки, что и в игре.
// ------------------------------------------------------------
struct AAsset { std::vector<char> data; };
static std::string g_assetRoot = "build/isocheck/assets/";

extern "C" {
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

#define VKOK(x) do { VkResult _r = (x); if (_r != VK_SUCCESS) { \
    std::printf("isocheck: Vulkan %d на %s:%d\n", (int)_r, __FILE__, __LINE__); \
    return 1; } } while (0)

static VKAPI_ATTR VkBool32 VKAPI_CALL dbgCb(
    VkDebugUtilsMessageSeverityFlagBitsEXT, VkDebugUtilsMessageTypeFlagsEXT,
    const VkDebugUtilsMessengerCallbackDataEXT* d, void* user)
{
    std::printf("  [слой] %s\n", d->pMessage);
    if (user) ++*(int*)user;
    return VK_FALSE;
}

int main(int argc, char** argv) {
    i32 size = 100;
    u32 viewIdx = 0;
    f32 px = 12.f;
    u64 seed = 20260920ULL;
    const char* out = "build/isocheck/iso.png";
    bool allViews = false;
    // Середина области. Выровненная по чанкам область (--pos 32 32
    // --size 64) нужна сверке «до/после»: в ней нечего обрезать.
    f32 posX = 8.5f, posZ = 8.5f;
    bool nature = true;

    for (int i = 1; i < argc; ++i) {
        auto next = [&](const char* d) { return i + 1 < argc ? argv[++i] : d; };
        if (!std::strcmp(argv[i], "--size"))       size = std::atoi(next("100"));
        else if (!std::strcmp(argv[i], "--view"))  viewIdx = (u32)std::atoi(next("0"));
        else if (!std::strcmp(argv[i], "--px"))    px = (f32)std::atof(next("12"));
        else if (!std::strcmp(argv[i], "--seed"))  seed = std::strtoull(next("1"), nullptr, 10);
        else if (!std::strcmp(argv[i], "--out"))   out = next(out);
        else if (!std::strcmp(argv[i], "--all-views")) allViews = true;
        else if (!std::strcmp(argv[i], "--pos")) {
            posX = (f32)std::atof(next("8.5"));
            posZ = (f32)std::atof(next("8.5"));
        }
        else if (!std::strcmp(argv[i], "--no-nature")) nature = false;
    }

    int layerMsgs = 0;

    // ---- Vulkan без окна ----
    const char* layers[] = { "VK_LAYER_KHRONOS_validation" };
    const char* exts[]   = { VK_EXT_DEBUG_UTILS_EXTENSION_NAME };
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "isocheck";
    app.apiVersion = VK_API_VERSION_1_1;

    VkDebugUtilsMessengerCreateInfoEXT dbg{
        VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
    dbg.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
                          VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
    dbg.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                      VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT;
    dbg.pfnUserCallback = dbgCb;
    dbg.pUserData = &layerMsgs;

    VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ici.pApplicationInfo = &app;
    ici.enabledLayerCount = 1; ici.ppEnabledLayerNames = layers;
    ici.enabledExtensionCount = 1; ici.ppEnabledExtensionNames = exts;
    ici.pNext = &dbg;
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
    if (!n) { std::printf("isocheck: нет устройств Vulkan\n"); return 1; }
    std::vector<VkPhysicalDevice> devs(n);
    vkEnumeratePhysicalDevices(inst, &n, devs.data());
    VkPhysicalDevice phys = devs[0];
    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(phys, &props);
    std::printf("isocheck: %s (API %u.%u)\n", props.deviceName,
                VK_VERSION_MAJOR(props.apiVersion), VK_VERSION_MINOR(props.apiVersion));

    u32 qn = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(phys, &qn, nullptr);
    std::vector<VkQueueFamilyProperties> qs(qn);
    vkGetPhysicalDeviceQueueFamilyProperties(phys, &qn, qs.data());
    u32 fam = 0;
    for (u32 i = 0; i < qn; ++i)
        if (qs[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) { fam = i; break; }

    float prio = 1.f;
    VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qci.queueFamilyIndex = fam; qci.queueCount = 1; qci.pQueuePriorities = &prio;
    VkPhysicalDeviceFeatures feats{};
    feats.samplerAnisotropy = VK_TRUE;
    feats.fillModeNonSolid  = VK_TRUE;
    VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    dci.queueCreateInfoCount = 1; dci.pQueueCreateInfos = &qci;
    dci.pEnabledFeatures = &feats;
    VkDevice dev;
    VKOK(vkCreateDevice(phys, &dci, nullptr, &dev));
    VkQueue queue;
    vkGetDeviceQueue(dev, fam, 0, &queue);

    // ---- настоящий мир ----
    world::blocks();
    jobs::gJobs.start(3);
    auto world = std::make_unique<world::ChunkManager>(seed, 6);

    render::IsoSnapshot snap;
    if (!snap.init(dev, phys, queue, fam, VK_FORMAT_D32_SFLOAT, nullptr)) {
        std::printf("isocheck: подсистема снимка не поднялась\n");
        return 1;
    }

    const glm::vec3 player{ posX, 70.f, posZ };
    const u32 first = allViews ? 0 : viewIdx;
    const u32 last  = allViews ? 3 : viewIdx;
    int rc = 0;

    for (u32 v = first; v <= last; ++v) {
        render::IsoRequest req;
        req.area  = render::iso::areaAround(player, size);
        req.view  = (render::iso::View)v;
        req.pixelsPerBlock = px;
        req.maxSide = 8192;
        req.nature = nature;

        const auto t0 = std::chrono::steady_clock::now();
        snap.begin(req);
        int guard = 0;
        while (snap.stage() != render::IsoStage::Ready &&
               snap.stage() != render::IsoStage::Failed &&
               guard++ < 200000) {
            world->update(player);
            snap.step(*world);
            if (snap.stage() == render::IsoStage::Preparing)
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        const auto ms = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - t0).count();

        if (snap.stage() != render::IsoStage::Ready) {
            std::printf("isocheck: вид %u не собрался (ошибка %d)\n",
                        v, (int)snap.error());
            rc = 1;
            continue;
        }

        char path[512];
        if (allViews) {
            const char* nm[4] = { "north", "east", "south", "west" };
            std::snprintf(path, sizeof(path), "%.*s_%s.png",
                          (int)(std::strrchr(out, '.') - out), out, nm[v]);
        } else {
            std::snprintf(path, sizeof(path), "%s", out);
        }

        if (!render::writePngFile(path, snap.pixels().data(),
                                  snap.width(), snap.height())) {
            std::printf("isocheck: PNG не записан\n");
            rc = 1;
            continue;
        }

        // Сколько картинки занято территорией: пустой снимок —
        // это чёрный прямоугольник, и отличить его от полного надо
        // числом, а не глазами.
        usize drawn = 0;
        const auto& px8 = snap.pixels();
        for (usize i = 0; i + 2 < px8.size(); i += 3)
            if (px8[i] > 16 || px8[i + 1] > 20 || px8[i + 2] > 26) ++drawn;
        const usize total = (usize)snap.width() * snap.height();

        std::printf("isocheck: вид %u -> %s, %ux%u, высоты %d..%d, "
                    "территории %.1f%%, %.0f мс\n",
                    v, path, snap.width(), snap.height(),
                    snap.yMin(), snap.yMax(),
                    100.0 * (double)drawn / (double)(total ? total : 1), ms);
        if (drawn * 4 < total) {
            std::printf("isocheck: картинка почти пуста — рисовать было нечем\n");
            rc = 1;
        }

        // ---- Растения: есть и не срезаны сверху ----
        //
        // Верх кадра считается по коробке области, и растения обязаны
        // в неё поместиться: макушка выше yMax ушла бы за край
        // картинки.
        //
        // Сверка — с самим миром, а не с числом «побольше нуля»: в
        // области над морем растений нет, и снимок без них верен.
        // Сколько их стоит в области (тем же отбором, что у меширования
        // чанка), столько снимок и обязан показать — за вычетом тех,
        // что у кромки не помещаются целиком: их немного.
        if (nature) {
            u32 standing = 0;
            const auto& A = req.area;
            const render::iso::ChunkRange cr =
                render::iso::chunkRange(A, world::CHUNK_SIZE, 0);
            for (i32 cz = cr.z0; cz <= cr.z1; ++cz)
                for (i32 cx = cr.x0; cx <= cr.x1; ++cx) {
                    auto ch = world->findChunk(cx, cz);
                    if (!ch) continue;
                    auto voxel = [&](i32 x, i32 y, i32 z) -> u16 {
                        return (u32)y < (u32)world::CHUNK_SIZE_Y
                             ? ch->voxels[world::chunkIndex(x, y, z)] : world::AIR;
                    };
                    const glm::vec3 org{ (f32)(cx * world::CHUNK_SIZE), 0.f,
                                         (f32)(cz * world::CHUNK_SIZE) };
                    for (const auto& f : ch->flora) {
                        if (!world::floraStillStands(f, voxel)) continue;
                        const glm::vec3 b = render::FloraBatch::basePosition(org, f);
                        if (b.x >= (f32)A.x0 && b.x < (f32)A.x1() &&
                            b.z >= (f32)A.z0 && b.z < (f32)A.z1()) ++standing;
                    }
                }
            std::printf("isocheck:   растений на снимке %u из %u стоящих в области, "
                        "макушка %.1f при верхе кадра %d\n",
                        snap.floraCount(), standing, (double)snap.floraTop(), snap.yMax());
            if (snap.floraCount() > standing ||
                (double)snap.floraCount() < 0.8 * (double)standing) {
                std::printf("isocheck: растения на снимке не сходятся с миром\n");
                rc = 1;
            }
            if (snap.floraTop() > (f32)snap.yMax()) {
                std::printf("isocheck: макушки растений выше кадра\n");
                rc = 1;
            }
        }

        // ---- Швов на стыке тайлов нет ----
        //
        // Большая картинка рисуется по частям, и если окно в
        // координатах отсечения посчитано хоть на полпикселя не так,
        // на границе тайла появится шов. Глазами он заметен не
        // всегда, а числом — всегда: на стыке соседние столбцы
        // разойдутся сильнее, чем расходятся столбцы внутри тайла.
        const u32 W = snap.width(), H = snap.height();
        if (W > render::IsoSnapshot::TILE) {
            auto colDiff = [&](u32 x) {
                if (x == 0 || x >= W) return 0.0;
                double sum = 0.0;
                for (u32 y = 0; y < H; ++y) {
                    const usize a = ((usize)y * W + x - 1) * 3;
                    const usize b = ((usize)y * W + x) * 3;
                    for (int k = 0; k < 3; ++k)
                        sum += std::abs((int)px8[a + k] - (int)px8[b + k]);
                }
                return sum / (double)(H * 3);
            };

            // Обычный разброс между соседними столбцами.
            double typical = 0.0;
            u32 n2 = 0;
            for (u32 x = 8; x + 8 < W; x += 37) {
                if (x % render::IsoSnapshot::TILE < 4) continue;
                typical += colDiff(x);
                ++n2;
            }
            typical /= (double)(n2 ? n2 : 1);

            double worstSeam = 0.0;
            u32 worstAt = 0;
            for (u32 x = render::IsoSnapshot::TILE; x < W;
                 x += render::IsoSnapshot::TILE) {
                const double d = colDiff(x);
                if (d > worstSeam) { worstSeam = d; worstAt = x; }
            }
            std::printf("isocheck:   стыки тайлов: худший на x=%u даёт %.2f "
                        "при обычном разбросе %.2f\n",
                        worstAt, worstSeam, typical);
            // Шов — это разрыв, то есть разброс В РАЗЫ больше обычного.
            if (worstSeam > typical * 3.0 + 4.0) {
                std::printf("isocheck: на стыке тайлов виден шов\n");
                rc = 1;
            }
        }
    }

    std::printf("isocheck: сообщений слоя проверки: %d\n", layerMsgs);
    if (layerMsgs) rc = 1;

    snap.destroy();
    world.reset();
    jobs::gJobs.stop();
    vkDestroyDevice(dev, nullptr);
    return rc;
}
