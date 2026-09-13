// ============================================================
// tools/hostcheck/tests.cpp — проверки логики, не зависящей от
// Android и Vulkan: шум, ECS, аллокаторы, планировщик задач,
// формат сохранений, жадное меширование.
//
// Собирается и запускается скриптом tools/hostcheck/run.sh.
// ============================================================
#include "core/job_system.h"
#include "audio/audio_engine.h"
#include "audio/sound_registry.h"
#include "audio/audio_events.h"
#include "save/save_inventory.h"
#include "save/world_delta.h"
#include "items/item_def.h"
#include "combat/components.h"
#include "trade/trade.h"
#include "ecs/components.h"
#include "npc/dialogue.h"
#include "save/save_player.h"
#include "progression/skill_tree.h"
#include "progression/progression.h"
#include "factions/faction.h"
#include "quests/quest.h"
#include "quests/quest_def.h"
#include "combat/status_effects.h"
#include "combat/damage.h"
#include "mobs/mob_def.h"
#include "mobs/mob_ai.h"
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
#include "world/ai/pathfinding.h"
#include "core/job_system.h"

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <set>
#include "ui/hud_layout.h"
#include "input/touch_layout.h"
#include <string>
#include <vector>
#include <thread>
#include <chrono>

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
// ------------------------------------------------------------
// Часть договоров о работе с Vulkan проверяется по исходному
// тексту: настоящего VkDevice на хосте нет, а заглушки не выделяют
// памяти и ничего не синхронизируют. Читаем файл целиком; пустая
// строка означает «запустили не из корня проекта».
// ------------------------------------------------------------
static std::string readSource(const char* path) {
    std::FILE* f = std::fopen(path, "rb");
    if (!f) return {};
    std::string out;
    char buf[4096];
    usize n;
    while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) out.append(buf, n);
    std::fclose(f);
    return out;
}

// ------------------------------------------------------------
// Синхронизация прохода рендера.
//
// Буфер глубины в движке ОДИН на всю цепочку показа, а кадров в
// работе два. Значит порядок доступа к нему держится исключительно
// на зависимости подпрохода от VK_SUBPASS_EXTERNAL — а она была
// неполной сразу трижды: не ждала позднюю стадию тестов глубины
// (именно на ней глубина дописывается), не делала прошлые записи
// доступными (нулевой srcAccessMask) и не упоминала чтений, хотя
// тест глубины читает. На экране это выглядело как «видно сквозь
// блоки»: глубина одного кадра проверялась против остатков другого.
// ------------------------------------------------------------
void testRenderPassSync() {
    group("vk: зависимость прохода рендера");

    const std::string src = readSource("app/src/main/cpp/src/vk/vk_context.cpp");
    if (src.empty()) {
        check(true, "исходник vk_context.cpp не найден, проверка пропущена");
        return;
    }

    const usize beg = src.find("bool Context::createRenderPass()");
    check(beg != std::string::npos, "createRenderPass на месте");
    if (beg == std::string::npos) return;
    const usize end = src.find("\nbool Context::", beg + 10);
    const std::string body = src.substr(beg, end - beg);

    const usize dep = body.find("VkSubpassDependency dep{}");
    check(dep != std::string::npos, "зависимость подпрохода объявлена");
    if (dep == std::string::npos) return;
    const std::string d = body.substr(dep);

    const usize src_ = d.find("dep.srcStageMask");
    const usize dst_ = d.find("dep.dstStageMask");
    check(src_ != std::string::npos && dst_ != std::string::npos && src_ < dst_,
          "обе половины зависимости заданы");
    if (src_ == std::string::npos || dst_ == std::string::npos) return;

    const std::string srcHalf = d.substr(src_, dst_ - src_);
    check(srcHalf.find("LATE_FRAGMENT_TESTS") != std::string::npos,
          "ждём позднюю стадию тестов глубины прошлого кадра");
    check(srcHalf.find("VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT") != std::string::npos,
          "и делаем его записи глубины доступными");
    check(srcHalf.find("VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT") != std::string::npos,
          "то же для цвета");

    const std::string dstHalf = d.substr(dst_);
    check(dstHalf.find("VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT") != std::string::npos,
          "тест глубины читает — чтение указано во второй половине");

    // --- Отвергнутая отправка не должна оборачиваться зависанием ---
    //
    // Забор кадра подаёт vkQueueSubmit. Если отправку отвергли, его не
    // подадут никогда, а ждут его без срока: приложение замирает без
    // единой строки в журнале. И показывать после провала отправки
    // нельзя — показ ждёт семафор, которого тоже никто не подаст.
    const usize efb = src.find("void Context::endFrame()");
    check(efb != std::string::npos, "endFrame на месте");
    if (efb != std::string::npos) {
        const std::string ef = src.substr(efb);
        const usize fail = ef.find("if (sub != VK_SUCCESS)");
        const usize pres = ef.find("vkQueuePresentKHR");
        check(fail != std::string::npos && pres != std::string::npos && fail < pres,
              "неудача отправки разбирается до показа");
        if (fail != std::string::npos && pres != std::string::npos && fail < pres) {
            const std::string branch = ef.substr(fail, pres - fail);
            check(branch.find("return;") != std::string::npos,
                  "после отвергнутой отправки кадр не показывается");
        }
    }

    check(src.find("if (framePending_[currentFrame_])") != std::string::npos,
          "забор ждут, только если его кто-то обещал подать");

    // --- Слой проверки включается сам, если он есть ---
    check(src.find("instanceLayerPresent(\"VK_LAYER_KHRONOS_validation\")")
              != std::string::npos,
          "наличие слоя проверки спрашивают у загрузчика");
    check(src.find("(void)layers;") == std::string::npos,
          "и не выбрасывают список слоёв, не дойдя до vkCreateInstance");
    check(src.find("ci.enabledLayerCount       = (u32)layers.size();")
              != std::string::npos,
          "найденный слой попадает в VkInstanceCreateInfo");
}

// ------------------------------------------------------------
// Договор vk::Buffer::map().
//
// map() возвращал сохранённый при создании указатель, а unmap() его
// обнулял — и второй map() отдавал nullptr, ничего об этом не
// сообщая. Единственный, кто этим пользовался, — интерфейс: он писал
// вершины, снимал отображение и на следующем кадре получал nullptr,
// молча пропуская отрисовку. Интерфейс жил ровно два первых кадра за
// весь запуск; в журнале это выглядело как «вершин 3468,
// нарисовано 0» и держалось много сборок подряд.
//
// Заглушки Vulkan на хосте не выделяют настоящей памяти, поэтому
// проверяем то, что от них не зависит: договор о том, что map()
// после unmap() обязан вернуть отображение, а не тишину.
// ------------------------------------------------------------
void testBufferMapContract() {
    group("vk::Buffer: отображение памяти");

    // Смотрим на исходный текст: на хосте настоящий VkDevice создать
    // нечем, а договор проверить надо.
    const char* path = "app/src/main/cpp/src/vk/vk_buffer.cpp";
    std::FILE* f = std::fopen(path, "rb");
    if (!f) {   // запуск не из корня проекта — проверку пропускаем
        check(true, "исходник vk_buffer.cpp не найден, проверка пропущена");
        return;
    }
    std::string src;
    char buf[4096];
    usize n;
    while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) src.append(buf, n);
    std::fclose(f);

    const usize mapPos = src.find("void* Buffer::map()");
    check(mapPos != std::string::npos, "map() на месте");
    if (mapPos == std::string::npos) return;

    const usize mapEnd = src.find("\n}", mapPos);
    const std::string body = src.substr(mapPos, mapEnd - mapPos);

    check(body.find("vkMapMemory") != std::string::npos,
          "map() создаёт отображение, если его нет, а не возвращает тишину");

    // И тот, кто этим пользуется, отображение больше не снимает.
    std::FILE* uf = std::fopen("app/src/main/cpp/src/ui/ui_renderer.cpp", "rb");
    check(uf != nullptr, "исходник ui_renderer.cpp на месте");
    if (uf) {
        std::string ui;
        while ((n = std::fread(buf, 1, sizeof(buf), uf)) > 0) ui.append(buf, n);
        std::fclose(uf);
        check(ui.find(".unmap()") == std::string::npos,
              "интерфейс не снимает отображение своих вершинных буферов");
    }

    // --- Данные кадра пишутся после ожидания на заборе ---
    //
    // Буферов камеры столько же, сколько кадров в работе, и выбираются
    // они по номеру кадра. Слот, в который пишем сейчас, последний раз
    // читался кадром, отправленным двумя кадрами назад; дождаться его
    // можно только на заборе, а забор ждёт beginFrame(). Значит писать
    // в такой буфер из prepareFrame(), который идёт ДО beginFrame(),
    // нельзя: процессор перепишет матрицы прямо во время того, как GPU
    // рисует ими предыдущий кадр, и геометрия перестанет сходиться
    // сама с собой.
    std::FILE* rf = std::fopen("app/src/main/cpp/src/render/render_system.cpp", "rb");
    check(rf != nullptr, "исходник render_system.cpp на месте");
    if (rf) {
        std::string rs;
        while ((n = std::fread(buf, 1, sizeof(buf), rf)) > 0) rs.append(buf, n);
        std::fclose(rf);

        const usize prep = rs.find("void RenderSystem::prepareFrame");
        const usize rend = rs.find("void RenderSystem::render(");
        check(prep != std::string::npos && rend != std::string::npos &&
              prep < rend, "prepareFrame и render на месте");

        if (prep != std::string::npos && rend != std::string::npos && prep < rend) {
            const std::string prepBody = rs.substr(prep, rend - prep);
            check(prepBody.find("uboBuffers_[") == std::string::npos ||
                  prepBody.find("uboBuffers_[frame].write") == std::string::npos,
                  "prepareFrame не пишет в буфер камеры: забор ещё не дождан");

            const std::string rendBody = rs.substr(rend);
            check(rendBody.find("uboBuffers_[frame].write") != std::string::npos,
                  "render пишет камеру сам — после ожидания на заборе");
        }
    }

    // --- Семафор показа принадлежит изображению, а не слоту кадра ---
    //
    // renderFinished_ ждёт vkQueuePresentKHR, а показ асинхронный: он
    // может быть ещё не выполнен, когда очередь кадров вернётся к тому
    // же слоту. Выбирая семафор по номеру кадра, мы подавали сигнал на
    // семафор, которого кто-то ещё ждёт, — а это неопределённое
    // поведение, из которого на экран попадает недорисованное.
    std::FILE* vf = std::fopen("app/src/main/cpp/src/vk/vk_context.cpp", "rb");
    check(vf != nullptr, "исходник vk_context.cpp на месте");
    if (vf) {
        std::string vc;
        while ((n = std::fread(buf, 1, sizeof(buf), vf)) > 0) vc.append(buf, n);
        std::fclose(vf);

        check(vc.find("renderFinished_[imgIdx_]") != std::string::npos,
              "семафор показа выбирается по изображению");
        check(vc.find("renderFinished_[currentFrame_]") == std::string::npos,
              "и не по слоту кадра");
        check(vc.find("imagesInFlight_[imgIdx_]") != std::string::npos,
              "занятость изображения отслеживается отдельно от слота кадра");
    }
}

// ------------------------------------------------------------
// Нажатие интерфейса переживает перерисовку.
//
// Интерфейс здесь immediate-mode: список интерактивных
// прямоугольников собирается заново каждым кадром, а beginFrame()
// очищает его. Пометка «этот прямоугольник держит палец номер N»
// лежала внутри списка — и стиралась первой же перерисовкой. Палец
// держат сотню миллисекунд, то есть пять-семь кадров; к моменту
// отпускания владельца уже не существовало, и обработчик не
// вызывался НИКОГДА, кроме случая, когда палец успевал подняться в
// том же кадре.
//
// Снаружи это выглядело как «многие кнопки не работают»: круглые
// кнопки экранного управления живут в TouchInput и работали, а все
// прямоугольные — меню, инвентарь, настройки, торговля — молчали.
// ------------------------------------------------------------
void testUiTapSurvivesRedraw() {
    group("ui: нажатие переживает перерисовку");

    ui::UiContext ctx;
    ctx.init(nullptr, 1000, 500);   // без рендерера: рисование само себя гасит

    int taps = 0;
    const ui::Rect r{ 100.f, 100.f, 200.f, 80.f };
    auto frame = [&]() {
        ctx.beginFrame();
        const int idx = ctx.pushInteractiveRect(r, [&]() { ++taps; });
        ctx.endFrame();
        return idx;
    };

    frame();
    check(ctx.handleTouch(1, 150.f, 140.f, 0), "нажатие попало в кнопку");

    const int idx = frame();   // вот здесь список и пересобирается
    check(ctx.isInteractivePressed(idx),
          "кнопка осталась нажатой после перерисовки");

    check(ctx.handleTouch(1, 150.f, 140.f, 1), "отпускание принято");
    check(taps == 1, "обработчик вызван ровно один раз");

    // Палец увели за пределы кнопки — нажатия нет.
    frame();
    ctx.handleTouch(2, 150.f, 140.f, 0);
    frame();
    ctx.handleTouch(2, 900.f, 400.f, 2);
    const int idx2 = frame();
    check(!ctx.isInteractivePressed(idx2), "уведённый палец снимает подсветку");
    ctx.handleTouch(2, 900.f, 400.f, 1);
    check(taps == 1, "и кнопку не нажимает");

    // Касание мимо всего интерфейс не забирает: иначе оно не дойдёт
    // ни до джойстика, ни до экранных кнопок.
    frame();
    check(!ctx.handleTouch(3, 900.f, 400.f, 0),
          "касание мимо кнопок интерфейс не перехватывает");
}

// ------------------------------------------------------------
// HUD и экранные кнопки делят один экран.
//
// Столбец меню стоял в пикселях от правого края, круглые кнопки — в
// NDC, и каждая сторона знала только свои числа. На экране 2306x1080
// кнопка «CAM» пришлась ровно на «ATT»; касание при этом доставалось
// HUD, потому что интерфейс проверяется первым, — то есть «CAM» не
// работала вовсе, а «ATT» нажималась не там, где нарисована.
//
// Обе раскладки теперь лежат в заголовках (ui/hud_layout.h,
// input/touch_layout.h), и их можно сверить, не собирая приложение.
// ------------------------------------------------------------
void testHudAndButtonsDoNotOverlap() {
    group("раскладка: HUD и экранные кнопки не налезают");

    struct Size { f32 w, h; const char* name; };
    const Size sizes[] = {
        { 2306.f, 1080.f, "2306x1080" },   // тот самый телефон
        { 2400.f, 1080.f, "2400x1080" },
        { 1920.f, 1080.f, "1920x1080" },
        { 1280.f,  720.f, "1280x720"  },
        { 2560.f, 1600.f, "2560x1600" },   // планшет
    };

    // Ближайшая точка прямоугольника к центру круга: если она ближе
    // радиуса, фигуры пересекаются.
    auto circleHitsRect = [](f32 cx, f32 cy, f32 rad, const ui::HudRect& r) {
        const f32 nx = cx < r.x ? r.x : (cx > r.x + r.w ? r.x + r.w : cx);
        const f32 ny = cy < r.y ? r.y : (cy > r.y + r.h ? r.y + r.h : cy);
        const f32 dx = nx - cx, dy = ny - cy;
        return dx * dx + dy * dy < rad * rad;
    };

    int collisions = 0;
    for (const auto& s : sizes) {
        // Все прямоугольники HUD, которые всегда на экране.
        std::vector<std::pair<const char*, ui::HudRect>> rects;
        static const char* MENU[ui::HUD_MENU_COUNT] =
            { "|||", "INV", "SKL", "ATT", "QST", "REP", "SAV" };
        for (u32 i = 0; i < ui::HUD_MENU_COUNT; ++i)
            rects.push_back({ MENU[i], ui::hudMenuRect(i, s.w, s.h) });
        rects.push_back({ "миникарта", ui::minimapRect(s.w, s.h) });
        rects.push_back({ "пояс",      ui::hotbarRect(s.w, s.h) });

        // Оба варианта: обычный и зеркальный для левшей — TouchInput
        // отражает центр по горизонтали, а HUD остаётся на месте.
        for (int mirror = 0; mirror < 2; ++mirror) {
            auto centerOf = [&](const input::ButtonLayout& L) {
                glm::vec2 c = input::buttonCenterPx(L, s.w, s.h);
                if (mirror) c.x = s.w - c.x;
                return c;
            };
            const char* mode = mirror ? " (левша)" : "";

            for (u32 b = 0; b < input::DEFAULT_BUTTON_COUNT; ++b) {
                const auto& L = input::DEFAULT_BUTTONS[b];
                const glm::vec2 c = centerOf(L);
                const f32 rad = input::buttonRadiusPx(L, s.h);
                if (c.x - rad < 0.f || c.y - rad < 0.f ||
                    c.x + rad > s.w || c.y + rad > s.h) {
                    ++collisions;
                    char msg[160];
                    std::snprintf(msg, sizeof(msg), "%s%s: кнопка %s за краем экрана",
                                  s.name, mode, L.label);
                    check(false, msg);
                }
                for (const auto& [name, r] : rects) {
                    if (!circleHitsRect(c.x, c.y, rad, r)) continue;
                    ++collisions;
                    char msg[160];
                    std::snprintf(msg, sizeof(msg), "%s%s: кнопка %s накрывает %s",
                                  s.name, mode, L.label, name);
                    check(false, msg);
                }
            }

            // И сами круглые кнопки не должны налезать друг на друга.
            for (u32 a = 0; a < input::DEFAULT_BUTTON_COUNT; ++a)
                for (u32 b = a + 1; b < input::DEFAULT_BUTTON_COUNT; ++b) {
                    const auto& A = input::DEFAULT_BUTTONS[a];
                    const auto& B = input::DEFAULT_BUTTONS[b];
                    const glm::vec2 ca = centerOf(A), cb = centerOf(B);
                    const f32 dx = ca.x - cb.x, dy = ca.y - cb.y;
                    const f32 rr = input::buttonRadiusPx(A, s.h)
                                 + input::buttonRadiusPx(B, s.h);
                    if (dx * dx + dy * dy >= rr * rr) continue;
                    ++collisions;
                    char msg[160];
                    std::snprintf(msg, sizeof(msg), "%s%s: кнопки %s и %s налезают",
                                  s.name, mode, A.label, B.label);
                    check(false, msg);
                }
        }
    }
    check(collisions == 0, "ни одного наложения на проверенных экранах");
}

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


// ------------------------------------------------------------
// Курсор чтения вокселей обязан отвечать ровно то же, что
// ChunkManager::getVoxel. Он ускоряет чтение вдесятеро, и вся его
// ценность держится на том, что правила краёв мира, незагруженных
// чанков и перехода через границу чанка у него те же самые.
// ------------------------------------------------------------
void testVoxelReader() {
    group("world::VoxelReader");

    world::blocks();
    world::ChunkManager mgr(777, 2);

    // Набор точек, задевающий все особые случаи: над миром, под миром,
    // далеко за пределами загруженного, и проход через границы чанков
    // по обеим осям, включая отрицательные координаты.
    const i32 pts[][3] = {
        { 0, 0, 0 }, { 0, -1, 0 }, { 0, -500, 0 },
        { 0, world::CHUNK_SIZE_Y, 0 }, { 0, world::CHUNK_SIZE_Y + 50, 0 },
        { 31, 40, 31 }, { 32, 40, 32 }, { 33, 40, 33 },
        { -1, 40, -1 }, { -32, 40, -32 }, { -33, 40, -33 },
        { 1'000'000, 32, 1'000'000 }, { -1'000'000, 32, -1'000'000 },
    };

    bool same = true;
    {
        world::VoxelReader rd(mgr);
        for (const auto& p : pts)
            if (rd.at(p[0], p[1], p[2]) != mgr.getVoxel(p[0], p[1], p[2])) same = false;
    }
    check(same, "ответ совпадает с getVoxel в особых точках");

    // Длинный проход по нескольким чанкам подряд: именно здесь курсор
    // переоткрывает чанк, и именно здесь легче всего разойтись.
    bool sameRun = true;
    {
        world::VoxelReader rd(mgr);
        for (i32 x = -70; x <= 70; x += 7)
            for (i32 z = -70; z <= 70; z += 7)
                for (i32 y = 0; y < world::CHUNK_SIZE_Y; y += 17)
                    if (rd.at(x, y, z) != mgr.getVoxel(x, y, z)) sameRun = false;
    }
    check(sameRun, "ответ совпадает при переходах через границы чанков");

    // Два курсора подряд по одним и тем же точкам дают одно и то же:
    // состояние курсора не должно влиять на ответ.
    bool stable = true;
    {
        world::VoxelReader a(mgr), b(mgr);
        for (i32 y = 0; y < world::CHUNK_SIZE_Y; y += 5) {
            (void)b.at(999, y, 999);          // уводим второй курсор в другой чанк
            if (a.at(4, y, 4) != b.at(4, y, 4)) stable = false;
        }
    }
    check(stable, "ответ не зависит от истории обращений курсора");

    // isSolid — та же таблица блоков, что у мира.
    bool solidSame = true;
    {
        world::VoxelReader rd(mgr);
        auto& reg = world::blocks();
        for (i32 y = -2; y < world::CHUNK_SIZE_Y + 2; y += 11)
            if (rd.isSolid(5, y, 5) != reg.isSolid(mgr.getVoxel(5, y, 5))) solidSame = false;
    }
    check(solidSame, "isSolid согласован с таблицей блоков");
}


// ------------------------------------------------------------
// Форма пути.
//
// A* хранил в узле собственный индекс, а записывал его как индекс
// родителя «предпоследнего добавленного» узла. Путь из-за этого
// собирался из чужой ветки поиска: восемь блоков превращались в
// четыре тысячи точек, моб шёл зигзагом, а сглаживание потом
// перебирало эти тысячи точек попарно с проверкой видимости.
//
// Ни один тест этого не ловил, потому что путь «находился». Ловит
// свойство: соседние точки пути обязаны быть соседними клетками.
// ------------------------------------------------------------
void testPathShape() {
    group("world::ai::findPath");

    world::blocks();
    jobs::gJobs.start(2);
    {
        world::ChunkManager mgr(31337, 2);
        // Ждём настоящий рельеф: без него весь мир — сплошной камень,
        // и путей не существует ни у правильного A*, ни у сломанного.
        bool ready = false;
        for (int i = 0; i < 500 && !ready; ++i) {
            mgr.update({ 8.f, 70.f, 8.f });
            ready = mgr.getVoxel(8, world::CHUNK_SIZE_Y - 1, 8) == world::AIR &&
                    mgr.getVoxel(8, 0, 8) == world::BEDROCK;
            if (!ready) std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        check(ready, "мир для поиска пути сгенерирован");

        if (ready) {
            const i32 sy = mgr.generator().surfaceHeight(8, 8);
            const i32 gy = mgr.generator().surfaceHeight(20, 20);
            world::ai::MoveParams mp;
            auto r = world::ai::findPath(mgr, { 8, sy, 8 }, { 20, gy, 20 }, mp, 2000);

            check(r.ok, "путь до точки в двенадцати блоках найден");
            if (r.ok && !r.waypoints.empty()) {
                check(r.waypoints.front() == glm::ivec3(8, sy, 8),
                      "путь начинается в стартовой клетке");
                check(r.waypoints.back() == glm::ivec3(20, gy, 20),
                      "путь заканчивается в целевой клетке");

                // Главное свойство: соседние точки — соседние клетки.
                bool adjacent = true;
                for (usize i = 1; i < r.waypoints.size(); ++i) {
                    const glm::ivec3 d = r.waypoints[i] - r.waypoints[i - 1];
                    const i32 stepXZ = std::abs(d.x) + std::abs(d.z);
                    if (stepXZ != 1) adjacent = false;          // ровно один шаг по горизонтали
                    if (d.y > 1 || d.y < -3) adjacent = false;  // прыжок на 1, падение до 3
                }
                check(adjacent, "соседние точки пути — соседние клетки");

                // Длина пути соразмерна расстоянию. Сломанная сборка
                // давала тысячи точек на дюжину блоков.
                check(r.waypoints.size() < 200,
                      "путь не разрастается в тысячи точек");
            }
        }
    }
    jobs::gJobs.stop();
}


// ------------------------------------------------------------
// Микшер звука.
//
// Подсистема целиком проверяема на хосте: звуки синтезируются
// процедурно, а AAudio нужен только чтобы отдать готовый буфер
// наружу. mixInto можно звать напрямую — это и есть то, что делает
// звуковой поток.
//
// Ловим ровно те дефекты, которые нашлись обзором и которые никак
// себя не проявляют, кроме как «звук какой-то странный»: молчащий
// ползунок громкости музыки, оборванное затухание и дескриптор
// давно закончившегося звука, управляющий чужим голосом.
// ------------------------------------------------------------
void testAudioMixer() {
    group("audio::AudioEngine");

    audio::SoundRegistry::instance().init(48000);
    audio::AudioEngine eng;              // без init(): поток AAudio не нужен

    constexpr u32 FRAMES = 256;
    std::vector<f32> buf((usize)FRAMES * 2);
    auto peak = [&]() {
        f32 m = 0.f;
        for (f32 x : buf) m = std::max(m, std::fabs(x));
        return m;
    };
    auto mix = [&]() {
        std::fill(buf.begin(), buf.end(), 0.f);
        eng.mixInto(buf.data(), FRAMES);
    };
    // Доводит микшер до полной тишины. Без этого чужой недоигравший
    // голос маскирует проверку: тишину от него не отличить.
    auto drain = [&]() {
        for (int i = 0; i < 20000; ++i) { mix(); if (peak() == 0.f) return true; }
        return false;
    };

    eng.setMasterVolume(1.f);
    eng.setSfxVolume(1.f);
    eng.setMusicVolume(1.f);

    // --- громкость категорий действительно применяется ---
    auto sfx = eng.play(audio::SOUND_UI_CLICK, 1.f, /*looping=*/true);
    check(sfx.valid(), "звук запускается");
    eng.update(0.016f, {});
    mix();
    const f32 loud = peak();
    check(loud > 0.f, "звучащий голос даёт ненулевой сигнал");

    eng.setSfxVolume(0.f);
    eng.update(0.016f, {});
    mix();
    check(peak() < loud * 0.01f, "ползунок эффектов заглушает эффект");

    eng.setSfxVolume(1.f);
    eng.setVoiceIsMusic(sfx, true);
    eng.setMusicVolume(0.f);
    eng.update(0.016f, {});
    mix();
    check(peak() < loud * 0.01f, "ползунок музыки заглушает музыку");

    // Громкость, заданную владельцем голоса, пересчёт громкостей
    // обязан уважать, а не затирать: setVoiceGain и update() писали в
    // одно поле, и ползунок музыки не работал вовсе.
    eng.setMusicVolume(1.f);
    eng.setVoiceGain(sfx, 0.25f);
    eng.update(0.016f, {});
    mix();
    const f32 quarter = peak();
    eng.setVoiceGain(sfx, 1.0f);
    eng.update(0.016f, {});
    mix();
    const f32 full = peak();
    check(quarter > 0.f && full > quarter * 2.f,
          "setVoiceGain не затирается пересчётом громкостей");

    eng.stop(sfx, 0.f);
    check(drain(), "после остановки микшер замолкает");

    // --- затухание доигрывает звук, а не повторяет один кусок ---
    //
    // Признак дефекта: у затухающего голоса не сохранялось положение
    // в сэмплах, и каждый следующий буфер брался с того же места —
    // один и тот же кусок, только всё тише. По самому сигналу это не
    // видно (громкость-то падает), поэтому сравниваем ФОРМУ волны:
    // нормируем оба буфера на их собственный пик. При застывшем
    // положении формы совпадут точно.
    auto fading = eng.play(audio::SOUND_MUSIC_EXPLORE, 1.f, /*looping=*/true);
    check(fading.valid(), "длинный звук запускается");
    eng.update(0.016f, {});
    eng.stop(fading, 2.0f);

    mix();
    std::vector<f32> blockA = buf;
    mix();
    std::vector<f32> blockB = buf;

    auto normalize = [](std::vector<f32>& v) {
        f32 m = 0.f;
        for (f32 x : v) m = std::max(m, std::fabs(x));
        if (m > 0.f) for (f32& x : v) x /= m;
        return m;
    };
    const f32 peakA = normalize(blockA);
    const f32 peakB = normalize(blockB);
    check(peakA > 0.f && peakB > 0.f, "затухающий голос ещё звучит");

    f32 shapeDiff = 0.f;
    for (usize i2 = 0; i2 < blockA.size(); ++i2)
        shapeDiff = std::max(shapeDiff, std::fabs(blockA[i2] - blockB[i2]));
    check(shapeDiff > 0.01f,
          "затухающий голос продолжает звук, а не повторяет тот же кусок");

    // Затухание обязано длиться столько, сколько заказано. Именно
    // это ломал прежний порядок записи: состояние Freeing
    // публиковалось раньше длительности, и звуковой поток мог
    // увидеть длительность от прошлого использования слота — а если
    // та была нулевой, звук обрывался мгновенно вместо угасания.
    //
    // Сравнивать огибающую по соседним буферам бессмысленно: сам
    // материал нарастает быстрее, чем снимает двухсекундный спад.
    // Проверяем по времени: на середине ещё слышно, после конца —
    // тишина. Один буфер это 256/48000 секунды.
    constexpr int BLOCKS_PER_SEC = 48000 / (int)FRAMES;
    bool audibleMidway = false;
    for (int i2 = 0; i2 < BLOCKS_PER_SEC; ++i2) {       // ~первая секунда из двух
        mix();
        if (peak() > 0.f) audibleMidway = true;
    }
    check(audibleMidway, "на середине заказанного спада звук ещё слышен");

    for (int i2 = 0; i2 < BLOCKS_PER_SEC * 2; ++i2) mix();   // спад заведомо кончился
    mix();
    check(peak() == 0.f, "после конца спада голос замолкает сам");

    eng.stop(fading, 0.f);
    check(drain(), "затухавший голос освобождается");

    // --- устаревший дескриптор не управляет чужим голосом ---
    auto oneShot = eng.play(audio::SOUND_UI_CLICK, 1.f, /*looping=*/false);
    check(oneShot.valid(), "одноразовый звук запускается");
    check(drain(), "доигравший звук освобождает голос");
    check(eng.activeVoiceCount() == 0, "активных голосов не осталось");

    auto fresh = eng.play(audio::SOUND_UI_CLICK, 1.f, /*looping=*/true);
    check(fresh.valid(), "слот переиспользуется");
    check(fresh.id == oneShot.id, "тот же слот — проверка имеет смысл");
    check(!(fresh == oneShot), "у нового голоса другое поколение");

    eng.update(0.016f, {});
    mix();
    const f32 before = peak();
    check(before > 0.f, "новый голос слышен");

    eng.stop(oneShot, 0.f);            // устаревший дескриптор
    eng.update(0.016f, {});
    mix();
    check(peak() > before * 0.5f, "устаревший дескриптор не глушит чужой голос");

    eng.setVoiceGain(oneShot, 0.f);    // он же
    eng.update(0.016f, {});
    mix();
    check(peak() > before * 0.5f, "устаревший дескриптор не меняет чужую громкость");

    eng.stop(fresh, 0.f);
    check(drain(), "чужой голос останавливается своим дескриптором");

    // --- молчащий голос не выпадает из синхронизации ---
    //
    // Все четыре музыкальные петли играют всегда, слышна одна;
    // переключение треков — плавная смена громкостей. Молчащие
    // голоса мы не смешиваем, но положение в сэмплах у них обязано
    // идти дальше: иначе вернувшаяся громкость продолжит трек с
    // давно устаревшего места, и переход прозвучит рывком.
    {
        audio::AudioEngine a, b;
        a.setMasterVolume(1.f); a.setSfxVolume(1.f);
        b.setMasterVolume(1.f); b.setSfxVolume(1.f);

        auto va = a.play(audio::SOUND_MUSIC_EXPLORE, 1.f, true);
        auto vb = b.play(audio::SOUND_MUSIC_EXPLORE, 1.f, true);
        check(va.valid() && vb.valid(), "две одинаковые петли запускаются");

        std::vector<f32> bufA((usize)FRAMES * 2), bufB((usize)FRAMES * 2);
        auto mixTo = [&](audio::AudioEngine& e, std::vector<f32>& dst) {
            std::fill(dst.begin(), dst.end(), 0.f);
            e.mixInto(dst.data(), FRAMES);
        };

        // Первый играет громко, второй молчит — двадцать буферов.
        a.setVoiceGain(va, 1.f);
        b.setVoiceGain(vb, 0.f);
        for (int i2 = 0; i2 < 20; ++i2) {
            a.update(0.016f, {}); mixTo(a, bufA);
            b.update(0.016f, {}); mixTo(b, bufB);
        }
        // Теперь оба громкие: если молчавший не двигал курсор, он
        // отстанет ровно на эти двадцать буферов.
        b.setVoiceGain(vb, 1.f);
        a.update(0.016f, {}); mixTo(a, bufA);
        b.update(0.016f, {}); mixTo(b, bufB);

        f32 diff = 0.f;
        for (usize i2 = 0; i2 < bufA.size(); ++i2)
            diff = std::max(diff, std::fabs(bufA[i2] - bufB[i2]));
        check(diff < 1e-5f, "молчавший голос остался на том же месте трека");
    }

    // --- выход за пределы не ломает буфер ---
    for (int i = 0; i < 70; ++i) eng.play(audio::SOUND_UI_CLICK, 1.f, true);
    eng.update(0.016f, {});
    mix();
    bool finite = true;
    for (f32 x : buf) if (!(x >= -1.0001f && x <= 1.0001f)) finite = false;
    check(finite, "сигнал не выходит за пределы даже при переполнении голосов");
}



// ------------------------------------------------------------
// Сохранение и загрузка состояния игрока.
//
// До сих пор проверялся только байтовый поток — что u32 читается тем
// же u32. А теряются данные не там: когда в структуру добавили поле,
// а в запись или в чтение его внести забыли, либо когда порядок
// записи и чтения разошёлся. Такой дефект тихо стирает прогресс
// игрока, и заметен он только через сутки игры.
//
// Проверяем свойство: что записали — то и прочли, включая края
// (полный инвентарь, максимальные значения, зачарования).
// ------------------------------------------------------------
void testSaveRoundTrip() {
    group("save: состояние игрока туда-обратно");

    items::items();   // таблица предметов нужна для maxStack

    ecs::Registry reg;
    const ecs::Entity player = reg.create();

    items::Inventory inv;
    inv.activeHotbar = 3;
    // Заполняем ВСЕ слоты: так ловится и потеря последнего, и сдвиг
    // на единицу, и обрыв на границе категорий.
    for (u32 i = 0; i < items::INV_TOTAL_SLOTS; ++i) {
        inv.slots[i].itemId = (u16)(1 + i);
        inv.slots[i].count  = (u16)(1 + (i % 7));
        inv.slots[i].enchant.id    = (combat::EnchantmentId)(i % 4);
        inv.slots[i].enchant.level = (u8)(1 + (i % 3));
    }
    reg.add(player, inv);

    items::Wallet wal;
    wal.gold = 0xFFFFFFFFFFULL;      // заведомо больше 32 бит
    reg.add(player, wal);

    combat::EquippedWeapon eq;
    eq.weaponId = 4321;
    eq.enchant.id = (combat::EnchantmentId)2;
    eq.enchant.level = 3;
    reg.add(player, eq);

    save::ByteWriter w;
    save::serializeInventory(w, reg, player);

    // Читаем в чистый реестр — как при загрузке сохранения.
    ecs::Registry reg2;
    const ecs::Entity player2 = reg2.create();
    reg2.add(player2, items::Inventory{});
    reg2.add(player2, items::Wallet{});
    reg2.add(player2, combat::EquippedWeapon{});

    save::ByteReader r(w.data());
    check(save::deserializeInventory(r, reg2, player2), "инвентарь читается");
    check(r.ok(), "чтение инвентаря без ошибок");
    check(r.remaining() == 0, "прочитано ровно столько, сколько записано");

    auto* inv2 = reg2.get<items::Inventory>(player2);
    check(inv2 != nullptr, "инвентарь на месте");
    if (inv2) {
        check(inv2->activeHotbar == 3, "выбранный слот пояса сохранился");
        bool allSame = true;
        for (u32 i = 0; i < items::INV_TOTAL_SLOTS; ++i) {
            const auto& a = inv.slots[i];
            const auto& b = inv2->slots[i];
            if (a.itemId != b.itemId || a.count != b.count ||
                a.enchant.id != b.enchant.id || a.enchant.level != b.enchant.level)
                allSame = false;
        }
        check(allSame, "все 42 слота совпадают до последнего поля");
    }

    auto* wal2 = reg2.get<items::Wallet>(player2);
    check(wal2 && wal2->gold == 0xFFFFFFFFFFULL, "золото не теряет старшие биты");

    auto* eq2 = reg2.get<combat::EquippedWeapon>(player2);
    check(eq2 && eq2->weaponId == 4321 &&
          eq2->enchant.id == (combat::EnchantmentId)2 &&
          eq2->enchant.level == 3, "оружие с зачарованием сохранилось");

    // Обрезанный сейв не должен ни падать, ни делать вид, что всё цело.
    {
        ecs::Registry reg3;
        const ecs::Entity p3 = reg3.create();
        reg3.add(p3, items::Inventory{});
        save::ByteReader cut(w.data().data(), w.data().size() / 3);
        const bool ok = save::deserializeInventory(cut, reg3, p3);
        check(!ok || !cut.ok(), "обрезанный сейв распознаётся как повреждённый");
    }
}

// ------------------------------------------------------------
// Изменения мира игроком: то, что отличает его мир от сгенерированного.
// Потерять их — значит стереть всё, что он построил.
// ------------------------------------------------------------
void testWorldDeltaRoundTrip() {
    group("save: правки мира туда-обратно");

    save::WorldDeltaStore store;
    // Несколько чанков, включая отрицательные координаты, и правки
    // на границах чанка.
    const i32 coords[][3] = {
        { 0, 64, 0 }, { 31, 70, 31 }, { 32, 12, 32 },
        { -1, 5, -1 }, { -32, 100, -33 }, { 1000, 1, -1000 },
    };
    u16 id = 3;
    for (const auto& c : coords) store.recordBlock(c[0], c[1], c[2], id++);

    const usize modsBefore = store.totalMods();
    const usize chunksBefore = store.chunkCount();
    check(modsBefore == 6, "записаны все правки");

    save::ByteWriter w;
    store.write(w);

    save::WorldDeltaStore loaded;
    save::ByteReader r(w.data());
    check(loaded.read(r), "правки мира читаются");
    check(r.ok() && r.remaining() == 0, "прочитано ровно столько, сколько записано");
    check(loaded.totalMods() == modsBefore, "число правок совпадает");
    check(loaded.chunkCount() == chunksBefore, "число затронутых чанков совпадает");

    // Повторная правка той же клетки должна заменять, а не копиться:
    // иначе сейв растёт без предела у игрока, который что-то строит.
    save::WorldDeltaStore rep;
    for (int i = 0; i < 50; ++i) rep.recordBlock(5, 5, 5, (u16)(i + 1));
    check(rep.totalMods() == 1, "повторная правка клетки не копится");

    // Мусор вместо сейва не должен ни падать, ни притворяться успехом.
    {
        const u8 junk[] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };
        save::WorldDeltaStore bad;
        save::ByteReader br(junk, sizeof(junk));
        const bool ok = bad.read(br);
        check(!ok || !br.ok(), "мусор вместо правок распознаётся");
    }
}


// ------------------------------------------------------------
// Инвентарь: свойства, а не отдельные случаи.
//
// Здесь живёт самый неприятный класс игровых дефектов — размножение
// и пропажа предметов. Замечают их поздно, а исправить задним числом
// уже нельзя: чужие сейвы уже испорчены. Поэтому проверяем не «вот
// этот случай работает», а инварианты, которые обязаны держаться
// всегда.
// ------------------------------------------------------------
void testInventoryInvariants() {
    group("items::Inventory");

    auto& defs = items::items();

    auto totalCount = [](const items::Inventory& inv) {
        u32 n = 0;
        for (u32 i = 0; i < items::INV_TOTAL_SLOTS; ++i)
            if (!inv.slots[i].empty()) n += inv.slots[i].count;
        return n;
    };

    // Подбираем предмет, который вообще складывается в стопки.
    u16 stackable = 0, maxS = 0;
    for (u16 id = 1; id < 200 && !stackable; ++id) {
        const u16 m = defs.maxStack(id);
        if (m > 1) { stackable = id; maxS = m; }
    }
    check(stackable != 0, "в таблице есть складываемый предмет");
    if (!stackable) return;

    // --- 1. Ничего не теряется и не возникает ---
    bool balanced = true, noOverflow = true;
    {
        items::Inventory inv;
        u32 expected = 0;
        // Кладём порциями разного размера, пока не переполним.
        for (int i = 0; i < 200; ++i) {
            const u16 want = (u16)(1 + (i * 7) % (maxS * 2 + 3));
            items::ItemStack in;
            in.itemId = stackable;
            in.count  = want;
            const auto res = inv.addStack(in);

            if ((u32)res.added + (u32)res.leftover != want) balanced = false;
            expected += res.added;
            if (totalCount(inv) != expected) balanced = false;

            for (u32 k = 0; k < items::INV_TOTAL_SLOTS; ++k) {
                const auto& sl = inv.slots[k];
                if (!sl.empty() && sl.count > defs.maxStack(sl.itemId)) noOverflow = false;
            }
        }
    }
    check(balanced, "добавлено плюс остаток всегда равно тому, что клали");
    check(noOverflow, "ни один стек не перерастает свой предел");

    // --- 2. Полный инвентарь ничего не принимает и не портит ---
    {
        items::Inventory inv;
        for (u32 i = 0; i < items::INV_TOTAL_SLOTS; ++i) {
            inv.slots[i].itemId = stackable;
            inv.slots[i].count  = maxS;
        }
        const u32 before = totalCount(inv);
        items::ItemStack in;
        in.itemId = stackable;
        in.count  = 10;
        const auto res = inv.addStack(in);
        check(res.added == 0 && res.leftover == 10,
              "в полный инвентарь ничего не влезает");
        check(totalCount(inv) == before, "полный инвентарь не изменился");
    }

    // --- 3. Зачарованные предметы не сливаются в один стек ---
    {
        items::Inventory inv;
        items::ItemStack a;
        a.itemId = stackable;
        a.count  = 1;
        a.enchant.id = (combat::EnchantmentId)1;
        a.enchant.level = 1;

        items::ItemStack b = a;
        b.enchant.level = 3;

        inv.addStack(a);
        inv.addStack(b);

        u32 occupied = 0;
        for (u32 i = 0; i < items::INV_TOTAL_SLOTS; ++i)
            if (!inv.slots[i].empty()) ++occupied;
        check(occupied == 2, "два зачарования не сливаются в один стек");
        check(totalCount(inv) == 2, "при этом ничего не потерялось");
    }

    // --- 4. Кладём в конкретный слот ---
    {
        items::Inventory inv;
        items::ItemStack in;
        in.itemId = stackable;
        in.count  = (u16)(maxS + 5);

        const auto res = inv.putStack(0, in);
        check((u32)res.added + (u32)res.leftover == in.count,
              "в слот: добавлено плюс остаток равно тому, что клали");
        check(inv.slots[0].count <= maxS, "слот не перерастает предел");

        const auto bad = inv.putStack(items::INV_TOTAL_SLOTS + 7, in);
        check(bad.added == 0 && bad.leftover == in.count,
              "слот за пределами инвентаря ничего не принимает");
    }

    // --- 5. Взять стек — значит убрать его, а не скопировать ---
    {
        items::Inventory inv;
        items::ItemStack in;
        in.itemId = stackable;
        in.count  = 3;
        inv.putStack(5, in);
        const u32 before = totalCount(inv);
        const items::ItemStack taken = inv.takeStack(5);
        check(taken.count == 3, "взятое равно тому, что лежало");
        check(inv.slots[5].empty(), "слот освободился");
        check(totalCount(inv) + taken.count == before,
              "взятое ушло из инвентаря, а не размножилось");
    }
}


// ------------------------------------------------------------
// Торговля.
//
// Система была собрана целиком — цены с учётом репутации, запас,
// ежедневное обновление ассортимента, экран интерфейса, обработчики
// покупки и продажи — и при этом недостижима: компонент
// TradeInventory не добавлялся ни одной сущности, а выбор в диалоге
// «покажи товар» просто закрывал диалог. Обе связи теперь есть, и
// проверки стерегут именно их, а заодно главный инвариант экономики:
// золото не возникает и не пропадает.
// ------------------------------------------------------------
void testTrade() {
    group("trade: покупка и продажа");

    items::items();
    ecs::Registry reg;

    const ecs::Entity player = reg.create();
    reg.add(player, items::Inventory{});
    reg.add(player, items::Wallet{ 100000 });

    const ecs::Entity trader = reg.create();
    trade::TradeInventory shop;
    trade::generateTraderInventory(shop, 0xC0FFEEull);
    check(!shop.entries.empty(), "ассортимент торговца не пуст");
    if (shop.entries.empty()) return;

    const u64 shopGold = shop.gold;
    reg.add(trader, shop);
    reg.add(trader, items::Wallet{ shopGold });

    auto* tinv = reg.get<trade::TradeInventory>(trader);
    auto* pwal = reg.get<items::Wallet>(player);
    auto* twal = reg.get<items::Wallet>(trader);
    auto* pinv = reg.get<items::Inventory>(player);
    check(tinv && pwal && twal && pinv, "компоненты на месте");
    if (!tinv || !pwal || !twal || !pinv) return;

    auto totalGold = [&]() { return pwal->gold + twal->gold; };
    const u64 goldAtStart = totalGold();

    // Ищем позицию, которую можно купить и у которой есть запас.
    trade::TradeEntry* buyable = nullptr;
    for (auto& e : tinv->entries)
        if (e.isBuyable && e.stock > 0) { buyable = &e; break; }
    check(buyable != nullptr, "есть что купить");
    if (!buyable) return;

    const u16 itemId = buyable->itemId;
    const u16 stockBefore = buyable->stock;
    const u32 haveBefore  = pinv->countOf(itemId);

    const auto res = trade::buy(reg, player, trader, itemId, 1);
    check(res == trade::TradeResult::Ok, "покупка проходит");
    check(pinv->countOf(itemId) == haveBefore + 1, "предмет попал в инвентарь");
    check(tinv->entries[0].stock <= stockBefore || buyable->stock == stockBefore - 1,
          "запас торговца уменьшился");
    check(totalGold() == goldAtStart, "покупка не создаёт и не уничтожает золото");

    // Продажа обратно.
    trade::TradeEntry* sellable = nullptr;
    for (auto& e : tinv->entries)
        if (e.isSellable && e.itemId == itemId) { sellable = &e; break; }
    if (sellable) {
        const u64 before = totalGold();
        const auto sres = trade::sell(reg, player, trader, itemId, 1);
        check(sres == trade::TradeResult::Ok, "продажа проходит");
        check(pinv->countOf(itemId) == haveBefore, "предмет ушёл из инвентаря");
        check(totalGold() == before, "продажа не создаёт и не уничтожает золото");
    }

    // Нельзя купить больше, чем есть в запасе.
    {
        const auto over = trade::buy(reg, player, trader, itemId,
                                     (u16)(buyable->stock + 50));
        check(over != trade::TradeResult::Ok, "сверх запаса купить нельзя");
    }

    // Нельзя продать то, чего нет.
    {
        const auto none = trade::sell(reg, player, trader, itemId, 9999);
        check(none != trade::TradeResult::Ok, "продать несуществующее нельзя");
    }

    // Торговцу без денег продать нельзя. Раньше проверка стояла под
    // условием «если у торговца есть кошелёк», а кошелька ему никто
    // не выдавал — золото бралось бы из ниоткуда.
    {
        pinv->addItem(itemId, 5);
        twal->gold = 0;
        const u64 playerBefore = pwal->gold;
        const auto broke = trade::sell(reg, player, trader, itemId, 5);
        check(broke != trade::TradeResult::Ok, "у торговца без денег не купят");
        check(pwal->gold == playerBefore, "золото игроку при этом не начислено");
    }
}

// ------------------------------------------------------------
// Диалог: выбор, который открывает экран.
// ------------------------------------------------------------
void testDialogueOpensScreens() {
    group("npc: диалог просит открыть экран");

    ecs::Registry reg;
    npc::ActiveDialogue dlg;
    dlg.active = true;
    dlg.npcEntity = 7;

    npc::DialogueChoice trade;
    trade.action = npc::DialogueAction::OpenTrade;
    npc::applyChoice(reg, dlg, trade);
    check(dlg.pendingAction == npc::DialogueAction::OpenTrade,
          "выбор «покажи товар» оставляет намерение открыть торговлю");
    check(!dlg.active, "диалог при этом закрывается");

    dlg = npc::ActiveDialogue{};
    dlg.active = true;
    npc::DialogueChoice craft;
    craft.action = npc::DialogueAction::OpenCraft;
    npc::applyChoice(reg, dlg, craft);
    check(dlg.pendingAction == npc::DialogueAction::OpenCraft,
          "выбор «скуй мне» оставляет намерение открыть крафт");

    // Обычный выбор намерения не оставляет — иначе экран открывался бы
    // на ровном месте.
    dlg = npc::ActiveDialogue{};
    dlg.active = true;
    npc::DialogueChoice bye;
    bye.action = npc::DialogueAction::EndDialogue;
    npc::applyChoice(reg, dlg, bye);
    check(dlg.pendingAction == npc::DialogueAction::None,
          "прощание не открывает никаких экранов");

    // --- Целитель берёт деньги ---
    // В самом варианте ответа написано «за 50 золотых», а списания не
    // было вовсе: полное здоровье бесплатно и сколько угодно раз.
    {
        const ecs::Entity p = reg.create();
        reg.add(p, ecs::Health{ 100.f, 100.f, 0.f, 0.f });
        reg.add(p, items::Wallet{ 120 });
        auto* hp = reg.get<ecs::Health>(p);
        auto* w  = reg.get<items::Wallet>(p);
        hp->current = 10.f;

        npc::ActiveDialogue hd;
        hd.active = true;
        hd.playerEntity = (u32)p;
        npc::DialogueChoice heal;
        heal.action = npc::DialogueAction::Heal;

        npc::applyChoice(reg, hd, heal);
        check(hp->current == hp->max, "целитель восстанавливает здоровье");
        check(w->gold < 120, "и берёт за это деньги");
        const u64 afterFirst = w->gold;

        // Полностью здоровому лечиться незачем — и платить тоже.
        hd.active = true;
        npc::applyChoice(reg, hd, heal);
        check(w->gold == afterFirst, "со здорового денег не берут");

        // Без денег не лечат.
        hp->current = 5.f;
        w->gold = 1;
        hd.active = true;
        npc::applyChoice(reg, hd, heal);
        check(hp->current == 5.f, "без денег не лечат");
        check(w->gold == 1, "и денег не списывают");
    }
}


// ------------------------------------------------------------
// Сохранение игрока: прогресс, навыки, квесты, репутация.
//
// Отдельно — инвариант формата: сколько записей объявлено, столько и
// должно быть записано. Оба места, где число бралось из размера
// списка, а цикл под ним умел пропускать записи, разъезжали поток:
// читатель верит числу и уходит вычитывать чужие данные. Это не
// «часть сейва потерялась», это сейв, испорченный целиком начиная с
// середины.
// ------------------------------------------------------------
void testPlayerSaveRoundTrip() {
    group("save: прогресс игрока туда-обратно");

    items::items();
    ecs::Registry reg;
    const ecs::Entity player = reg.create();

    reg.add(player, ecs::Transform{ glm::vec3(12.5f, 70.25f, -33.75f) });
    reg.add(player, ecs::Health{ 42.f, 155.f, 1.5f, 0.f });
    reg.add(player, ecs::Mana{ 7.f, 99.f, 2.5f });
    reg.add(player, ecs::Stamina{ 33.f, 120.f, 9.f });
    reg.add(player, ecs::Attributes{ 17, 13, 21, 11 });

    progression::Progression prog;
    prog.xp = 123456;
    prog.level = 9;
    prog.availableAttrPoints = 4;
    reg.add(player, prog);

    progression::SkillTree tree;
    tree.unspentPoints = 3;
    tree.totalPointsEarned = 11;
    for (u16 i = 0; i < progression::SKILL_NODE_COUNT; ++i)
        tree.ranks[i] = (u8)(i % 3);
    reg.add(player, tree);

    factions::Reputation rep;
    for (u8 i = 0; i < (u8)factions::FactionId::Count; ++i)
        rep.values[i] = 100 - (i32)i * 37;
    reg.add(player, rep);

    combat::EquippedWeapon eq;
    eq.weaponId = 77;
    eq.enchant.id = (combat::EnchantmentId)1;
    eq.enchant.level = 2;
    reg.add(player, eq);

    reg.add(player, combat::Combatant{});
    reg.add(player, combat::ResonanceState{});

    // Журнал квестов: один живой квест и одна запись истории.
    quests::QuestLog qlog;
    const ecs::Entity questEnt = reg.create();
    quests::Quest q{};
    q.id = 555;
    q.progress = 3;
    q.timeRemaining = 61.5f;
    q.rewards.xp = 900;
    q.rewards.gold = 250;
    std::snprintf(q.title, sizeof(q.title), "Найти пропажу");
    reg.add(questEnt, q);
    qlog.activeQuests.push_back(questEnt);
    qlog.addHistory(111, quests::QuestState::TurnedIn, "Старое дело");
    reg.add(player, qlog);

    save::ByteWriter w;
    save::serializePlayer(w, reg, player);

    ecs::Registry reg2;
    const ecs::Entity p2 = reg2.create();
    save::ByteReader r(w.data());
    check(save::deserializePlayer(r, reg2, p2), "сохранение игрока читается");
    check(r.ok(), "чтение без ошибок");
    check(r.remaining() == 0, "прочитано ровно столько, сколько записано");

    auto* h2 = reg2.get<ecs::Health>(p2);
    check(h2 && h2->current == 42.f && h2->max == 155.f, "здоровье сохранилось");

    auto* a2 = reg2.get<ecs::Attributes>(p2);
    check(a2 && a2->strength == 17 && a2->agility == 13 &&
          a2->intelligence == 21 && a2->endurance == 11, "атрибуты сохранились");

    auto* pr2 = reg2.get<progression::Progression>(p2);
    check(pr2 && pr2->xp == 123456 && pr2->level == 9 &&
          pr2->availableAttrPoints == 4, "уровень и опыт сохранились");

    auto* t2 = reg2.get<progression::SkillTree>(p2);
    bool ranksSame = t2 != nullptr;
    if (t2) {
        if (t2->unspentPoints != 3 || t2->totalPointsEarned != 11) ranksSame = false;
        for (u16 i = 0; i < progression::SKILL_NODE_COUNT; ++i)
            if (t2->ranks[i] != (u8)(i % 3)) ranksSame = false;
    }
    check(ranksSame, "вложенные очки навыков сохранились до последнего узла");

    auto* rp2 = reg2.get<factions::Reputation>(p2);
    bool repSame = rp2 != nullptr;
    if (rp2)
        for (u8 i = 0; i < (u8)factions::FactionId::Count; ++i)
            if (rp2->values[i] != 100 - (i32)i * 37) repSame = false;
    check(repSame, "репутация по всем фракциям сохранилась");

    auto* ql2 = reg2.get<quests::QuestLog>(p2);
    check(ql2 && ql2->activeQuests.size() == 1, "активный квест сохранился");
    check(ql2 && ql2->history.size() == 1, "история квестов сохранилась");
    if (ql2 && !ql2->activeQuests.empty()) {
        auto* q2 = reg2.get<quests::Quest>(ql2->activeQuests[0]);
        check(q2 && q2->id == 555 && q2->progress == 3,
              "прогресс квеста сохранился");
    }

    // --- Инвариант формата ---
    // В журнале остался дескриптор квеста, у которого компонента уже
    // нет: так бывает после сдачи квеста. Запись о нём пропускается —
    // и если число записей взято из размера списка, поток разъедется.
    {
        auto* ql = reg.get<quests::QuestLog>(player);
        check(ql != nullptr, "журнал квестов на месте");
        if (ql) {
            ql->activeQuests.push_back(reg.create());   // сущность без Quest
            ql->activeQuests.push_back(reg.create());

            save::ByteWriter w2;
            save::serializePlayer(w2, reg, player);

            ecs::Registry reg3;
            const ecs::Entity p3 = reg3.create();
            save::ByteReader r2(w2.data());
            const bool okRead = save::deserializePlayer(r2, reg3, p3);
            check(okRead && r2.ok(),
                  "пропущенная запись квеста не ломает чтение");
            check(r2.remaining() == 0,
                  "поток не разъезжается: прочитано ровно записанное");

            auto* rep3 = reg3.get<factions::Reputation>(p3);
            bool repOk = rep3 != nullptr;
            if (rep3)
                for (u8 i = 0; i < (u8)factions::FactionId::Count; ++i)
                    if (rep3->values[i] != 100 - (i32)i * 37) repOk = false;
            check(repOk, "репутация после пропущенного квеста не испорчена");
        }
    }
}


// ------------------------------------------------------------
// Прогресс квестов.
//
// Функции notifyMobKilled и notifyItemCollected существовали, были
// написаны правильно — и их никто не вызывал. А на цели «убить N
// таких-то» и «принести N таких-то» приходится большинство
// выдаваемых квестов: генератор берёт их с весами 5 и 4 из 13.
// То есть примерно у семи квестов из десяти счётчик навсегда
// оставался в нуле. Взял квест, перебил всех — ничего не произошло.
//
// Проверяем НЕ сами notify-функции (они и раньше работали), а путь
// целиком: от удара по мобу до счётчика в журнале. Пропущенная связь
// ловится только так.
// ------------------------------------------------------------
void testQuestProgress() {
    group("quests: прогресс целей");

    mobs::mobRegistry();
    ecs::Registry reg;

    // Игрок с журналом и взятым квестом «убить двух».
    const ecs::Entity player = reg.create();
    reg.add(player, ecs::Health{ 100.f, 100.f, 0.f, 0.f });
    reg.add(player, progression::Progression{});

    const u16 mobId = 1;
    const ecs::Entity questEnt = reg.create();
    quests::Quest q{};
    q.id = 1;
    q.tmpl.type = quests::QuestType::Kill;
    q.tmpl.targetMobId = mobId;
    q.tmpl.requiredCount = 2;
    q.state = quests::QuestState::Active;
    q.ownerEntity = (u32)player;
    reg.add(questEnt, q);

    quests::QuestLog qlog;
    qlog.activeQuests.push_back(questEnt);
    reg.add(player, qlog);

    // Моб, которого сейчас убьют ударом от игрока.
    auto spawnMob = [&]() {
        const ecs::Entity m = reg.create();
        reg.add(m, ecs::Health{ 5.f, 5.f, 0.f, 0.f });
        reg.add(m, ecs::AIAgent{});
        reg.add(m, mobs::MobTag{ mobId });
        reg.add(m, combat::StatusEffects{});
        return m;
    };

    auto killByPlayer = [&](ecs::Entity m) {
        combat::DamageInstance dmg;
        dmg.amount = 999.f;
        dmg.sourceEntity = (u32)player;
        dmg.targetEntity = (u32)m;
        combat::applyDamage(reg, m, dmg);
    };

    auto* liveQuest = reg.get<quests::Quest>(questEnt);
    check(liveQuest && liveQuest->progress == 0, "счётчик начинается с нуля");

    killByPlayer(spawnMob());
    check(liveQuest && liveQuest->progress == 1,
          "убийство подходящего моба двигает счётчик");

    killByPlayer(spawnMob());
    check(liveQuest && liveQuest->progress == 2, "второе убийство тоже");
    check(liveQuest && liveQuest->state == quests::QuestState::Completed,
          "набрав требуемое, квест становится выполненным");

    // Чужой вид не засчитывается.
    {
        auto* q2 = reg.get<quests::Quest>(questEnt);
        q2->state = quests::QuestState::Active;
        q2->progress = 0;
        const ecs::Entity other = reg.create();
        reg.add(other, ecs::Health{ 5.f, 5.f, 0.f, 0.f });
        reg.add(other, ecs::AIAgent{});
        reg.add(other, mobs::MobTag{ (u16)(mobId + 1) });
        reg.add(other, combat::StatusEffects{});
        killByPlayer(other);
        check(q2->progress == 0, "убийство чужого вида не засчитывается");
    }

    // --- Бой звучит ---
    // Звуки удара по мобу, его смерти и урона игроку были написаны и
    // синтезировались при запуске, но не проигрывались нигде: бой шёл
    // молча. Проверяем через настоящий движок звука — сколько голосов
    // он завёл.
    {
        audio::SoundRegistry::instance().init(48000);
        audio::AudioEngine snd;
        audio::events().setEngine(&snd);

        const u32 before = snd.activeVoiceCount();
        const ecs::Entity m = spawnMob();
        reg.add(m, ecs::Transform{ glm::vec3(1.f, 2.f, 3.f) });
        combat::DamageInstance light;
        light.amount = 1.f;                 // не смертельный
        light.sourceEntity = (u32)player;
        combat::applyDamage(reg, m, light);
        check(snd.activeVoiceCount() > before, "удар по мобу слышен");

        // Отдельный моб: у только что ударенного стоят кадры
        // неуязвимости, и добить его тем же ударом нельзя.
        const u32 afterHit = snd.activeVoiceCount();
        const ecs::Entity victim = spawnMob();
        reg.add(victim, ecs::Transform{ glm::vec3(4.f, 5.f, 6.f) });
        killByPlayer(victim);
        check(snd.activeVoiceCount() > afterHit, "смерть моба слышна");

        const u32 afterDeath = snd.activeVoiceCount();
        combat::DamageInstance onPlayer;
        onPlayer.amount = 1.f;
        onPlayer.sourceEntity = (u32)m;
        reg.add(player, ecs::PlayerTag{});
        combat::applyDamage(reg, player, onPlayer);
        check(snd.activeVoiceCount() > afterDeath, "урон по игроку слышен");

        audio::events().setEngine(nullptr);
    }

    // Убийство не игроком не засчитывается: у моба нет прогрессии,
    // и квесты считают только игрока.
    {
        auto* q2 = reg.get<quests::Quest>(questEnt);
        q2->progress = 0;
        const ecs::Entity killer = reg.create();   // моб без Progression
        const ecs::Entity victim = spawnMob();
        combat::DamageInstance dmg;
        dmg.amount = 999.f;
        dmg.sourceEntity = (u32)killer;
        dmg.targetEntity = (u32)victim;
        combat::applyDamage(reg, victim, dmg);
        check(q2->progress == 0, "чужое убийство игроку не засчитывается");
    }
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
    testRenderPassSync();
    testUiTapSurvivesRedraw();
    testHudAndButtonsDoNotOverlap();
    testBufferMapContract();
    testUiGeometry();
    testMeshFitsPacking();
    testWorldQueries();
    testVoxelReader();
    testPathShape();
    testAudioMixer();
    testSaveRoundTrip();
    testWorldDeltaRoundTrip();
    testPlayerSaveRoundTrip();
    testQuestProgress();
    testInventoryInvariants();
    testTrade();
    testDialogueOpensScreens();

    std::printf("\n  итог: %d из %d проверок пройдено\n", g_total - g_failed, g_total);
    if (g_failed) {
        std::printf("  ПРОВАЛЕНО: %d\n", g_failed);
        return 1;
    }
    std::printf("  все проверки пройдены\n");
    return 0;
}
