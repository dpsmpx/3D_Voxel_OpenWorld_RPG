// ============================================================
// tools/gpubench/gpubench.cpp — сколько стоит полноэкранный проход.
//
// Зачем. Журнал с устройства говорит, что кадр рисуется двадцать
// миллисекунд и что это время НЕ зависит от геометрии: в
// диагностической сборке 1884 индекса и в обычной 154380 дают
// одинаковые девятнадцать с лишним. Значит кадр упирается не в
// вершины и не в число вызовов отрисовки, а в фрагменты. А самый
// большой фрагментный проход в кадре — небо: оно рисуется на весь
// экран, первым, до всей остальной сцены.
//
// Здесь этот проход меряется настоящим шейдером на настоящем
// конвейере, в разрешении телефона.
//
// Оговорка, без которой числа отсюда вводят в заблуждение: считает
// llvmpipe, программный растеризатор на процессоре. Абсолютные
// миллисекунды к телефону не переносятся НИКАК. Переносится другое —
// отношение между проходами и доля, которую съедает лишняя работа.
// Именно за этим сюда и ходят.
//
//   ./tools/gpubench/run.sh
//   ./tools/gpubench/run.sh --size 2306 1080 --iters 8
//   ./tools/gpubench/run.sh --cloud 0.6 --sun -0.3   # небо с тучами, ночь
//
// Небо меряется по умолчанию при ясной погоде и дневном солнце. Цена
// шейдера неба от погоды и суток зависит (тучи считаются только при
// облачности), поэтому их можно задать: --cloud (доля неба под
// тучами), --rain (сила осадков), --sun (высота солнца, −1..1).
// --sky-only меряет одно небо — для сравнения «до/после».
// ============================================================
#include "core/types.h"
#include <vulkan/vulkan.h>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;

#define VKOK(x) do { VkResult r_ = (x); if (r_ != VK_SUCCESS) { \
    std::printf("gpubench: %s -> %d (строка %d)\n", #x, (int)r_, __LINE__); \
    std::exit(1); } } while (0)

/// Каталог с шейдерами ИГРЫ, собранными для этого запуска. По
/// умолчанию — каталог сборки инструмента, а не поставка: собирать
/// шейдеры прямо в app/src/main/assets значит подложить их в APK.
/// Ровно так и вышло однажды: APK уехал на устройство со старым
/// voxel.frag, оставленным там предыдущим прогоном инструмента, и
/// замер на устройстве молча повторил прежние числа.
std::string g_assets = "build/gpubench/assets/";
/// Каталог опорных шейдеров самого бенчмарка. Отдельно от ассетов
/// игры: класть туда своё — значит отправить это в APK.
std::string g_benchAssets = "build/gpubench/";

std::vector<char> readFile(const std::string& rel, bool bench = false) {
    const std::string path = (bench ? g_benchAssets : g_assets) + rel;
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) { std::printf("gpubench: нет файла %s\n", path.c_str()); std::exit(1); }
    std::fseek(f, 0, SEEK_END);
    const long n = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    std::vector<char> data((usize)n);
    if (std::fread(data.data(), 1, (usize)n, f) != (usize)n) { std::fclose(f); std::exit(1); }
    std::fclose(f);
    return data;
}

VkShaderModule loadShader(VkDevice dev, const std::string& rel, bool bench = false) {
    auto code = readFile(rel, bench);
    VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    ci.codeSize = code.size();
    ci.pCode    = reinterpret_cast<const uint32_t*>(code.data());
    VkShaderModule m;
    VKOK(vkCreateShaderModule(dev, &ci, nullptr, &m));
    return m;
}

/// Тот же набор, что у игры: одна униформа камеры на set 0, binding 0.
struct CameraUbo {
    glm::mat4 viewProj;
    glm::mat4 invViewProj;
    glm::vec4 cameraPos;
    glm::vec4 screenSize;
    glm::vec4 sunDir;
    glm::vec4 fogParams;
    glm::vec4 skyColor;
    glm::vec4 sunLight;
    glm::vec4 ambLight;
    glm::vec4 skyLinear;
    glm::vec4 weather;
    glm::vec4 wind;
    glm::vec4 lightInfo;
    glm::vec4 lightPos[8];
    glm::vec4 lightColor[8];
};

u32 findMem(VkPhysicalDevice phys, u32 bits, VkMemoryPropertyFlags want) {
    VkPhysicalDeviceMemoryProperties mp;
    vkGetPhysicalDeviceMemoryProperties(phys, &mp);
    for (u32 i = 0; i < mp.memoryTypeCount; ++i)
        if ((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & want) == want)
            return i;
    std::printf("gpubench: нет подходящей памяти\n");
    std::exit(1);
}

} // namespace

int main(int argc, char** argv) {
    u32 W = 2306, H = 1080;      // разрешение телефона из журнала
    int iters = 8;               // проходов на замер
    float cloud = 0.f, rain = 0.f, sunY = 0.6f;
    bool skyOnly = false;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&]() -> const char* { return (i + 1 < argc) ? argv[++i] : "0"; };
        if      (a == "--size")   { W = (u32)atoi(next()); H = (u32)atoi(next()); }
        else if (a == "--iters")  iters = atoi(next());
        else if (a == "--assets")       g_assets = std::string(next()) + "/";
        else if (a == "--bench-assets") g_benchAssets = std::string(next()) + "/";
        else if (a == "--cloud")    cloud = (float)atof(next());
        else if (a == "--rain")     rain = (float)atof(next());
        else if (a == "--sun")      sunY = (float)atof(next());
        else if (a == "--sky-only") skyOnly = true;
    }

    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "gpubench";
    app.apiVersion       = VK_API_VERSION_1_1;
    VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ici.pApplicationInfo = &app;
    VkInstance inst;
    VKOK(vkCreateInstance(&ici, nullptr, &inst));

    u32 n = 0;
    vkEnumeratePhysicalDevices(inst, &n, nullptr);
    if (!n) { std::printf("gpubench: нет устройств Vulkan\n"); return 1; }
    std::vector<VkPhysicalDevice> devs(n);
    vkEnumeratePhysicalDevices(inst, &n, devs.data());
    VkPhysicalDevice phys = devs[0];
    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(phys, &props);

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
    VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    dci.queueCreateInfoCount = 1; dci.pQueueCreateInfos = &qci;
    VkDevice dev;
    VKOK(vkCreateDevice(phys, &dci, nullptr, &dev));
    VkQueue queue;
    vkGetDeviceQueue(dev, fam, 0, &queue);

    std::printf("gpubench: %s, кадр %ux%u (%.2f Мпикс), проходов на замер %d\n",
                props.deviceName, W, H, (double)W * H / 1e6, iters);
    std::printf("          ВНИМАНИЕ: считает процессор (llvmpipe). Абсолютные\n"
                "          миллисекунды к телефону не переносятся; переносится\n"
                "          отношение между проходами.\n\n");

    // ---- цель вывода ----
    VkImage color; VkDeviceMemory colorMem; VkImageView colorView;
    {
        VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        ci.imageType = VK_IMAGE_TYPE_2D;
        ci.format    = VK_FORMAT_R8G8B8A8_UNORM;
        ci.extent    = { W, H, 1 };
        ci.mipLevels = 1; ci.arrayLayers = 1;
        ci.samples   = VK_SAMPLE_COUNT_1_BIT;
        ci.tiling    = VK_IMAGE_TILING_OPTIMAL;
        ci.usage     = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        VKOK(vkCreateImage(dev, &ci, nullptr, &color));
        VkMemoryRequirements mr; vkGetImageMemoryRequirements(dev, color, &mr);
        VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        ai.allocationSize = mr.size;
        ai.memoryTypeIndex = findMem(phys, mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        VKOK(vkAllocateMemory(dev, &ai, nullptr, &colorMem));
        VKOK(vkBindImageMemory(dev, color, colorMem, 0));
        VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vi.image = color; vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vi.format = ci.format;
        vi.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        VKOK(vkCreateImageView(dev, &vi, nullptr, &colorView));
    }

    // ---- проход рендера: только цвет, без глубины ----
    // Меряем стоимость ФРАГМЕНТОВ, а глубина к ней ничего не
    // добавляет: у неба она всё равно выключена.
    VkRenderPass rp;
    {
        VkAttachmentDescription at{};
        at.format = VK_FORMAT_R8G8B8A8_UNORM;
        at.samples = VK_SAMPLE_COUNT_1_BIT;
        at.loadOp  = VK_ATTACHMENT_LOAD_OP_CLEAR;
        at.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        at.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        at.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        at.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        at.finalLayout   = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        VkAttachmentReference ref{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
        VkSubpassDescription sp{};
        sp.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        sp.colorAttachmentCount = 1; sp.pColorAttachments = &ref;
        VkRenderPassCreateInfo ci{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
        ci.attachmentCount = 1; ci.pAttachments = &at;
        ci.subpassCount = 1; ci.pSubpasses = &sp;
        VKOK(vkCreateRenderPass(dev, &ci, nullptr, &rp));
    }
    VkFramebuffer fb;
    {
        VkFramebufferCreateInfo ci{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
        ci.renderPass = rp; ci.attachmentCount = 1; ci.pAttachments = &colorView;
        ci.width = W; ci.height = H; ci.layers = 1;
        VKOK(vkCreateFramebuffer(dev, &ci, nullptr, &fb));
    }

    // ---- набор дескрипторов с камерой ----
    VkBuffer ubo; VkDeviceMemory uboMem;
    {
        VkBufferCreateInfo ci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        ci.size = sizeof(CameraUbo);
        ci.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
        ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        VKOK(vkCreateBuffer(dev, &ci, nullptr, &ubo));
        VkMemoryRequirements mr; vkGetBufferMemoryRequirements(dev, ubo, &mr);
        VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        ai.allocationSize = mr.size;
        ai.memoryTypeIndex = findMem(phys, mr.memoryTypeBits,
                                     VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                     VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        VKOK(vkAllocateMemory(dev, &ai, nullptr, &uboMem));
        VKOK(vkBindBufferMemory(dev, ubo, uboMem, 0));

        // Камера смотрит на горизонт в ясный день: небо считает всё,
        // что умеет, — и градиент, и полосу у горизонта, и диск.
        CameraUbo c{};
        const glm::vec3 eye{ 0.f, 40.f, 0.f };
        glm::mat4 proj = glm::perspective(glm::radians(70.f), (f32)W / (f32)H, 0.1f, 512.f);
        proj[1][1] *= -1.f;
        glm::mat4 view = glm::lookAt(eye, eye + glm::vec3(0.f, -0.05f, -1.f),
                                     glm::vec3(0.f, 1.f, 0.f));
        c.viewProj    = proj * view;
        c.invViewProj = glm::inverse(c.viewProj);
        c.cameraPos   = glm::vec4(eye, 1.f);
        c.screenSize  = glm::vec4((f32)W, (f32)H, 0.f, 0.f);
        c.sunDir      = glm::vec4(glm::normalize(glm::vec3(0.4f, sunY, 0.7f)), 1.f);
        c.fogParams   = glm::vec4(120.f, 224.f, 0.5f, 0.f);
        // Доля дня — по высоте солнца, как у world::DayCycle: ночью в
        // небе считаются звёзды и луна, днём — нет.
        const f32 dayFrac = glm::smoothstep(-0.25f, 0.10f, glm::normalize(glm::vec3(0.4f, sunY, 0.7f)).y);
        c.skyColor    = glm::vec4(glm::mix(glm::vec3(0.03f, 0.04f, 0.10f),
                                           glm::vec3(0.42f, 0.62f, 0.92f), dayFrac), dayFrac);

        // Свет суток — теми же выражениями, что в render::Camera::toUbo.
        // Инструмент гоняет НАСТОЯЩИЕ шейдеры игры: разойдись эта
        // копия с игровой — они читали бы чужие байты и мерили мусор,
        // молча и с правдоподобными числами. За совпадением следит
        // тест «мир освещён по одной модели».
        {
            auto toLin = [](const glm::vec3& v) { return v * v; };
            const glm::vec3 sd{ c.sunDir };
            const f32 day   = c.skyColor.w;
            const f32 above = glm::smoothstep(-0.10f, 0.06f, sd.y);
            const glm::vec3 sunTint =
                toLin(glm::mix(glm::vec3(1.00f, 0.52f, 0.26f),
                               glm::vec3(1.00f, 0.97f, 0.92f),
                               glm::smoothstep(0.f, 0.30f, sd.y)));
            const glm::vec3 skyLin = toLin(glm::vec3(c.skyColor));
            const f32 skyMax = glm::max(glm::max(skyLin.r, skyLin.g),
                                        glm::max(skyLin.b, 0.001f));
            const glm::vec3 ambTint =
                glm::mix(glm::vec3(1.f), skyLin / skyMax, 0.55f);
            c.sunLight  = glm::vec4(sunTint, day * above);
            c.ambLight  = glm::vec4(ambTint, glm::mix(0.14f, 0.60f, day));
            c.skyLinear = glm::vec4(skyLin, above);
            // Погода: по умолчанию ясно; облачность и осадки задаются
            // ключами — цена неба от них зависит.
            c.weather   = glm::vec4(cloud, rain, 0.f, 0.f);
            c.wind      = glm::vec4(2.f, 1.f, 30.f, 0.f);
        }
        void* p = nullptr;
        VKOK(vkMapMemory(dev, uboMem, 0, sizeof(CameraUbo), 0, &p));
        std::memcpy(p, &c, sizeof(CameraUbo));
        vkUnmapMemory(dev, uboMem);
    }

    VkDescriptorSetLayout dsl;
    {
        VkDescriptorSetLayoutBinding b{};
        b.binding = 0; b.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        b.descriptorCount = 1;
        b.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
        VkDescriptorSetLayoutCreateInfo ci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        ci.bindingCount = 1; ci.pBindings = &b;
        VKOK(vkCreateDescriptorSetLayout(dev, &ci, nullptr, &dsl));
    }
    VkDescriptorPool pool; VkDescriptorSet dset;
    {
        VkDescriptorPoolSize ps{ VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1 };
        VkDescriptorPoolCreateInfo ci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        ci.maxSets = 1; ci.poolSizeCount = 1; ci.pPoolSizes = &ps;
        VKOK(vkCreateDescriptorPool(dev, &ci, nullptr, &pool));
        VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        ai.descriptorPool = pool; ai.descriptorSetCount = 1; ai.pSetLayouts = &dsl;
        VKOK(vkAllocateDescriptorSets(dev, &ai, &dset));
        VkDescriptorBufferInfo bi{ ubo, 0, sizeof(CameraUbo) };
        VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        w.dstSet = dset; w.dstBinding = 0; w.descriptorCount = 1;
        w.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER; w.pBufferInfo = &bi;
        vkUpdateDescriptorSets(dev, 1, &w, 0, nullptr);
    }

    VkPipelineLayout plo;
    {
        VkPipelineLayoutCreateInfo ci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        ci.setLayoutCount = 1; ci.pSetLayouts = &dsl;
        VKOK(vkCreatePipelineLayout(dev, &ci, nullptr, &plo));
    }

    // Конвейер полноэкранного треугольника из готовых модулей.
    auto makeFullscreen = [&](VkShaderModule vs, VkShaderModule fs) {
        VkPipelineShaderStageCreateInfo st[2]{};
        st[0] = { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO };
        st[0].stage = VK_SHADER_STAGE_VERTEX_BIT; st[0].module = vs; st[0].pName = "main";
        st[1] = { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO };
        st[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT; st[1].module = fs; st[1].pName = "main";

        VkPipelineVertexInputStateCreateInfo vi{
            VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
        VkPipelineInputAssemblyStateCreateInfo ia{
            VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
        ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        VkViewport vp{ 0.f, 0.f, (f32)W, (f32)H, 0.f, 1.f };
        VkRect2D sc{ {0, 0}, { W, H } };
        VkPipelineViewportStateCreateInfo vps{
            VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
        vps.viewportCount = 1; vps.pViewports = &vp;
        vps.scissorCount = 1; vps.pScissors = &sc;
        VkPipelineRasterizationStateCreateInfo rs{
            VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
        rs.polygonMode = VK_POLYGON_MODE_FILL;
        rs.cullMode = VK_CULL_MODE_NONE;
        rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
        rs.lineWidth = 1.f;
        VkPipelineMultisampleStateCreateInfo ms{
            VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
        ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
        VkPipelineColorBlendAttachmentState cba{};
        cba.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                             VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
        VkPipelineColorBlendStateCreateInfo cb{
            VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
        cb.attachmentCount = 1; cb.pAttachments = &cba;
        VkGraphicsPipelineCreateInfo ci{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
        ci.stageCount = 2; ci.pStages = st;
        ci.pVertexInputState = &vi; ci.pInputAssemblyState = &ia;
        ci.pViewportState = &vps; ci.pRasterizationState = &rs;
        ci.pMultisampleState = &ms; ci.pColorBlendState = &cb;
        ci.layout = plo; ci.renderPass = rp; ci.subpass = 0;
        VkPipeline p;
        VKOK(vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &ci, nullptr, &p));
        return p;
    };

    VkShaderModule skyVs  = loadShader(dev, "shaders/sky.vert.spv");
    VkShaderModule skyFs  = loadShader(dev, "shaders/sky.frag.spv");
    VkShaderModule flatFs = loadShader(dev, "gpubench_flat.frag.spv", true);
    // Воксельный фрагментный шейдер — настоящий, из игры; вершинный к
    // нему подаёт те же входы, что даёт voxel.vert (см. voxel_probe.vert).
    VkShaderModule voxVs  = loadShader(dev, "gpubench_voxel_probe.vert.spv", true);
    VkShaderModule watVs  = loadShader(dev, "gpubench_water_probe.vert.spv", true);
    VkShaderModule voxFs  = loadShader(dev, "shaders/voxel.frag.spv");
    VkPipeline pSky   = makeFullscreen(skyVs, skyFs);
    VkPipeline pFlat  = makeFullscreen(skyVs, flatFs);
    VkPipeline pVoxel = makeFullscreen(voxVs, voxFs);
    // Тот же фрагментный шейдер, но с полупрозрачным цветом грани:
    // меряем цену ветки воды, а не догадываемся о ней.
    VkPipeline pWater = makeFullscreen(watVs, voxFs);

    VkCommandPool cp;
    {
        VkCommandPoolCreateInfo ci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        ci.queueFamilyIndex = fam;
        ci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        VKOK(vkCreateCommandPool(dev, &ci, nullptr, &cp));
    }
    VkCommandBuffer cmd;
    {
        VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        ai.commandPool = cp; ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        ai.commandBufferCount = 1;
        VKOK(vkAllocateCommandBuffers(dev, &ai, &cmd));
    }
    VkFence fence;
    {
        VkFenceCreateInfo ci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        VKOK(vkCreateFence(dev, &ci, nullptr, &fence));
    }

    // Один замер: iters проходов в одной отправке, ждём забор.
    auto measure = [&](VkPipeline pipe, int passes) {
        VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        VKOK(vkBeginCommandBuffer(cmd, &bi));
        VkClearValue clear{};
        clear.color = { { 0.f, 0.f, 0.f, 1.f } };
        for (int i = 0; i < passes; ++i) {
            VkRenderPassBeginInfo rbi{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
            rbi.renderPass = rp; rbi.framebuffer = fb;
            rbi.renderArea = { {0, 0}, { W, H } };
            rbi.clearValueCount = 1; rbi.pClearValues = &clear;
            vkCmdBeginRenderPass(cmd, &rbi, VK_SUBPASS_CONTENTS_INLINE);
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, plo,
                                    0, 1, &dset, 0, nullptr);
            vkCmdDraw(cmd, 3, 1, 0, 0);
            vkCmdEndRenderPass(cmd);
        }
        VKOK(vkEndCommandBuffer(cmd));
        vkResetFences(dev, 1, &fence);
        VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        si.commandBufferCount = 1; si.pCommandBuffers = &cmd;
        const auto t0 = Clock::now();
        VKOK(vkQueueSubmit(queue, 1, &si, fence));
        VKOK(vkWaitForFences(dev, 1, &fence, VK_TRUE, UINT64_MAX));
        const double ms =
            std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
        return ms / (double)passes;
    };

    // Прогрев: первая отправка тащит за собой компиляцию и раскладку.
    measure(pFlat, 1);
    measure(pSky, 1);
    measure(pVoxel, 1);
    measure(pWater, 1);

    double best_flat = 1e9, best_sky = 1e9, best_vox = 1e9, best_wat = 1e9;
    for (int r = 0; r < (skyOnly ? 5 : 3); ++r) {
        const double f = measure(pFlat, iters);
        const double s = measure(pSky, iters);
        if (f < best_flat) best_flat = f;
        if (s < best_sky)  best_sky  = s;
        if (skyOnly) continue;
        const double v = measure(pVoxel, iters);
        const double w = measure(pWater, iters);
        if (v < best_vox)  best_vox  = v;
        if (w < best_wat)  best_wat  = w;
    }

    const double px = (double)W * H;
    std::printf("полноэкранный проход на %ux%u:\n", W, H);
    std::printf("  ровный цвет (растр и запись)   %8.2f мс   %6.2f нс/пиксель\n",
                best_flat, best_flat * 1e6 / px);
    std::printf("  шейдер неба                    %8.2f мс   %6.2f нс/пиксель\n",
                best_sky, best_sky * 1e6 / px);
    std::printf("  из них собственно небо         %8.2f мс   (%.1fx к ровному цвету)\n",
                best_sky - best_flat, best_flat > 0 ? best_sky / best_flat : 0.0);
    std::printf("  (облачность %.2f, осадки %.2f, солнце на высоте %.2f)\n",
                (double)cloud, (double)rain, (double)sunY);
    if (skyOnly) { vkDeviceWaitIdle(dev); return 0; }
    std::printf("  шейдер вокселей                %8.2f мс   %6.2f нс/пиксель\n",
                best_vox, best_vox * 1e6 / px);
    std::printf("  из них собственно воксели      %8.2f мс   (%.1fx к ровному цвету)\n",
                best_vox - best_flat, best_flat > 0 ? best_vox / best_flat : 0.0);
    std::printf("  он же на грани воды            %8.2f мс   %6.2f нс/пиксель\n",
                best_wat, best_wat * 1e6 / px);
    std::printf("  из них ветка блика и Френеля   %8.2f мс   (+%.0f%% к грани террейна)\n",
                best_wat - best_vox,
                best_vox > best_flat
                    ? (best_wat - best_vox) / (best_vox - best_flat) * 100.0 : 0.0);

    // Главное число: журнал с устройства и кадр vkcheck говорят, что
    // геометрия закрывает 64% экрана. Ровно эта доля работы неба и
    // выбрасывается, пока небо рисуется ПЕРВЫМ.
    // Террейн закрывает под две трети экрана (замер кадра vkcheck:
    // 323777 пикселей геометрии из 504000), небо — остальное.
    const double covered = 0.64;
    std::printf("\n  кадр как он есть: %.0f%% экрана воксели, %.0f%% небо\n",
                covered * 100.0, (1.0 - covered) * 100.0);
    std::printf("    воксели                      %8.2f мс\n",
                (best_vox - best_flat) * covered);
    std::printf("    небо                         %8.2f мс\n",
                (best_sky - best_flat) * (1.0 - covered));

    vkDeviceWaitIdle(dev);
    return 0;
}
