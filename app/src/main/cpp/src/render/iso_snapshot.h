/**
 * @file iso_snapshot.h
 * @brief Изометрический снимок мира: подготовка, рендер, картинка.
 *
 * ============================================================
 * ПОЧЕМУ ОТДЕЛЬНЫЙ РЕНДЕР, А НЕ ОРТО-КАМЕРА В ИГРОВОМ
 * ============================================================
 *
 * Игровой путь кадра приварен к цепочке показа: `vk::Context` владеет
 * командным буфером кадра, кольцом из двух кадров в работе, проходом
 * рендера под формат экрана и вьюпортом от его размера. Снимок на
 * четыре тысячи пикселей в этот путь не помещается: пришлось бы либо
 * пересоздавать цепочку показа (то есть рвать игровой кадр), либо
 * протаскивать «цель рендера» через RenderSystem, ChunkRenderer и
 * каждый проход. Второе — большая правка в коде, который рисует
 * КАЖДЫЙ кадр, ради возможности, которой пользуются раз в час.
 *
 * Поэтому снимок владеет своей целью. Но НИЧЕМ, кроме цели: всё
 * содержательное берётся готовым и ровно то же, что в игре —
 *
 *   * `world::ChunkManager::getChunk` — настоящая генерация мира;
 *   * `world::buildGreedyMesh`        — тот же жадный мешер;
 *   * `render::buildChunkVertices`    — та же упаковка вершины;
 *   * `render::voxelPipelineDesc`     — то же описание конвейера;
 *   * `shaders/voxel.vert|frag`       — те же шейдеры и материалы;
 *   * `render::CameraUbo`             — тот же блок формы;
 *   * `vk::createVoxelRenderPass`     — тот же проход рендера;
 *   * `world::Chunk::flora`            — те же растения, отобранные
 *                                        `floraStillStands`, как при
 *                                        мешировании чанка;
 *   * `render::buildFloraMeshes`,
 *     `render::FloraBatch`,
 *     `render::floraPipelineDesc`     — те же модели, экземпляры и
 *                                        конвейер, что у FloraRenderer.
 *
 * Своего здесь: ортографическая камера (iso_projection), цель
 * рендера с тайлами, подготовка территории и запись PNG.
 *
 * ============================================================
 * ЗАВИСИМОСТЬ ТОЛЬКО ОТ РУЧЕК VULKAN
 * ============================================================
 *
 * Класс не знает ни про `vk::Context`, ни про окно, ни про цепочку
 * показа: ему дают устройство, очередь и семейство. Это не
 * украшение — так тот же код гоняется на хосте под программным
 * Vulkan (tools/isocheck), и снимок можно посмотреть глазами и
 * измерить числами, не имея телефона под рукой.
 *
 * ============================================================
 * ЧТО ПОПАДАЕТ В КАРТИНКУ
 * ============================================================
 *
 * Территория: ландшафт, растения и мелкие природные вещи мира, вода.
 * Сущностей здесь нет не потому, что их отключили, а потому, что этот
 * рендер о них не знает вовсе: он читает только чанки мира — воксели
 * и их Chunk::flora — и рисует ровно три прохода: непрозрачный
 * ландшафт, растения, воду. Ни игрока, ни мобов, ни NPC, ни
 * предметов, ни снарядов, ни неба, ни интерфейса, ни осадков.
 * Выключать нечего.
 *
 * Растения на снимке — те же, что в игре, и стоят там же: список чанка
 * не пересобирается, а отбирается по вокселям ровно как при
 * мешировании. Своего два правила. Дальности нет: при ортографии блок
 * везде одного размера, и ступень детальности выбирается по размеру
 * вокселя в пикселях (FloraBatch::beginOrtho), а не по расстоянию до
 * камеры. И растение, которое не помещается в область целиком — крона
 * свешивается за кромку, — не рисуется: кадр кроится по коробке
 * области, и свес за неё был бы обрезан краем картинки.
 *
 * Свет закреплён (см. `fixedLightUbo`): два снимка одного мира,
 * сделанные днём и ночью, обязаны совпасть.
 */
#pragma once
#include "../core/types.h"
#include "../core/math.h"
#include "../vk/vk_buffer.h"
#include "../vk/vk_descriptors.h"
#include "../vk/vk_pipeline.h"
#include "../vk/vk_shader.h"
#include "../world/chunk_manager.h"
#include "camera.h"
#include "flora_batch.h"
#include "flora_models.h"
#include "mesh_builder.h"
#include "iso_projection.h"
#include "mesh_table.h"
#include <memory>
#include <string>
#include <vector>
#include <vulkan/vulkan.h>

struct AAssetManager;

namespace render {

/// Что просим снять.
struct IsoRequest {
    iso::Area area;
    iso::View view = iso::View::North;
    /// Пикселей на один блок мира вдоль оси экрана.
    f32  pixelsPerBlock = 16.f;
    /// Потолок стороны итоговой картинки. При превышении уменьшается
    /// масштаб, а не область: игрок просил территорию, а не пиксели.
    u32  maxSide = 8192;
    /// Растения и мелкие природные вещи. Выключаются только проверкой:
    /// без них снимок обязан совпасть с прежним голым ландшафтом.
    bool nature = true;
};

/// Где сейчас процесс. Порядок — порядок прохождения.
enum class IsoStage : u8 {
    Idle,        ///< ничего не начато
    Preparing,   ///< догружаем и генерируем чанки
    Meshing,     ///< строим меши территории
    Rendering,   ///< рисуем тайлы
    Ready,       ///< картинка готова
    Failed,      ///< не вышло; см. error()
};

/// Почему не вышло. Текст показывает интерфейс, поэтому это ключи, а
/// не строки: перевод живёт в таблице строк.
enum class IsoError : u8 {
    None = 0,
    OutOfMemory,     ///< не хватило памяти под цель или буферы
    Generation,      ///< мир не выдал чанки за отведённое время
    RenderTarget,    ///< Vulkan не дал цель рендера
    TooLarge,        ///< область не влезает ни в какие разумные пределы
};

class IsoSnapshot {
public:
    /// Сколько блоков territории берём по умолчанию.
    static constexpr i32 DEFAULT_SIZE = 100;
    /// Пределы размера области, блоков.
    static constexpr i32 MIN_SIZE = 16;
    /// Верхняя граница — не вкус, а память: 512 блоков это 16x16
    /// чанков, их меши в видеопамяти и картинка в оперативной.
    static constexpr i32 MAX_SIZE = 512;

    /// Сторона тайла. Картинка больше этого рисуется по частям:
    /// цель рендера в 1024x1024xRGBA — это четыре мегабайта, и
    /// столько же занимает глубина, сколько бы ни просили пикселей.
    static constexpr u32 TILE = 1024;

    /// Сколько блоков слоя берём НИЖЕ самой низкой поверхности.
    /// Камера смотрит под углом, и у ближнего края слоя видна его
    /// боковая стенка: без запаса она обрубается по линии.
    static constexpr i32 SKIRT_BELOW = 8;

    bool init(VkDevice dev, VkPhysicalDevice phys, VkQueue queue,
              u32 queueFamily, VkFormat depthFormat, AAssetManager* assets);
    void destroy();

    /// Начать снимок. Прежний результат сбрасывается.
    void begin(const IsoRequest& req);
    /// Бросить всё и вернуться в Idle.
    void cancel();

    /// Один шаг. Зовётся из игрового цикла раз в кадр: подготовка
    /// территории и рендер тайлов не должны съедать кадр целиком.
    /// @return true, если что-то сделано (есть смысл звать ещё).
    bool step(world::ChunkManager& world);

    /// Начать и довести снимок до конца прямо сейчас, не возвращаясь
    /// в цикл кадров.
    ///
    /// Для превью мира: его снимают при выгрузке мира, когда кадров
    /// после этого уже не будет. Путь ровно тот же — begin и step, —
    /// меняется только то, кто их зовёт. Пока мир догружает чанки,
    /// поток спит по миллисекунде: генерация идёт фоновыми задачами,
    /// и крутить пустые шаги значило бы съесть их процессор.
    ///
    /// @return true — картинка готова (pixels()); false — не вышло
    ///         или не уложились в `maxSeconds`, заказ снят.
    bool runNow(const IsoRequest& req, world::ChunkManager& world,
                f32 maxSeconds);

    /// Перерисовать УЖЕ подготовленную территорию под другой вид или
    /// другое разрешение, не пересобирая меши. Так работает смена
    /// стороны обзора в предпросмотре.
    bool rerender(const IsoRequest& req);

    IsoStage stage() const { return stage_; }
    IsoError error() const { return error_; }
    /// Доля готовности, 0..1. Считается по этапам, а не выдумывается.
    f32 progress() const;

    /// Готовая картинка: RGB8, построчно сверху вниз.
    const std::vector<u8>& pixels() const { return pixels_; }
    u32 width()  const { return outW_; }
    u32 height() const { return outH_; }

    /// Что именно снимали — для имени файла и для подписи.
    const IsoRequest& request() const { return req_; }
    /// Диапазон высот, который вошёл в кадр. Считается по вокселям.
    i32 yMin() const { return yMin_; }
    i32 yMax() const { return yMax_; }
    /// Растений на снимке — сколько попало в область целиком.
    u32 floraCount() const { return floraCount_; }
    /// Высшая точка растительности, блоков; ниже yMax по построению.
    f32 floraTop() const { return floraTop_; }

    /// Блок формы с ЗАКРЕПЛЁННЫМ светом.
    ///
    /// Открыт наружу затем, что именно его проверяет хостовая
    /// проверка: «снимок не зависит от времени суток» — утверждение о
    /// числах в этом блоке, и сверять его надо с ними, а не с
    /// картинкой.
    /// Зерно крапчатости — параметром, а не полем: свет у снимка
    /// закреплён, а крапчатость блоков закреплять нельзя. Она
    /// свойство МИРА, и подменять её значило бы рисовать другую
    /// землю, чем та, по которой игрок ходит.
    static CameraUbo fixedLightUbo(const iso::Camera& cam, iso::View view,
                                   f32 tintSeed = 0.f);

    /// Оставить от меша чанка только то, что в коробке снимка: в
    /// области по X и Z и не ниже yMin. Чанк шире области, если она не
    /// выровнена по чанкам, и глубже её дна. Открыто ради проверки.
    static void clipQuadsToBox(const iso::Area& area, i32 yMin,
                               world::ChunkCoord coord, std::vector<world::Quad>& quads);

private:
    /// Зерно крапчатости блоков, снятое с мира в step().
    /// См. render::tintSeedOf и комментарий в renderTile.
    f32 tintSeed_ = 0.f;

    struct Tile { u32 x, y, w, h; };
    struct ChunkMeshGpu {
        vk::Buffer vb, ib;
        u32 opaqueIndices = 0;
        u32 totalIndices  = 0;
        VkIndexType indexType = VK_INDEX_TYPE_UINT16;
        glm::vec3 origin{0.f};
        glm::vec3 blendCenter{0.f};
        bool valid = false;
        /// Растения чанка, стоящие при нынешних вокселях и целиком
        /// внутри области, и их общая коробка в мире.
        std::vector<world::FloraInstance> flora;
        glm::vec3 floraLo{0.f}, floraHi{0.f};
    };

    bool ensureTarget(u32 w, u32 h);
    void destroyTarget();
    bool buildOneMesh(world::ChunkManager& world, usize index);
    /// Поперечный разрез по кромке области — из настоящих блоков.
    void appendAreaWalls(const world::Chunk& chunk, world::ChunkCoord coord,
                         std::vector<world::Quad>& quads) const;
    bool renderTile(const Tile& t);
    /// Модели растений: строятся при первом снимке с растениями и
    /// живут до destroy(). Большинство игроков снимков не делает, и
    /// держать ради них меши в видеопамяти с самого старта незачем.
    bool ensureFloraModels();
    /// Растения чанка: отбор и габариты. Зовётся под замками вокселей.
    void collectFlora(const world::Chunk& chunk, ChunkMeshGpu& m);
    /// Нарисовать растения чанков, попавших в тайл.
    void drawFlora(const math::Frustum& fr);
    bool natureOn() const { return req_.nature && floraReady_; }
    void computeTiles();
    void scanHeights(world::ChunkManager& world);
    void releaseMeshes();

    VkDevice         dev_   = VK_NULL_HANDLE;
    VkPhysicalDevice phys_  = VK_NULL_HANDLE;
    VkQueue          queue_ = VK_NULL_HANDLE;
    u32              family_ = 0;
    VkFormat         depthFormat_ = VK_FORMAT_D32_SFLOAT;

    vk::ShaderCache      shaders_;
    vk::DescriptorSet    descriptors_;
    vk::GraphicsPipeline opaquePipe_, blendPipe_;
    // Растения: конвейер создаётся с остальными, модели — лениво.
    vk::GraphicsPipeline floraPipe_;
    MeshTable            floraMeshes_;
    std::vector<u32>     floraQuads_;
    std::vector<FloraExtent> floraExtents_;
    vk::Buffer           floraInst_;
    FloraBatch           floraBatch_;
    bool                 floraReady_ = false;
    u32                  floraCount_ = 0;
    f32                  floraTop_   = 0.f;
    vk::Buffer           ubo_;
    VkRenderPass         renderPass_ = VK_NULL_HANDLE;

    // Цель рендера: одна на тайл, переиспользуется.
    VkImage        colorImg_ = VK_NULL_HANDLE, depthImg_ = VK_NULL_HANDLE;
    VkDeviceMemory colorMem_ = VK_NULL_HANDLE, depthMem_ = VK_NULL_HANDLE;
    VkImageView    colorView_ = VK_NULL_HANDLE, depthView_ = VK_NULL_HANDLE;
    VkFramebuffer  fb_ = VK_NULL_HANDLE;
    VkBuffer       readBuf_ = VK_NULL_HANDLE;
    VkDeviceMemory readMem_ = VK_NULL_HANDLE;
    void*          readMapped_ = nullptr;
    u32            targetW_ = 0, targetH_ = 0;

    VkCommandPool  cmdPool_ = VK_NULL_HANDLE;
    VkCommandBuffer cmd_    = VK_NULL_HANDLE;
    VkFence        fence_   = VK_NULL_HANDLE;

    IsoRequest req_{};
    IsoStage   stage_ = IsoStage::Idle;
    IsoError   error_ = IsoError::None;

    /// Чанки территории (с рамкой в один чанк — ради граней на стыке).
    std::vector<world::ChunkCoord> needChunks_;
    /// Чанки, которые реально рисуем (без рамки).
    std::vector<world::ChunkCoord> drawChunks_;
    std::vector<ChunkMeshGpu>      meshes_;
    usize   meshCursor_ = 0;
    u32     prepareTicks_ = 0;

    iso::Camera cam_{};
    i32 yMin_ = 0, yMax_ = 0;

    std::vector<Tile> tiles_;
    usize tileCursor_ = 0;

    std::vector<u8> pixels_;
    u32 outW_ = 0, outH_ = 0;

    // Временные буферы сборки — живут между вызовами, чтобы не
    // выделять их заново на каждый чанк.
    std::vector<world::Quad>  scratchQuads_;
    std::vector<VoxelVertex>  scratchVerts_;
    std::vector<u32>          scratchIdx_;
    std::vector<u16>          scratchIdx16_;
    std::vector<u8>           tileRgba_;
};

} // namespace render
