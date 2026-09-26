/**
 * @file chunk_renderer.h
 * @brief Рендер: меширование чанков, отсечение, инстансинг, камера.
 */
#pragma once
#include "../core/types.h"
#include "../core/math.h"
#include "../vk/vk_buffer.h"
#include "../vk/vk_context.h"
#include "../vk/vk_staging_pool.h"
#include "../world/chunk_manager.h"
#include "mesh_builder.h"
#include <cmath>
#include <memory>
#include <unordered_map>
#include <vector>

namespace render {


class ChunkRenderer {
public:
    /// Сколько свежепостроенных мешей берём в один кадр. Каждый стоит
    /// сборки вершин на процессоре и копии в видеопамять; при загрузке
    /// мира их приезжает по нескольку десятков разом.
    static constexpr usize MAX_MESH_UPLOADS_PER_FRAME = 12;

    bool init(VkDevice dev, VkPhysicalDevice phys);
    void shutdown();

    /// Загружает на GPU меши только что перестроенных чанков. Все
    /// копии собираются в один пакет передачи — одна остановка GPU
    /// на кадр.
    ///
    /// Мир передаётся не ради чтения: у очереди готовых мешей нет
    /// второй попытки, и то, что не удалось выгрузить, возвращается
    /// в неё здесь же.
    void uploadChunks(vk::Context& ctx, world::ChunkManager& world,
                      const std::vector<world::MeshReady>& ready);

    void forgetChunk(world::ChunkCoord c);


    /// Отбирает видимые чанки, раскладывает их по расстоянию и рисует
    /// НЕПРОЗРАЧНУЮ часть, от ближнего к дальнему.
    ///
    /// Порядок здесь не косметика. Мобильные GPU отбрасывают
    /// закрытые фрагменты по глубине ДО фрагментного шейдера, но
    /// только если ближнее уже нарисовано: обход в порядке
    /// хэш-таблицы, как было раньше, отдавал эту экономию даром.
    ///
    /// Заодно этот вызов готовит список полупрозрачного для
    /// renderBlended: отбор и отсечение по пирамиде видимости делаются
    /// один раз на кадр, а не дважды.
    void renderOpaque(vk::Context& ctx,
                      VkPipeline opaquePipe,
                      VkPipelineLayout layout,
                      VkDescriptorSet set, const math::Frustum& frustum,
                      const glm::vec3& cameraPos);

    /// Только отбор: пирамида видимости, сортировка, счётчики кадра.
    /// Ничего не рисует.
    ///
    /// Нужен затем, что проход ландшафта можно выключить (render_passes
    /// в settings.cfg) ради замера его цены вычитанием. Отбор при этом
    /// обязан идти как обычно, иначе выключение прохода меняло бы не
    /// цену рисования, а ещё и то, что считают счётчики, — и
    /// сравнивать замер стало бы не с чем.
    void cullOnly(const math::Frustum& frustum, const glm::vec3& cameraPos) {
        cull(frustum, cameraPos);
    }

    /// Рисует полупрозрачную часть — воду — от дальнего к ближнему.
    ///
    /// Зовётся ОТДЕЛЬНО и позже renderOpaque, потому что между ними
    /// обязаны встать непрозрачные сущности (вода не пишет глубину, и
    /// моб под водой иначе рисуется поверх неё) и небо (пока оно
    /// рисовалось первым, его шейдер считался и для тех пикселей,
    /// которые потом закрывал ландшафт).
    ///
    /// Вызывать только после renderOpaque в том же кадре: список
    /// полупрозрачного собирает он.
    void renderBlended(vk::Context& ctx,
                       VkPipeline blendPipe,
                       VkPipelineLayout layout,
                       VkDescriptorSet set);

    /// Растения видимых чанков: fn(угол чанка в мире, список). Зовётся
    /// после renderOpaque/cullOnly того же кадра — отбор делает он.
    template <class Fn>
    void forEachVisibleFlora(Fn&& fn) const {
        for (const Visible& v : visible_)
            if (!v.mesh->flora.empty()) fn(v.origin, v.mesh->flora);
    }

    /// Метрики
    u32 lastDrawnChunks() const { return lastDrawnChunks_; }
    u32 lastDrawnIndices() const { return lastDrawnIndices_; }
    u32 lastEmptyChunks() const   { return lastEmptyChunks_; }
    u32 lastWaitingChunks() const { return lastWaitingChunks_; }
    /// Сколько чанков всего рассматривалось до отсечения по пирамиде.
    u32 lastConsideredChunks() const { return lastConsideredChunks_; }
    /// Сколько отсечено пирамидой видимости.
    u32 lastCulledChunks() const  { return lastCulledChunks_; }
    /// Чанки с полупрозрачной частью — по одному draw call на каждый.
    u32 lastBlendedChunks() const { return (u32)blended_.size(); }
    /// Вершин в нарисованном: по четыре на квад, как их кладёт мешер.
    u32 lastDrawnVertices() const { return lastDrawnVertices_; }

    /// Порядок непрозрачных чанков: обычно от ближнего к дальнему,
    /// чтобы ранний тест глубины отбрасывал закрытые фрагменты до
    /// фрагментного шейдера.
    ///
    /// Развернуть его — это ЗАМЕР, а не настройка. Картинка от порядка
    /// не зависит (геометрия непрозрачная, тест глубины включён), а
    /// время зависит ровно на то, сколько ранний тест сейчас
    /// экономит. Других способов увидеть эту величину нет: счётчика
    /// перекрытых фрагментов у нас не будет, а гадать по картинке
    /// нельзя.
    void setFarFirst(bool on) { farFirst_ = on; }

private:
    struct GpuMesh {
        vk::Buffer vb;
        vk::Buffer ib;
        /// Индексы непрозрачной части: [0, opaqueIndices).
        u32 opaqueIndices = 0;
        /// Всего индексов; хвост — полупрозрачные грани.
        u32 totalIndices  = 0;
        /// 16 бит, пока вершин меньше 65536 — а это почти всегда.
        /// Вдвое меньше индексного трафика на ровном месте.
        VkIndexType indexType = VK_INDEX_TYPE_UINT16;
        bool valid = false;
        /// Меш уже побывал в видеопамяти. Отличает «чанк пуст —
        /// рисовать нечего» от «меш ещё не приехал»: в обоих случаях
        /// индексов ноль, но ждать приезда надо только во втором.
        bool uploaded = false;
        /// Chunk::version, по которой построен лежащий здесь меш:
        /// по ней видно, что он устарел после правки блоков.
        u64 revision = 0;
        /// Центр полупрозрачной геометрии в координатах чанка.
        /// Сортировать проход со смешиванием по центру всего чанка
        /// нельзя: он на половине высоты мира, в шестидесяти блоках
        /// над прудом. См. mesh_builder.
        glm::vec3 blendCenter{ 0.f };
        /// Растения, видимые при этих вокселях: забираются из
        /// ChunkMesh::flora вместе с квадами. Рисует их FloraRenderer.
        std::vector<world::FloraInstance> flora;
    };
    struct ChunkGpu {
        GpuMesh                        mesh;
        std::shared_ptr<world::Chunk>  chunk;
    };

    /// Откладывает уничтожение буфера. Кадр, в котором из него ещё
    /// читает GPU, может быть в работе — освобождать сразу нельзя.
    void retire(vk::Buffer& b);
    /// Освобождает то, что GPU точно дочитал.
    void collectRetired();

    /// Чем кончилась попытка выгрузить меш. Три исхода, а не два:
    /// «не выгрузили» распадается на «нечего было» и «не смогли», и
    /// путать их нельзя. Первое — норма (квады уже отданы), второе
    /// обязано вернуть чанк в очередь, иначе он не приедет никогда.
    enum class Upload {
        Done,    ///< Меш в видеопамяти, квады можно отпускать.
        Nothing, ///< Квадов нет — выгружать нечего.
        Failed,  ///< Не хватило ресурсов; заказ нужно повторить.
    };

    /// Загружает меш чанка в уже открытый пакет передачи.
    Upload uploadMesh(vk::Context& ctx, VkCommandBuffer cmd,
                      ChunkGpu& gpu, world::Chunk& chunk);


    VkDevice         dev_  = VK_NULL_HANDLE;
    VkPhysicalDevice phys_ = VK_NULL_HANDLE;

    vk::StagingPool staging_;

    std::unordered_map<world::ChunkCoord, ChunkGpu, world::ChunkCoordHash> meshes_;

    /// Что рисуем в этом кадре, уже отобранное и отсортированное.
    struct Visible {
        GpuMesh* mesh;
        glm::vec3 origin;
        f32 distSq;
    };

    /// Полупрозрачная часть видимых чанков, со своим расстоянием —
    /// от камеры до самой воды, а не до центра чанка.
    struct Blended {
        GpuMesh* mesh;
        glm::vec3 origin;
        f32 distSq;
    };

    /// Один вызов отрисовки: сдвиг чанка, буферы, индексы. Общий для
    /// обоих проходов — раньше был лямбдой внутри render().
    /// Отбор: пирамида видимости и порядок обхода. Заполняет
    /// visible_ и blended_ и все счётчики кадра.
    void cull(const math::Frustum& frustum, const glm::vec3& cameraPos);

    void drawMesh(VkCommandBuffer cmd, VkPipelineLayout layout,
                  const Visible& v, u32 first, u32 count);

    std::vector<Blended>      blended_;
    std::vector<VoxelVertex>  scratchVerts_;
    std::vector<u32>          scratchIndices_;
    std::vector<u16>          scratchIndices16_;
    std::vector<world::Quad>  scratchQuads_;
    /// Из них те, чей меш уже построен и правда грузится.
    std::vector<Visible>      visible_;

    /// Буферы, отправленные на покой: номер кадра и дескрипторы.
    struct Retired { u64 frame; vk::Buffer::Handles h; };
    std::vector<Retired> retired_;
    /// Номер последнего показанного кадра — отсчёт для Retired.
    u64 frameNo_ = 0;
    /// Staging-буферы текущего пакета: освобождаются только после
    /// того, как GPU дочитал их (endTransferBatch).
    std::vector<vk::StagingBuffer*> pendingStaging_;

    /// См. setFarFirst. По умолчанию — от ближнего к дальнему.
    bool farFirst_ = false;
    u32 lastDrawnChunks_  = 0;
    u32 lastDrawnIndices_ = 0;
    /// Чанки, попавшие в пирамиду видимости, но не нарисованные:
    /// геометрии нет. Ровно это и видно на экране как
    /// дыра в ландшафте, и по картинке «дыра» неотличима от «за этим
    /// чанком просто нет мира». По числу — отличима.
    u32 lastEmptyChunks_  = 0;
    /// Из них те, чей меш ещё заказан и ожидается.
    u32 lastWaitingChunks_ = 0;
    /// Чанки с мешами, рассмотренные в этом кадре, и сколько из них
    /// отсекла пирамида видимости. Без первого числа «нарисовано 63»
    /// не говорит ничего: то ли отсечение работает, то ли чанков всего
    /// шестьдесят три.
    u32 lastConsideredChunks_ = 0;
    u32 lastCulledChunks_     = 0;
    u32 lastDrawnVertices_    = 0;
};

} // namespace render
