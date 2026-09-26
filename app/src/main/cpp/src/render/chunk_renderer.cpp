/**
 * @file chunk_renderer.cpp
 * @brief Рендер: меширование чанков, отсечение, инстансинг, камера.
 */
#include "chunk_renderer.h"
#include "../core/log.h"
#include <algorithm>
#include <cstring>
#include <atomic>
#include <memory>
#include <mutex>
#include <vector>

namespace render {

bool ChunkRenderer::init(VkDevice dev, VkPhysicalDevice phys) {
    dev_ = dev; phys_ = phys;
    staging_.init(dev, phys);
    scratchVerts_.reserve(65536);
    scratchIndices_.reserve(98304);
    scratchQuads_.reserve(16384);
    return true;
}

void ChunkRenderer::shutdown() {
    for (auto& [_, m] : meshes_) {
        auto& g = m.mesh;
        if (g.valid) { g.vb.destroy(); g.ib.destroy(); }
    }
    meshes_.clear();

    // Здесь уже всё дождались: устройство простаивает.
    for (auto& r : retired_) {
        vkDestroyBuffer(r.h.dev, r.h.buf, nullptr);
        vkFreeMemory(r.h.dev, r.h.mem, nullptr);
    }
    retired_.clear();
    staging_.destroy();
}

void ChunkRenderer::retire(vk::Buffer& b) {
    auto h = b.release();
    if (h.buf != VK_NULL_HANDLE) retired_.push_back({ frameNo_, h });
}

void ChunkRenderer::collectRetired() {
    // Столько кадров GPU может держать в работе; плюс один про запас.
    const u64 keep = (u64)vk::Context::MAX_FRAMES + 1;
    usize w = 0;
    for (usize i = 0; i < retired_.size(); ++i) {
        if (frameNo_ < retired_[i].frame + keep) {
            retired_[w++] = retired_[i];
            continue;
        }
        vkDestroyBuffer(retired_[i].h.dev, retired_[i].h.buf, nullptr);
        vkFreeMemory(retired_[i].h.dev, retired_[i].h.mem, nullptr);
    }
    retired_.resize(w);
}

void ChunkRenderer::forgetChunk(world::ChunkCoord c) {
    auto it = meshes_.find(c);
    if (it == meshes_.end()) return;
    // Чанк выгружается посреди кадра, а его буферы могут быть заняты
    // в ещё не показанных кадрах: уничтожение откладываем.
    {
        auto& g = it->second.mesh;
        if (g.valid) { retire(g.vb); retire(g.ib); }
    }
    meshes_.erase(it);
}

// ============================================================
// Загрузка меша чанка в открытый пакет передачи.
// ============================================================
ChunkRenderer::Upload ChunkRenderer::uploadMesh(vk::Context& ctx,
                                                VkCommandBuffer cmd,
                                                ChunkGpu& gpu,
                                                world::Chunk& chunk)
{
    auto& gm = gpu.mesh;

    // Вершины строим прямо из хранимых квадов, под замком меша.
    // Копия всего списка, которая была здесь раньше, ничего не давала:
    // замок всё равно нужен, а сотня килобайт на чанк переливалась
    // туда-обратно каждый кадр загрузки.
    {
        std::lock_guard lk(chunk.meshMutex);
        if (!chunk.mesh.built) return Upload::Nothing;
        buildChunkVertices(chunk, chunk.mesh.quads,
                           scratchVerts_, scratchIndices_, gm.opaqueIndices,
                           &gm.blendCenter);
        // По каким вокселям построено то, что сейчас поедет в
        // видеопамять: по этому номеру видно, что лежащий меш устарел
        // после правки блоков.
        gm.revision = chunk.mesh.revision;
        // Растения едут вместе с мешем: список построен по тем же
        // вокселям. Копия, а не перенос: если выгрузка сорвётся и чанк
        // встанет в очередь заново, список у чанка должен остаться.
        // Отпускается он вместе с квадами (releaseQuads).
        gm.flora = chunk.mesh.flora;
        chunk.mesh.ready.store(false, std::memory_order_release);
    }

    // Разбор первых нескольких мешей в журнал. По картинке нельзя
    // отличить «граней не построилось» от «построились, но ушли не в
    // тот проход»: и то и другое выглядит как мир, вывернутый
    // наизнанку. А по числам — можно.
    //
    // Раньше здесь печатались только границы «небо min..max»: на целый
    // чанк они почти всегда 0..7 и не значат ничего. Считать нужно
    // РАСПРЕДЕЛЕНИЕ, и считать по верхним граням — это они видны с
    // поверхности. Открытая земля обязана давать небо 7; если её
    // грани уходят в младшие значения, освещение всего мира
    // проваливается ровно во столько раз, во сколько мал множитель
    // 0.18 + 0.82 * небо/7, а из общего сумрака торчат отдельные
    // правильно посчитанные блоки.
    {
        static int logged = 0;
        if (logged < 6) {
            ++logged;
            usize skyHist[8] = {}, aoHist[4] = {};
            usize topSky[8] = {}, topCount = 0;
            std::lock_guard lk(chunk.meshMutex);
            for (const auto& q : chunk.mesh.quads) {
                const bool up = (q.v0.face == 2);   // +Y, см. world::FACES
                for (u8 v : q.sky) {
                    ++skyHist[v & 7];
                    if (up) { ++topSky[v & 7]; ++topCount; }
                }
                for (u8 v : q.ao) ++aoHist[v & 3];
            }
            LOGI("меш чанка %d,%d: квадов %zu, вершин %zu, индексов %zu "
                 "(непрозрачных %u, полупрозрачных %zu)",
                 chunk.coord.x, chunk.coord.z,
                 chunk.mesh.quads.size(), scratchVerts_.size(),
                 scratchIndices_.size(), gm.opaqueIndices,
                 scratchIndices_.size() - gm.opaqueIndices);
            LOGI("  небо 0..7: %zu %zu %zu %zu %zu %zu %zu %zu; AO 0..3: %zu %zu %zu %zu",
                 skyHist[0], skyHist[1], skyHist[2], skyHist[3],
                 skyHist[4], skyHist[5], skyHist[6], skyHist[7],
                 aoHist[0], aoHist[1], aoHist[2], aoHist[3]);
            LOGI("  верхние грани, небо 0..7: %zu %zu %zu %zu %zu %zu %zu %zu"
                 " (углов %zu, доля открытых %.2f)",
                 topSky[0], topSky[1], topSky[2], topSky[3],
                 topSky[4], topSky[5], topSky[6], topSky[7], topCount,
                 topCount ? (double)topSky[7] / (double)topCount : 0.0);
        }
    }

    if (scratchIndices_.empty()) {
        // Чанк целиком пустой (небо или толща камня внутри) — рисовать нечего.
        gm.totalIndices = 0;
        gm.opaqueIndices = 0;
        gm.uploaded = true;
        return Upload::Done;
    }

    // Индекс в 16 бит, пока вершин меньше 65536. Для чанка 32x128x32
    // после жадного слияния это практически всегда так, а трафик и
    // видеопамять под индексы сразу вдвое меньше.
    const bool narrow = scratchVerts_.size() <= 65535;
    gm.indexType = narrow ? VK_INDEX_TYPE_UINT16 : VK_INDEX_TYPE_UINT32;

    const void* ibSrc = scratchIndices_.data();
    u64 ibBytes = scratchIndices_.size() * sizeof(u32);
    if (narrow) {
        scratchIndices16_.resize(scratchIndices_.size());
        for (usize i = 0; i < scratchIndices_.size(); ++i)
            scratchIndices16_[i] = (u16)scratchIndices_[i];
        ibSrc   = scratchIndices16_.data();
        ibBytes = scratchIndices16_.size() * sizeof(u16);
    }
    const u64 vbBytes = scratchVerts_.size() * sizeof(VoxelVertex);

    if (gm.valid && (gm.vb.size() < vbBytes || gm.ib.size() < ibBytes)) {
        retire(gm.vb); retire(gm.ib);
        gm.valid = false;
        // Буферов больше нет: если пересоздать их не удастся, меш
        // должен считаться незагруженным — иначе чанк останется
        // дырой, про которую все думают, что она нарисована.
        gm.uploaded = false;
    }
    if (!gm.valid) {
        // С запасом 25%, чтобы мелкие правки блоков не пересоздавали буфер.
        const u64 vbCap = vbBytes + vbBytes / 4;
        const u64 ibCap = ibBytes + ibBytes / 4;
        if (!gm.vb.create(dev_, phys_, vbCap, vk::BufferUsage::Vertex, false))
            return Upload::Failed;
        if (!gm.ib.create(dev_, phys_, ibCap, vk::BufferUsage::Index, false)) {
            gm.vb.destroy();
            return Upload::Failed;
        }
        gm.valid = true;
    }

    auto* sVb = staging_.acquire(vbBytes);
    if (!sVb) return Upload::Failed;
    auto* sIb = staging_.acquire(ibBytes);
    if (!sIb) { staging_.retire(sVb); return Upload::Failed; }

    std::memcpy(sVb->mapped, scratchVerts_.data(), vbBytes);
    std::memcpy(sIb->mapped, ibSrc, ibBytes);

    VkBufferCopy c1{0, 0, vbBytes};
    vkCmdCopyBuffer(cmd, sVb->buffer, gm.vb.handle(), 1, &c1);
    VkBufferCopy c2{0, 0, ibBytes};
    vkCmdCopyBuffer(cmd, sIb->buffer, gm.ib.handle(), 1, &c2);

    // Staging возвращается в оборот не сразу: после endTransferBatch()
    // вызывающий помечает его сроком годности, и он освободится, когда
    // GPU наверняка дочитает.
    pendingStaging_.push_back(sVb);
    pendingStaging_.push_back(sIb);

    gm.totalIndices = (u32)scratchIndices_.size();
    gm.uploaded = true;
    return Upload::Done;
}

/// Освобождает список квадов чанка: они уже в видеопамяти.
static void releaseQuads(world::Chunk& chunk) {
    std::lock_guard lk(chunk.meshMutex);
    chunk.mesh.built = false;
    std::vector<world::Quad>().swap(chunk.mesh.quads);
    std::vector<world::FloraInstance>().swap(chunk.mesh.flora);
}

// ============================================================
// Кадровая загрузка. Раньше каждая пара буферов уходила отдельным
// submitOneShot с ожиданием fence — восемь полных остановок GPU на
// чанк. Теперь все копии кадра идут одним пакетом.
// ============================================================
void ChunkRenderer::uploadChunks(vk::Context& ctx, world::ChunkManager& world,
                                 const std::vector<world::MeshReady>& ready)
{
    frameNo_ = ctx.framesPresented();
    collectRetired();
    if (ready.empty()) return;

    VkCommandBuffer cmd = ctx.beginTransferBatch();
    if (cmd == VK_NULL_HANDLE) {
        // Пакет не открылся — не выгружен НИ ОДИН из забранных мешей.
        // Очередь их уже отдала, и второй раз сама не отдаст.
        for (const auto& rm : ready) world.requeueMesh(rm.chunk);
        return;
    }
    // Новый пакет: staging-буферы, из которых GPU уже дочитал, снова
    // свободны. Срок — во столько же пакетов, во сколько кольцо
    // командных буферов передачи.
    staging_.collect(vk::Context::TRANSFER_SLOTS);

    for (const auto& rm : ready) {
        if (!rm.chunk) continue;
        world::Chunk& c = *rm.chunk;
        if (c.removed.load(std::memory_order_acquire)) continue;
        const world::ChunkCoord key{ c.coord.x, c.coord.z };

        auto& gpu = meshes_[key];
        gpu.chunk = rm.chunk;

        switch (uploadMesh(ctx, cmd, gpu, c)) {
            case Upload::Done:    releaseQuads(c);        break;
            case Upload::Nothing:                         break;
            case Upload::Failed:  world.requeueMesh(rm.chunk); break;
        }
    }

    ctx.endTransferBatch();

    // Пакет отправлен, но не дождан: GPU ещё читает из staging.
    // Буферы вернутся в оборот сами, через несколько пакетов.
    for (auto* s : pendingStaging_) staging_.retire(s);
    pendingStaging_.clear();
}

// ============================================================
// Отрисовка: отбор, сортировка, два прохода.
//
// Проходы разнесены по двум вызовам, и это не косметика. Между
// непрозрачным ландшафтом и полупрозрачной водой обязаны попасть две
// вещи, которые раньше рисовались ПОСЛЕ воды:
//
//   * непрозрачные сущности — мобы, NPC, предметы, трава. Вода не
//     пишет глубину (иначе смешивание не работает), поэтому моб,
//     нарисованный после неё, проходил проверку глубины и оказывался
//     ПОВЕРХ водной глади, стоя при этом под водой;
//   * небо. Оно закрывает весь экран, и пока оно рисовалось первым,
//     каждый пиксель считал его шейдер, даже если поверх ложился
//     ландшафт. Замер (tools/gpubench): шейдер неба в десять раз
//     дороже простой заливки той же площади, а кадр vkcheck
//     показывает, что геометрия закрывает под две трети экрана.
//
// Порядок в RenderSystem::render теперь такой: ландшафт, сущности,
// трава, небо, вода, контур, интерфейс.
// ============================================================
void ChunkRenderer::drawMesh(VkCommandBuffer cmd, VkPipelineLayout layout,
                             const Visible& v, u32 first, u32 count)
{
    if (count == 0) return;
    const ChunkPush push{ glm::vec4(v.origin, 0.f) };
    vkCmdPushConstants(cmd, layout, VK_SHADER_STAGE_VERTEX_BIT,
                       0, sizeof(push), &push);
    const VkBuffer vb = v.mesh->vb.handle();
    VkDeviceSize offsets[] = { 0 };
    vkCmdBindVertexBuffers(cmd, 0, 1, &vb, offsets);
    vkCmdBindIndexBuffer(cmd, v.mesh->ib.handle(), 0, v.mesh->indexType);
    vkCmdDrawIndexed(cmd, count, 1, first, 0, 0);
}

// ============================================================
// Отбор. Вынесен из renderOpaque отдельной функцией: те же решения
// нужны и когда проход ландшафта выключен ради замера (см.
// cullOnly в заголовке). Рисование отсюда убрано целиком — здесь
// только выбор геометрии и счётчики.
// ============================================================
void ChunkRenderer::cull(const math::Frustum& frustum, const glm::vec3& cameraPos)
{
    lastDrawnChunks_   = 0;
    lastDrawnIndices_  = 0;
    lastEmptyChunks_   = 0;
    lastWaitingChunks_ = 0;
    lastConsideredChunks_ = 0;
    lastCulledChunks_     = 0;
    lastDrawnVertices_    = 0;

    constexpr f32 CH = (f32)world::CHUNK_SIZE;
    constexpr f32 CY = (f32)world::CHUNK_SIZE_Y;

    // ---- 1. Отбор ----
    visible_.clear();
    blended_.clear();
    for (auto& [coord, cm] : meshes_) {
        ++lastConsideredChunks_;
        const glm::vec3 cmin{ (f32)coord.x * CH, 0.f, (f32)coord.z * CH };
        const glm::vec3 cmax{ cmin.x + CH, CY, cmin.z + CH };

        math::AABB aabb; aabb.min = cmin; aabb.max = cmax;
        if (!frustum.intersectsAABB(aabb)) { ++lastCulledChunks_; continue; }

        const glm::vec3 center = (cmin + cmax) * 0.5f;
        const glm::vec3 d = center - cameraPos;
        const f32 distSq = glm::dot(d, d);
        GpuMesh* chosen = nullptr;
        if (cm.mesh.valid && cm.mesh.totalIndices > 0) chosen = &cm.mesh;

        if (!chosen) {
            // Чанк в кадре, но рисовать нечем — это и есть дыра в
            // ландшафте. По картинке она неотличима от «за этим чанком
            // просто нет мира», по числу — вполне.
            ++lastEmptyChunks_;
            if (!cm.mesh.uploaded) ++lastWaitingChunks_;
            continue;
        }

        visible_.push_back({ chosen, cmin, distSq });
        if (chosen->totalIndices > chosen->opaqueIndices) {
            // Расстояние до ВОДЫ, а не до центра чанка. Центр чанка
            // лежит на половине высоты мира; для пруда на поверхности
            // это шестьдесят с лишним блоков мимо, и порядок
            // смешивания по нему выходил случайный — дальняя вода
            // ложилась поверх ближней, и стык двух чанков с водой
            // читался как ступенька.
            const glm::vec3 wd = cmin + chosen->blendCenter - cameraPos;
            blended_.push_back({ chosen, cmin, glm::dot(wd, wd) });
        }
        ++lastDrawnChunks_;
        lastDrawnIndices_ += chosen->totalIndices;
        lastDrawnVertices_ += chosen->totalIndices / 6 * 4;   // квад = 6 индексов на 4 вершины
    }
    // blended_ — подмножество visible_: пусто одно, пусто и другое.
    if (visible_.empty()) return;

    // ---- 2. Порядок ----
    //
    // От ближнего к дальнему: ранний тест глубины отбрасывает
    // закрытые фрагменты до фрагментного шейдера, а он здесь стоит
    // больше всего в кадре.
    //
    // farFirst_ разворачивает порядок и нужен только замеру: картинка
    // от него не меняется (геометрия непрозрачная, тест глубины
    // включён), а разница во времени и есть то, что ранний тест
    // сейчас экономит.
    if (farFirst_)
        std::sort(visible_.begin(), visible_.end(),
                  [](const Visible& a, const Visible& b) { return a.distSq > b.distSq; });
    else
        std::sort(visible_.begin(), visible_.end(),
                  [](const Visible& a, const Visible& b) { return a.distSq < b.distSq; });
}

void ChunkRenderer::renderOpaque(vk::Context& ctx,
                                 VkPipeline opaquePipe,
                                 VkPipelineLayout layout,
                                 VkDescriptorSet set, const math::Frustum& frustum,
                                 const glm::vec3& cameraPos)
{
    VkCommandBuffer cmd = ctx.currentCmd();

    ctx.setFullViewport(cmd);

    cull(frustum, cameraPos);
    if (visible_.empty()) return;

    // Непрозрачное, от ближнего к дальнему: порядок задал cull().
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, opaquePipe);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout,
                            0, 1, &set, 0, nullptr);
    for (const auto& v : visible_) drawMesh(cmd, layout, v, 0, v.mesh->opaqueIndices);
}

// ============================================================
// Полупрозрачный проход: вода. Отбор и сортировку сделал
// renderOpaque, здесь остаётся только нарисовать — и нарисовать
// ПОСЛЕ всей непрозрачной геометрии и после неба.
// ============================================================
void ChunkRenderer::renderBlended(vk::Context& ctx,
                                  VkPipeline blendPipe,
                                  VkPipelineLayout layout,
                                  VkDescriptorSet set)
{
    // Обратный порядок — единственный, при котором смешивание даёт
    // верный результат: дальняя вода должна лечь под ближнюю.
    //
    // Сортируется отдельный список: порядок непрозрачного прохода
    // здесь не годится, он считан от центров чанков.
    if (blended_.empty()) return;
    VkCommandBuffer cmd = ctx.currentCmd();
    ctx.setFullViewport(cmd);
    std::sort(blended_.begin(), blended_.end(),
              [](const Blended& a, const Blended& b) { return a.distSq > b.distSq; });

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, blendPipe);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout,
                            0, 1, &set, 0, nullptr);
    for (const auto& b : blended_) {
        const Visible v{ b.mesh, b.origin, b.distSq };
        drawMesh(cmd, layout, v, b.mesh->opaqueIndices,
                 b.mesh->totalIndices - b.mesh->opaqueIndices);
    }
}

} // namespace render
