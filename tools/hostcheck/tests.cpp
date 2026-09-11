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
#include "world/noise.h"
#include "world/terrain.h"

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
    check(n <= 6, "грани слиты жадно (не по вокселю на грань)");

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
}

} // namespace

int main() {
    std::printf("hostcheck: проверки логики\n");
    testNoise();
    testTerrain();
    testRegistry();
    testMemory();
    testJobSystem();
    testSaveFormat();
    testGreedyMesh();

    std::printf("\n  итог: %d из %d проверок пройдено\n", g_total - g_failed, g_total);
    if (g_failed) {
        std::printf("  ПРОВАЛЕНО: %d\n", g_failed);
        return 1;
    }
    std::printf("  все проверки пройдены\n");
    return 0;
}
