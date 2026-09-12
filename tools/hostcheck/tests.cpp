// ============================================================
// tools/hostcheck/tests.cpp — проверки логики, не зависящей от
// Android и Vulkan: шум, ECS, аллокаторы, планировщик задач,
// формат сохранений, жадное меширование.
//
// Собирается и запускается скриптом tools/hostcheck/run.sh.
// ============================================================
#include "core/job_system.h"
#include "core/memory.h"
#include "ecs/registry.h"
#include "save/save_format.h"
#include "world/block.h"
#include "world/chunk.h"
#include "world/chunk_manager.h"
#include "render/mesh_builder.h"
#include "vk/vk_buffer.h"
#include "vk/vk_texture.h"
#include "world/noise.h"
#include "world/terrain.h"
#include "world/day_cycle.h"
#include "mobs/mob_ai.h"
#include "mobs/mob_def.h"
#include "ui/ui_context.h"
#include "world/features.h"

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <set>
#include <string>
#include <vector>

namespace {

int g_failed = 0;
int g_total  = 0;
const char* g_group = "";

void group(const char* name) {
    g_group = name;
    std::printf("\n  %s\n", name);
}

void check(bool cond, const char* what) {
    ++g_total;
    if (cond) {
        std::printf("    ok   %s\n", what);
    } else {
        ++g_failed;
        std::printf("    FAIL %s  [%s]\n", what, g_group);
    }
}

// ------------------------------------------------------------
// Шум: диапазон, детерминированность, зависимость от seed
// ------------------------------------------------------------
void testNoise() {
    group("world::SimplexNoise");

    world::SimplexNoise n(12345);

    f32 lo = 1e9f, hi = -1e9f;
    f64 sum = 0.0;
    const int N = 40000;
    for (int i = 0; i < N; ++i) {
        const f32 x = (f32)(i % 211) * 0.137f;
        const f32 y = (f32)(i % 97)  * 0.311f;
        const f32 z = (f32)(i % 313) * 0.079f;
        const f32 v = n.sample3D(x, y, z);
        lo = v < lo ? v : lo;
        hi = v > hi ? v : hi;
        sum += v;
    }
    check(lo >= -1.05f && hi <= 1.05f, "значения лежат в [-1, 1]");
    check(hi - lo > 1.0f, "шум не вырожден (есть размах)");
    check(std::fabs(sum / N) < 0.1f, "среднее близко к нулю");

    check(n.sample3D(1.5f, 2.5f, 3.5f) == n.sample3D(1.5f, 2.5f, 3.5f),
          "детерминирован при повторном вызове");

    world::SimplexNoise same(12345);
    check(same.sample3D(7.1f, 0.3f, 2.2f) == n.sample3D(7.1f, 0.3f, 2.2f),
          "одинаковый seed даёт одинаковый результат");

    world::SimplexNoise other(999);
    check(other.sample3D(7.1f, 0.3f, 2.2f) != n.sample3D(7.1f, 0.3f, 2.2f),
          "другой seed даёт другой результат");

    // Непрерывность: соседние точки не должны отличаться скачком.
    f32 maxJump = 0.f;
    for (int i = 0; i < 2000; ++i) {
        const f32 x = (f32)i * 0.01f;
        const f32 d = std::fabs(n.sample3D(x, 4.f, 9.f) - n.sample3D(x + 0.01f, 4.f, 9.f));
        maxJump = d > maxJump ? d : maxJump;
    }
    check(maxJump < 0.2f, "функция непрерывна (нет разрывов)");

    const f32 f = n.fbm3D(3.f, 1.f, 2.f, 4);
    check(f >= -1.05f && f <= 1.05f, "fbm3D нормирован");
    const f32 r = n.ridged3D(3.f, 1.f, 2.f, 3);
    check(r >= 0.f && r <= 1.05f, "ridged3D в [0, 1]");
}

// ------------------------------------------------------------
// Генерация ландшафта поверх шума
// ------------------------------------------------------------
void testTerrain() {
    group("world::TerrainGenerator");

    world::TerrainGenerator gen(0xC0FFEE);

    bool inRange = true, varies = false;
    const i32 first = gen.surfaceHeight(0, 0);
    for (i32 x = -200; x <= 200; x += 7) {
        for (i32 z = -200; z <= 200; z += 11) {
            const i32 h = gen.surfaceHeight(x, z);
            if (h < 1 || h > 127) inRange = false;
            if (h != first) varies = true;
        }
    }
    check(inRange, "высота поверхности в пределах мира");
    check(varies, "рельеф не плоский");

    check(gen.surfaceHeight(42, -17) == gen.surfaceHeight(42, -17),
          "высота детерминирована");

    world::TerrainGenerator same(0xC0FFEE);
    check(same.surfaceHeight(42, -17) == gen.surfaceHeight(42, -17),
          "тот же seed — тот же мир");

    std::set<int> biomes;
    for (i32 x = -800; x <= 800; x += 37)
        for (i32 z = -800; z <= 800; z += 41)
            biomes.insert((int)gen.biomeAt(x, z));
    check(biomes.size() >= 3, "встречается несколько биомов");

    bool caveFound = false;
    for (i32 y = 4; y < 40 && !caveFound; ++y)
        for (i32 x = 0; x < 120 && !caveFound; ++x)
            if (gen.isCave(x, y, x / 3)) caveFound = true;
    check(caveFound, "пещеры генерируются");
}

// ------------------------------------------------------------
// ECS: поколения, переиспользование индексов, пулы
// ------------------------------------------------------------
struct Pos { float x, y; };
struct Tag  { int v; };

void testRegistry() {
    group("ecs::Registry");

    ecs::Registry reg;
    const ecs::Entity a = reg.create();
    const ecs::Entity b = reg.create();

    check(a.valid() && b.valid(), "созданные сущности валидны");
    check(a != b, "дескрипторы различаются");
    check(reg.alive(a) && reg.alive(b), "обе живы");
    check(reg.aliveCount() == 2, "счётчик живых верен");

    reg.add<Pos>(a, Pos{1.f, 2.f});
    reg.add<Tag>(a, Tag{7});
    reg.add<Pos>(b, Pos{3.f, 4.f});

    check(reg.get<Pos>(a) && reg.get<Pos>(a)->x == 1.f, "компонент читается");
    check(reg.has<Tag>(a) && !reg.has<Tag>(b), "has<> различает сущности");

    reg.destroy(a);
    check(!reg.alive(a), "после destroy сущность мертва");
    check(reg.get<Pos>(a) == nullptr, "компоненты удалены вместе с сущностью");
    check(reg.alive(b) && reg.get<Pos>(b)->x == 3.f, "соседняя сущность не задета");
    check(reg.aliveCount() == 1, "счётчик уменьшился");

    // Индекс переиспользуется, но старый дескриптор не должен воскреснуть.
    const ecs::Entity c = reg.create();
    check(c.id == a.id, "индекс переиспользован");
    check(c != a, "поколение отличает новую сущность от старой");
    check(reg.alive(c) && !reg.alive(a), "старый дескриптор остался мёртвым");

    reg.destroy(a);   // повторное удаление старого дескриптора
    check(reg.alive(c), "повторный destroy старого дескриптора безвреден");

    // Перегрузка по «голому» индексу.
    reg.add<Tag>(c, Tag{42});
    check(reg.get<Tag>((u32)c.id) && reg.get<Tag>((u32)c.id)->v == 42,
          "доступ по u32-индексу работает");

    // View обходит только сущности со всеми компонентами.
    ecs::Registry v;
    const ecs::Entity e1 = v.create(), e2 = v.create(), e3 = v.create();
    v.add<Pos>(e1, Pos{}); v.add<Tag>(e1, Tag{1});
    v.add<Pos>(e2, Pos{});
    v.add<Tag>(e3, Tag{3});
    int visited = 0;
    v.view<Pos, Tag>().each([&](ecs::Entity, Pos&, Tag&) { ++visited; });
    check(visited == 1, "View пересекает наборы компонентов");

    // Долгий цикл создания и удаления не должен течь.
    // Хранилище — именно EnTT (требование ТЗ 3.2), а не своя реализация.
    {
        ecs::Registry e;
        const ecs::Entity x = e.create();
        e.add<Pos>(x, Pos{9.f, 9.f});
        auto& raw = e.raw();
        check(raw.valid(x.toEntt()), "сущность видна напрямую в entt::registry");
        check(raw.all_of<Pos>(x.toEntt()), "компонент лежит в хранилище EnTT");
        check(raw.storage<Pos>().size() == 1, "размер совпадает с storage EnTT");
        check(&raw.get<Pos>(x.toEntt()) == e.get<Pos>(x),
              "обёртка и EnTT указывают на один объект");
    }

    ecs::Registry churn;
    for (int i = 0; i < 5000; ++i) {
        const ecs::Entity e = churn.create();
        churn.add<Pos>(e, Pos{});
        churn.destroy(e);
    }
    check(churn.aliveCount() == 0, "циклы create/destroy не накапливают сущности");
    check(churn.pool<Pos>().size() == 0, "пул компонентов пуст после удалений");
}

// ------------------------------------------------------------
// Аллокаторы
// ------------------------------------------------------------
void testMemory() {
    group("mem::Arena / mem::Pool");

    mem::Arena arena(4096);
    void* first = arena.alloc(16);
    check(first != nullptr, "первое выделение не падает");

    bool aligned = true;
    for (int i = 0; i < 200; ++i) {
        void* p = arena.alloc(64, 16);
        if (!p || ((usize)p & 15u) != 0) { aligned = false; break; }
    }
    check(aligned, "выравнивание соблюдается через границы блоков");

    void* big = arena.alloc(1 << 16);   // больше размера блока
    check(big != nullptr, "запрос больше блока обслуживается");

    arena.reset();
    check(arena.alloc(32) != nullptr, "после reset арена снова выдаёт память");

    struct Node { int v; Node* next; };
    mem::Pool<Node> pool(64);
    std::vector<Node*> live;
    for (int i = 0; i < 500; ++i) {
        Node* n = pool.alloc();
        if (!n) break;
        n->v = i;
        live.push_back(n);
    }
    check(live.size() == 500, "пул выдал все запрошенные объекты");

    bool stable = true;
    for (usize i = 0; i < live.size(); ++i)
        if (live[i]->v != (int)i) stable = false;
    check(stable, "адреса остаются валидными после роста пула");

    for (Node* n : live) pool.free(n);
    check(pool.liveCount() == 0, "все слоты возвращены");

    Node* again = pool.alloc();
    check(again != nullptr, "слоты переиспользуются");
    pool.free(again);

    // ---- std::pmr поверх арены (требование ТЗ 3.2) ----
    mem::Arena pmrArena(1 << 16);
    mem::ArenaResource res(pmrArena);

    std::pmr::vector<int> v(&res);
    for (int i = 0; i < 5000; ++i) v.push_back(i);
    bool contentOk = (v.size() == 5000);
    for (int i = 0; i < 5000 && contentOk; ++i)
        if (v[(usize)i] != i) contentOk = false;
    check(contentOk, "std::pmr::vector поверх арены хранит данные верно");

    const usize usedBefore = pmrArena.totalBytes();
    check(usedBefore > 0, "выделения ушли в арену, а не в кучу");

    v.clear();
    v.shrink_to_fit();
    pmrArena.reset();
    std::pmr::vector<int> v2(&res);
    v2.push_back(7);
    check(v2.size() == 1 && v2[0] == 7, "после reset арену можно использовать снова");
    check(res.is_equal(res), "ресурс равен сам себе");
}

// ------------------------------------------------------------
// Планировщик задач
// ------------------------------------------------------------
std::atomic<int> g_jobCounter{0};
void incJob(void*) { g_jobCounter.fetch_add(1, std::memory_order_relaxed); }

void testJobSystem() {
    group("jobs::JobSystem");

    jobs::gJobs.start(4);

    g_jobCounter.store(0);
    jobs::Counter c;
    for (int i = 0; i < 200; ++i) jobs::gJobs.submit(&c, &incJob, nullptr);
    c.wait();
    check(g_jobCounter.load() == 200, "все задачи выполнены, счётчик дошёл до нуля");

    // Регрессия: parallelFor увеличивал счётчик дважды и вис навсегда.
    std::atomic<int> sum{0};
    jobs::gJobs.parallelFor(10000, 64, [&](u32 b, u32 e) {
        int local = 0;
        for (u32 i = b; i < e; ++i) local += 1;
        sum.fetch_add(local, std::memory_order_relaxed);
    });
    check(sum.load() == 10000, "parallelFor обходит весь диапазон и не виснет");

    std::atomic<int> single{0};
    jobs::gJobs.parallelFor(1, 64, [&](u32 b, u32 e) {
        single.fetch_add((int)(e - b), std::memory_order_relaxed);
    });
    check(single.load() == 1, "parallelFor корректен на одном элементе");

    jobs::gJobs.parallelFor(0, 64, [&](u32, u32) { single.fetch_add(1000); });
    check(single.load() == 1, "пустой диапазон ничего не выполняет");

    jobs::gJobs.stop();
}

// ------------------------------------------------------------
// Формат сохранений
// ------------------------------------------------------------
void testSaveFormat() {
    group("save::ByteWriter / ByteReader");

    save::ByteWriter w;
    w.writeU8(0xAB);
    w.writeU16(0x1234);
    w.writeU32(0xDEADBEEF);
    w.writeU64(0x0123456789ABCDEFull);
    w.writeI32(-42);
    w.writeF32(3.5f);
    w.writeF64(-2.25);
    w.varU32(300);
    w.varI32(-77);
    w.str(std::string("привет"));

    save::ByteReader r(w.data());
    u8 a8 = 0; u16 a16 = 0; u32 a32 = 0; u64 a64 = 0;
    check(r.u8v(a8)  && a8  == 0xAB,       "u8 туда-обратно");
    check(r.u16v(a16) && a16 == 0x1234,    "u16 туда-обратно");
    check(r.u32v(a32) && a32 == 0xDEADBEEF,"u32 туда-обратно");
    check(r.u64v(a64) && a64 == 0x0123456789ABCDEFull, "u64 туда-обратно");

    i32 ai = 0; f32 af = 0; f64 ad = 0;
    check(r.i32v(ai) && ai == -42,   "знаковое i32 переживает запись");
    check(r.f32v(af) && af == 3.5f,  "f32 переживает запись");
    check(r.f64v(ad) && ad == -2.25, "f64 переживает запись");

    u32 av = 0; i32 avi = 0;
    check(r.varU32v(av) && av == 300, "varint u32");
    check(r.varI32v(avi) && avi == -77, "zigzag varint i32");

    std::string s;
    check(r.strv(s) && s == "привет", "строка в UTF-8");
    check(r.ok(), "чтение без ошибок");
    check(r.remaining() == 0, "буфер прочитан целиком");

    // Обрезанный буфер должен выставить ошибку, а не читать за границу.
    save::ByteReader bad(w.data().data(), 2);
    u32 dummy = 0;
    check(!bad.u32v(dummy) && !bad.ok(), "чтение за границей помечается ошибкой");
}

// ------------------------------------------------------------
// Жадное меширование
// ------------------------------------------------------------
void fillFlat(world::Chunk& c, i32 height, u16 block) {
    for (i32 x = 0; x < world::CHUNK_SIZE; ++x)
        for (i32 z = 0; z < world::CHUNK_SIZE; ++z)
            for (i32 y = 0; y < height; ++y)
                c.voxels[world::chunkIndex(x, y, z)] = block;
}

void testGreedyMesh() {
    group("world::buildGreedyMesh");

    world::blocks();   // инициализация реестра блоков

    auto chunk = std::make_unique<world::Chunk>();
    chunk->coord = {0, 0, 0};
    world::ChunkNeighbors nb;
    std::vector<world::Quad> quads;

    // Пустой чанк — ни одного квада.
    u32 n = world::buildGreedyMesh(*chunk, nb, quads, world::Lod::Full);
    check(n == 0 && quads.empty(), "пустой чанк не даёт геометрии");

    // Плоский слой камня: верх и низ должны слиться в один квад каждый,
    // плюс четыре боковых стенки.
    fillFlat(*chunk, 1, world::STONE);
    n = world::buildGreedyMesh(*chunk, nb, quads, world::Lod::Full);
    check(n > 0, "сплошной слой даёт геометрию");
    // 32x32 верхних граней обязаны схлопнуться в одну. Низ у чанка
    // без соседей дробится: по краю у него «нет свода» над границей,
    // и открытость неба там другая. В настоящем мире соседи есть.
    usize topQuads = 0;
    for (const auto& q : quads) if (q.v0.face == 2) ++topQuads;
    check(topQuads == 1, "верхняя грань слита в один квад");
    check(n < 32, "грани слиты жадно (не по вокселю на грань)");

    f32 area = 0.f;
    for (const auto& q : quads) area += glm::length(q.du) * glm::length(q.dv);
    check(area >= 32.f * 32.f, "верхняя грань покрыта целиком");

    // Одиночный блок: ровно 6 граней.
    auto single = std::make_unique<world::Chunk>();
    single->voxels[world::chunkIndex(5, 5, 5)] = world::STONE;
    n = world::buildGreedyMesh(*single, nb, quads, world::Lod::Full);
    check(n == 6, "у одиночного блока ровно 6 граней");

    // Два соседних блока вдоль X. Внутренние грани отсекаются (10 из 12),
    // а оставшиеся четыре боковые сливаются в квады шириной 2 — итого 6.
    auto pair = std::make_unique<world::Chunk>();
    pair->voxels[world::chunkIndex(5, 5, 5)] = world::STONE;
    pair->voxels[world::chunkIndex(6, 5, 5)] = world::STONE;
    n = world::buildGreedyMesh(*pair, nb, quads, world::Lod::Full);
    check(n == 6, "внутренние грани отсекаются, внешние сливаются");

    int wide = 0;
    f32 pairArea = 0.f;
    for (const auto& q : quads) {
        const f32 w = glm::length(q.du), h = glm::length(q.dv);
        if (w * h > 1.5f) ++wide;
        pairArea += w * h;
    }
    check(wide == 4, "четыре грани слиты на два блока в ширину");
    check(pairArea == 10.f, "суммарная площадь равна 10 граням единичных блоков");

    // LOD уменьшает число квадов.
    fillFlat(*chunk, 8, world::STONE);
    std::vector<world::Quad> q0, q3;
    world::buildGreedyMesh(*chunk, nb, q0, world::Lod::Full);
    world::buildGreedyMesh(*chunk, nb, q3, world::Lod::Eighth);
    check(!q3.empty(), "LOD 3 всё ещё даёт геометрию");
    check(q3.size() <= q0.size(), "LOD не увеличивает число квадов");

    // Детерминированность: тот же чанк — тот же меш.
    std::vector<world::Quad> again;
    world::buildGreedyMesh(*chunk, nb, again, world::Lod::Full);
    check(again.size() == q0.size(), "меширование детерминировано");

    // Уровни детализации не должны рождать геометрию в пустоте.
    //
    // Так выглядела настоящая поломка: огрубление брало каждый N-й
    // воксель, поверхность между точками выборки исчезала, а куски
    // её оставались висеть в воздухе. На экране это была метель из
    // чёрных плит. Проверяем прямо: ни один квад не уходит выше
    // поверхности, ни один не висит ниже дна, и верх остаётся сплошным.
    auto slab = std::make_unique<world::Chunk>();
    constexpr i32 TOP = 40;
    for (i32 x = 0; x < world::CHUNK_SIZE; ++x)
        for (i32 z = 0; z < world::CHUNK_SIZE; ++z)
            for (i32 y = 0; y < TOP; ++y)
                slab->voxels[world::chunkIndex(x, y, z)] = world::STONE;

    for (u8 l = 0; l < 4; ++l) {
        const i32 step = 1 << l;
        std::vector<world::Quad> lq;
        world::buildGreedyMesh(*slab, nb, lq, (world::Lod)l);

        f32 maxY = 0.f, topArea = 0.f;
        bool inside = true;
        for (const auto& q : lq) {
            const f32 y0 = q.v0.pos.y;
            const f32 y1 = y0 + q.du.y + q.dv.y;
            maxY = std::max(maxY, std::max(y0, y1));
            if (y0 < 0.f || y1 > (f32)world::CHUNK_SIZE_Y) inside = false;
            if (q.v0.face == 2)
                topArea += glm::length(q.du) * glm::length(q.dv);
        }
        char what[96];
        std::snprintf(what, sizeof(what),
                      "LOD %u: геометрия не выходит за чанк", (unsigned)l);
        check(inside, what);
        std::snprintf(what, sizeof(what),
                      "LOD %u: ничего не висит выше поверхности", (unsigned)l);
        check(maxY <= (f32)(TOP + step), what);
        std::snprintf(what, sizeof(what),
                      "LOD %u: верхняя поверхность сплошная", (unsigned)l);
        check(topArea >= (f32)(world::CHUNK_SIZE * world::CHUNK_SIZE), what);
    }
}

// ------------------------------------------------------------
// Затенение углов и упаковка вершины
// ------------------------------------------------------------
void testVoxelShading() {
    group("воксельное затенение и упаковка");

    world::blocks();
    world::ChunkNeighbors nb;
    std::vector<world::Quad> quads;

    // Ровная площадка без препятствий: все углы всех граней открыты.
    auto flat = std::make_unique<world::Chunk>();
    fillFlat(*flat, 1, world::STONE);
    world::buildGreedyMesh(*flat, nb, quads, world::Lod::Full);
    bool allOpen = true;
    for (const auto& q : quads)
        if (q.v0.face == 2)
            for (u8 a : q.ao) if (a != 3) allOpen = false;
    check(allOpen, "на открытой плоскости углы не затенены");

    // Ступенька: у верхней грани нижнего уровня два угла упираются
    // в стенку — они обязаны потемнеть.
    auto step = std::make_unique<world::Chunk>();
    for (i32 x = 0; x < world::CHUNK_SIZE; ++x)
        for (i32 z = 0; z < world::CHUNK_SIZE; ++z)
            step->voxels[world::chunkIndex(x, 0, z)] = world::STONE;
    for (i32 z = 0; z < world::CHUNK_SIZE; ++z)
        step->voxels[world::chunkIndex(10, 1, z)] = world::STONE;
    world::buildGreedyMesh(*step, nb, quads, world::Lod::Full);
    bool anyShaded = false;
    for (const auto& q : quads)
        if (q.v0.face == 2)
            for (u8 a : q.ao) if (a < 3) anyShaded = true;
    check(anyShaded, "у стены верхняя грань темнеет в углах");

    // Разное затенение обязано разрывать слияние: иначе тень от стены
    // растеклась бы по всей плоскости одним квадом.
    usize flatTop = 0, stepTop = 0;
    world::buildGreedyMesh(*flat, nb, quads, world::Lod::Full);
    for (const auto& q : quads) if (q.v0.face == 2) ++flatTop;
    world::buildGreedyMesh(*step, nb, quads, world::Lod::Full);
    for (const auto& q : quads) if (q.v0.face == 2) ++stepTop;
    check(stepTop > flatTop, "разное затенение не склеивается в один квад");

    // Упаковка вершины: всё достаётся обратно ровно так, как её
    // читает шейдер.
    for (u32 face = 0; face < 6; ++face) {
        const u32 p = render::packVoxelPos(32, 128, 31, face, face % 4,
                                           (face + 2) % 8, 5, face & 1);
        const bool ok = ( p        & 63u)  == 32
                     && ((p >>  6) & 255u) == 128
                     && ((p >> 14) & 63u)  == 31
                     && ((p >> 20) & 7u)   == face
                     && ((p >> 23) & 3u)   == (face % 4)
                     && ((p >> 25) & 7u)   == ((face + 2) % 8)
                     && ((p >> 28) & 7u)   == 5
                     && ((p >> 31) & 1u)   == (face & 1);
        if (!ok) { check(false, "упаковка вершины распаковывается обратно"); return; }
    }
    check(true, "упаковка вершины распаковывается обратно");
    check(sizeof(render::VoxelVertex) == 8, "вершина террейна весит 8 байт");

    // Геометрия из квадов: по четыре вершины и шесть индексов на квад,
    // и ни один индекс не выходит за пределы буфера.
    std::vector<render::VoxelVertex> verts;
    std::vector<u32> idx;
    world::buildGreedyMesh(*step, nb, quads, world::Lod::Full);
    u32 opaqueIdx = 0;
    render::buildChunkVertices(*step, quads, verts, idx, opaqueIdx);
    check(verts.size() == quads.size() * 4, "на квад приходится четыре вершины");
    check(idx.size() == quads.size() * 6, "на квад приходится шесть индексов");
    u32 maxIdx = 0;
    for (u32 i : idx) if (i > maxIdx) maxIdx = i;
    check(idx.empty() || maxIdx < verts.size(), "индексы не выходят за буфер");

    // Цвет берётся из материала, а не из текстуры.
    bool colored = false;
    for (const auto& v : verts) if (v.r || v.g || v.b) colored = true;
    check(colored, "вершины несут цвет материала");
    check(opaqueIdx == idx.size(), "у камня нет полупрозрачной части");

    // Небо: открытый склон освещён полностью, пещера — нет.
    auto cave = std::make_unique<world::Chunk>();
    for (i32 x = 0; x < world::CHUNK_SIZE; ++x)
        for (i32 z = 0; z < world::CHUNK_SIZE; ++z)
            for (i32 y = 0; y < 40; ++y)
                cave->voxels[world::chunkIndex(x, y, z)] = world::STONE;
    // Полость в толще: её пол неба не видит.
    for (i32 x = 8; x < 24; ++x)
        for (i32 z = 8; z < 24; ++z)
            for (i32 y = 20; y < 24; ++y)
                cave->voxels[world::chunkIndex(x, y, z)] = world::AIR;
    world::buildGreedyMesh(*cave, nb, quads, world::Lod::Full);
    bool openLit = false, caveDark = false;
    for (const auto& q : quads) {
        if (q.v0.face != 2) continue;
        const bool surface = q.v0.pos.y > 39.f;
        for (u8 v : q.sky) {
            if (surface && v == 7) openLit = true;
            if (!surface && v < 4) caveDark = true;
        }
    }
    check(openLit, "открытая поверхность видит небо полностью");
    check(caveDark, "пол пещеры неба почти не видит");

    // Вода уходит в хвост буфера: её рисуют отдельным проходом.
    auto lake = std::make_unique<world::Chunk>();
    for (i32 x = 0; x < world::CHUNK_SIZE; ++x)
        for (i32 z = 0; z < world::CHUNK_SIZE; ++z) {
            lake->voxels[world::chunkIndex(x, 0, z)] = world::STONE;
            lake->voxels[world::chunkIndex(x, 1, z)] = world::WATER;
        }
    world::buildGreedyMesh(*lake, nb, quads, world::Lod::Full);
    render::buildChunkVertices(*lake, quads, verts, idx, opaqueIdx);
    check(opaqueIdx > 0, "непрозрачная часть озера не пуста");
    check(opaqueIdx < idx.size(), "вода вынесена в отдельный хвост буфера");
}

// ------------------------------------------------------------
// Игровые сутки
// ------------------------------------------------------------
void testDayCycle() {
    group("world::DayCycle");

    world::DayCycle c;
    c.reset(0.30f, 0);
    check(!c.isNight(), "утро — не ночь");
    check(c.day() == 0, "счётчик суток стартует с нуля");

    // Полдень ярче полуночи.
    c.reset(0.50f, 0);
    const f32 noonLight = c.skyLight();
    c.reset(0.00f, 0);
    const f32 midnightLight = c.skyLight();
    check(noonLight > midnightLight, "днём светлее, чем ночью");
    check(midnightLight >= 0.10f, "ночью не абсолютная темнота");
    check(noonLight <= 1.01f, "освещённость не превышает единицу");

    c.reset(0.00f, 0);
    check(c.isNight(), "полночь — ночь");
    c.reset(0.90f, 0);
    check(c.isNight(), "поздний вечер — ночь");

    // Солнце поднимается к полудню и садится к полуночи.
    c.reset(0.50f, 0);
    check(c.sunElevation() > 0.9f, "в полдень солнце в зените");
    c.reset(0.00f, 0);
    check(c.sunElevation() < -0.9f, "в полночь солнце в надире");

    // Переход суток происходит ровно один раз.
    c.reset(0.99f, 3);
    i32 changes = 0;
    for (int i = 0; i < 200; ++i) {
        c.tick(world::DayCycle::DAY_LENGTH_SEC * 0.001f);
        if (c.dayJustChanged()) ++changes;
    }
    check(changes == 1, "смена суток срабатывает один раз");
    check(c.day() == 4, "номер суток увеличился");
    check(c.timeOfDay() >= 0.f && c.timeOfDay() < 1.f, "время суток остаётся в [0,1)");

    // Сериализация восстанавливает состояние.
    world::DayCycle d;
    d.setRaw(0.625f, 12);
    check(d.day() == 12 && std::fabs(d.rawTime() - 0.625f) < 1e-5f,
          "состояние восстанавливается из сохранения");
}

// ------------------------------------------------------------
// Фазы боя с боссом
// ------------------------------------------------------------
void testBossPhases() {
    group("mobs::bossPhaseFor");

    check(mobs::bossPhaseFor(1.0f, 3) == 0, "полное здоровье — первая фаза");
    check(mobs::bossPhaseFor(0.9f, 3) == 0, "выше 2/3 — всё ещё первая");
    check(mobs::bossPhaseFor(0.5f, 3) == 1, "между 1/3 и 2/3 — вторая");
    check(mobs::bossPhaseFor(0.2f, 3) == 2, "ниже 1/3 — последняя");
    check(mobs::bossPhaseFor(0.0f, 3) == 2, "ноль здоровья — последняя фаза");
    check(mobs::bossPhaseFor(0.5f, 1) == 0, "у обычного моба фаза всегда нулевая");
    check(mobs::bossPhaseFor(0.6f, 2) == 0, "две фазы: выше половины — первая");
    check(mobs::bossPhaseFor(0.4f, 2) == 1, "две фазы: ниже половины — вторая");

    // Фаза не может уменьшаться при падении здоровья.
    u8 prev = 0;
    bool monotonic = true;
    for (int i = 100; i >= 0; --i) {
        const u8 p = mobs::bossPhaseFor((f32)i / 100.f, 3);
        if (p < prev) monotonic = false;
        prev = p;
    }
    check(monotonic, "фаза только растёт по мере потери здоровья");

    const auto& warden = mobs::mobRegistry().get(mobs::MOB_BOSS_WARDEN);
    check(warden.isBoss && warden.phaseCount == 3, "Каменный Страж — босс с тремя фазами");
    check(warden.slamRadius > 0.f, "у Стража есть удар по площади");
    check(warden.spawnWeight == 0.f, "босс не участвует в обычном спавне");

    const auto& hollow = mobs::mobRegistry().get(mobs::MOB_BOSS_HOLLOW);
    check(hollow.isBoss && hollow.phaseCount == 2, "Полый Владыка — босс с двумя фазами");
    check(hollow.enrageMult > 1.f, "на последней фазе босс усиливается");
}

} // namespace

// ------------------------------------------------------------
// Нулевые дескрипторы Vulkan
//
// Драйвер их не проверяет: разыменовывает и роняет процесс внутри
// libvulkan, где от нашего кода не остаётся ни имени функции, ни
// строки. Ровно так игра падала при первом кадре — интерфейс дорастил
// свой буфер, передав нулевое физическое устройство.
// ------------------------------------------------------------
void testVulkanGuards() {
    group("vk: защита от нулевых дескрипторов");

    vk::Buffer buf;
    check(!buf.create(VK_NULL_HANDLE, VK_NULL_HANDLE, 1024,
                      vk::BufferUsage::Vertex, true),
          "буфер без устройства не создаётся");
    check(!buf.create((VkDevice)0x1, VK_NULL_HANDLE, 1024,
                      vk::BufferUsage::Vertex, true),
          "буфер без физического устройства не создаётся");
    check(!buf.create((VkDevice)0x1, (VkPhysicalDevice)0x2, 0,
                      vk::BufferUsage::Vertex, true),
          "буфер нулевого размера не создаётся");
    vk::Texture2D tex;
    check(!tex.create(VK_NULL_HANDLE, VK_NULL_HANDLE, VK_NULL_HANDLE, 0,
                      4, 4, VK_FORMAT_R8G8B8A8_UNORM, nullptr, 64,
                      VK_FILTER_NEAREST, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, false),
          "текстура без устройства не создаётся");
    check(!tex.create((VkDevice)0x1, (VkPhysicalDevice)0x2, (VkQueue)0x3, 0,
                      0, 0, VK_FORMAT_R8G8B8A8_UNORM, nullptr, 0,
                      VK_FILTER_NEAREST, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, false),
          "текстура нулевого размера не создаётся");
}

// ------------------------------------------------------------
// Мир отвечает на запрос вокселя честно
//
// Чанк попадает в карту сразу, а генерируется в фоне, и до конца
// генерации его воксели — нули. Если выдавать их за воздух, всё, что
// опирается на мир, шагает в пустоту: игрок проваливался сквозь землю
// на старте и падал бесконечно, потому что ниже нулевой отметки тоже
// был «воздух».
// ------------------------------------------------------------
void testWorldQueries() {
    group("world::ChunkManager::getVoxel");

    world::ChunkManager mgr(12345, 2);

    check(mgr.getVoxel(0, -1, 0) != world::AIR,
          "ниже мира не воздух — провалиться некуда");
    check(mgr.getVoxel(0, -500, 0) != world::AIR,
          "глубоко под миром тоже не воздух");
    check(mgr.getVoxel(1'000'000, 32, 1'000'000) != world::AIR,
          "незагруженный чанк не выдаётся за воздух");
    check(mgr.getVoxel(0, world::CHUNK_SIZE_Y, 0) == world::AIR,
          "выше мира воздух");
    check(mgr.getVoxel(0, world::CHUNK_SIZE_Y + 100, 0) == world::AIR,
          "высоко над миром тоже воздух");

    auto& reg = world::blocks();
    check(reg.isSolid(mgr.getVoxel(0, -1, 0)),
          "то, что ниже мира, твёрдое — на нём можно стоять");
    check(reg.isSolid(mgr.getVoxel(1'000'000, 32, 1'000'000)),
          "незагруженный чанк считается твёрдым");
}


// ------------------------------------------------------------
// Геометрия интерфейса.
//
// Интерфейс строится целиком на процессоре: пиксели экрана он сам
// переводит в координаты отсечения и сам раскладывает прямоугольники
// на треугольники. Обе эти операции уже были сломаны — сначала
// перевёрнутой осью Y (весь HUD уезжал за верхний край), потом
// десятью вершинами на прямоугольник вместо шести (поток вершин
// разъезжался, и интерфейса не было видно вовсе). Ни то, ни другое
// компилятор поймать не может, а на устройстве оба выглядят
// одинаково — «интерфейса нет». Поэтому проверяем здесь.
// ------------------------------------------------------------
void testUiGeometry() {
    group("ui: геометрия интерфейса");

    // Типичный экран: телефон в альбомной ориентации.
    const int W = 2306, H = 1080;
    ui::UiRenderer r;
    ui::UiContext  ui;
    ui.init(&r, W, H);

    auto verts = [&r]() -> const std::vector<ui::UiVertex>& {
        return r.pendingVertices(0);
    };

    // --- один прямоугольник = шесть вершин ---
    ui.beginFrame();
    ui.rect(100.f, 100.f, 50.f, 40.f, ui::COL_WHITE);
    check(verts().size() == 6, "прямоугольник даёт ровно шесть вершин");

    ui.beginFrame();
    ui.rect(0.f, 0.f, 10.f, 10.f, ui::COL_WHITE);
    ui.rect(20.f, 20.f, 10.f, 10.f, ui::COL_WHITE);
    ui.rectOutline(40.f, 40.f, 30.f, 30.f, 2.f, ui::COL_WHITE);
    check(verts().size() % 3 == 0,
          "поток вершин делится на треугольники без остатка");
    check(verts().size() == 6 * 6, "рамка — это четыре прямоугольника");

    // --- ось Y смотрит вниз, как принято в Vulkan ---
    ui.beginFrame();
    ui.rect(0.f, 0.f, 4.f, 4.f, ui::COL_WHITE);       // левый верхний угол
    const float topY = verts()[0].pos.y;
    ui.beginFrame();
    ui.rect(0.f, (float)H - 4.f, 4.f, 4.f, ui::COL_WHITE);  // левый нижний
    const float bottomY = verts()[0].pos.y;
    check(topY < 0.f,  "верх экрана — отрицательный Y в координатах отсечения");
    check(bottomY > 0.f, "низ экрана — положительный Y");
    check(topY < bottomY, "низ экрана ниже верха, а не наоборот");

    // --- всё нарисованное в пределах экрана остаётся в пределах экрана ---
    ui.beginFrame();
    ui.rect(0.f, 0.f, (float)W, (float)H, ui::COL_WHITE);
    bool inRange = true;
    for (const auto& v : verts())
        if (v.pos.x < -1.001f || v.pos.x > 1.001f ||
            v.pos.y < -1.001f || v.pos.y > 1.001f) inRange = false;
    check(inRange, "прямоугольник во весь экран не выходит за [-1, 1]");

    // --- сплошная заливка не трогает атлас, буквы трогают ---
    ui.beginFrame();
    ui.rect(10.f, 10.f, 10.f, 10.f, ui::COL_WHITE);
    check(verts()[0].uv.x < 0.f, "заливка помечена как «без текстуры»");

    ui.beginFrame();
    ui.text("AB", 10.f, 10.f, 2.f, ui::COL_WHITE);
    bool textUv = !verts().empty();
    for (const auto& v : verts())
        if (v.uv.x < 0.f || v.uv.x > 1.f || v.uv.y < 0.f || v.uv.y > 1.f) textUv = false;
    check(verts().size() == 12, "две буквы — два прямоугольника");
    check(textUv, "у букв координаты атласа лежат в [0, 1]");

    // --- круг и кольцо действительно что-то строят ---
    ui.beginFrame();
    ui.circle(100.f, 100.f, 40.f, ui::COL_WHITE, 16);
    check(verts().size() == 16 * 3, "круг из N сегментов — N треугольников");
    ui.beginFrame();
    ui.ring(100.f, 100.f, 30.f, 40.f, ui::COL_WHITE, 16);
    check(verts().size() == 16 * 6, "кольцо — по два треугольника на сегмент");

    // --- доворот экрана: чистый поворот, без растяжения и без сноса ---
    ui.beginFrame();
    ui.rect(0.f, 0.f, 4.f, 4.f, ui::COL_WHITE);
    const glm::vec2 base = verts()[0].pos;

    r.setSurfaceRotation(90);
    ui.beginFrame();
    ui.rect(0.f, 0.f, 4.f, 4.f, ui::COL_WHITE);
    const glm::vec2 rot90 = verts()[0].pos;

    r.setSurfaceRotation(180);
    ui.beginFrame();
    ui.rect(0.f, 0.f, 4.f, 4.f, ui::COL_WHITE);
    const glm::vec2 rot180 = verts()[0].pos;

    r.setSurfaceRotation(0);

    auto len = [](glm::vec2 p) { return std::sqrt(p.x * p.x + p.y * p.y); };
    check(std::fabs(len(base) - len(rot90)) < 1e-4f,
          "доворот на 90° не меняет длину вектора");
    check(std::fabs(rot180.x + base.x) < 1e-4f &&
          std::fabs(rot180.y + base.y) < 1e-4f,
          "доворот на 180° — это смена знака обеих координат");
    check(std::fabs(rot90.x + base.y) < 1e-4f &&
          std::fabs(rot90.y - base.x) < 1e-4f,
          "доворот на 90° переставляет координаты по часовой стрелке");

    // --- нулевой поворот ничего не трогает ---
    ui.beginFrame();
    ui.rect(0.f, 0.f, 4.f, 4.f, ui::COL_WHITE);
    check(std::fabs(verts()[0].pos.x - base.x) < 1e-6f &&
          std::fabs(verts()[0].pos.y - base.y) < 1e-6f,
          "без поворота координаты остаются прежними");

    // --- кнопки экранного управления попадают туда, куда по ним жмут ---
    // Прямоугольник интерактивной области задаётся в пикселях, а
    // рисуется в координатах отсечения: если эти два перевода
    // разойдутся, нажимать придётся мимо.
    const ui::Rect btn{ 100.f, (float)H - 200.f, 120.f, 120.f };
    check(btn.contains(btn.x + 1.f, btn.y + 1.f), "точка внутри кнопки — внутри");
    check(!btn.contains(btn.x - 1.f, btn.y + 1.f), "точка слева от кнопки — снаружи");
    check(!btn.contains(btn.x + 1.f, btn.y + btn.h), "нижняя граница не включается");
}


// ------------------------------------------------------------
// Упаковка вершины отводит под координаты 6, 8 и 6 бит. Если
// меширование когда-нибудь выдаст координату больше, она молча
// обрежется по модулю — геометрия уедет внутрь чанка, и на экране
// это будет выглядеть как «кривая отрисовка», а не как ошибка.
// Проверяем на настоящем рельефе, на всех уровнях детализации.
// ------------------------------------------------------------
void testMeshFitsPacking() {
    group("геометрия влезает в упаковку вершины");

    world::blocks();
    world::ChunkManager mgr(4242, 1);
    world::ChunkNeighbors nb;
    std::vector<world::Quad> quads;
    std::vector<render::VoxelVertex> verts;
    std::vector<u32> idx;

    auto chunk = std::make_unique<world::Chunk>();
    chunk->coord = { 3, 0, -5 };
    std::vector<world::TerrainGenerator::Column> cols;
    world::computeChunkColumns(mgr.generator(), 3, -5, cols);
    world::generateChunkVoxels(*chunk, mgr.generator(), cols.data(), 4242);

    const world::Lod levels[4] = { world::Lod::Full, world::Lod::Half,
                                   world::Lod::Quarter, world::Lod::Eighth };
    bool allFit = true, anyGeometry = false;
    for (u8 l = 0; l < 4; ++l) {
        world::buildGreedyMesh(*chunk, nb, quads, levels[l]);
        u32 opaque = 0;
        render::buildChunkVertices(*chunk, quads, verts, idx, opaque);
        if (!verts.empty()) anyGeometry = true;
        for (const auto& v : verts) {
            const u32 x = v.packed & 63u;
            const u32 y = (v.packed >> 6) & 255u;
            const u32 z = (v.packed >> 14) & 63u;
            if ((i32)x > world::CHUNK_SIZE || (i32)z > world::CHUNK_SIZE ||
                (i32)y > world::CHUNK_SIZE_Y)
                allFit = false;
        }
    }
    check(anyGeometry, "на настоящем рельефе геометрия строится");
    check(allFit, "ни одна координата не выходит за отведённые биты");

    // Сами пределы должны оставаться достижимыми: если чанк вырастет,
    // эта проверка обязана упасть здесь, а не на устройстве.
    check(world::CHUNK_SIZE   <= 63,  "сторона чанка влезает в шесть бит");
    check(world::CHUNK_SIZE_Y <= 255, "высота чанка влезает в восемь бит");
}

int main() {
    std::printf("hostcheck: проверки логики\n");
    testNoise();
    testTerrain();
    testRegistry();
    testMemory();
    testJobSystem();
    testSaveFormat();
    testGreedyMesh();
    testVoxelShading();
    testDayCycle();
    testBossPhases();

    testVulkanGuards();
    testUiGeometry();
    testMeshFitsPacking();
    testWorldQueries();

    std::printf("\n  итог: %d из %d проверок пройдено\n", g_total - g_failed, g_total);
    if (g_failed) {
        std::printf("  ПРОВАЛЕНО: %d\n", g_failed);
        return 1;
    }
    std::printf("  все проверки пройдены\n");
    return 0;
}
