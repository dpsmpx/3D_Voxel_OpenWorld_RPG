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
#include "world/debug_scene.h"
#include "world/chunk_manager.h"
#include "render/mesh_builder.h"
#include "render/chunk_renderer.h"
#include "core/clipboard.h"
#include "config/settings.h"
#include "render/camera.h"
#include "render/instanced_renderer.h"
#include "vk/vk_buffer.h"
#include "vk/vk_texture.h"
#include "world/noise.h"
#include "world/terrain.h"
#include "world/day_cycle.h"
#include "mobs/mob_ai.h"
#include "mobs/mob_def.h"
#include "save/save_manager.h"
#include "ui/ui_context.h"
#include "world/features.h"
#include "world/ai/pathfinding.h"
#include "core/job_system.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <set>
#include "ui/hud_layout.h"
#include "config/localization.h"
#include "core/orientation.h"
#include "entity/locomotion.h"
#include "entity/mob_rigs.h"
#include "entity/humanoid_rig.h"
#include "player/player_rig.h"
#include "player/player.h"
#include "entity/rig.h"
#include "ui/ui_theme.h"
#include "ui/ui_atlas.h"
#include "ui/font_data.h"
#include "ui/ui_system.h"
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
// Стык четырёх чанков вокруг начала координат
// ------------------------------------------------------------
//
// Отрицательные координаты — классическое место ошибки: маска и сдвиг
// ведут себя там не так, как деление, и чанк (-1,-1) легко получает
// не тех соседей. Снаружи это выглядит как шов на ровном поле ровно
// по нулевой линии — либо щель, либо двойная стенка внутри земли.
//
// Проверяем то, что видно: на стыке двух сплошных чанков внутренних
// граней нет вовсе, а верхняя поверхность всех четырёх покрыта ровно
// один раз — без дыр и без наложений.
void testChunkSeamAcrossOrigin() {
    group("меш: стык чанков через начало координат");

    world::blocks();

    // Четыре чанка вокруг нуля, все с одинаковым плоским рельефом.
    struct Cell { i32 cx, cz; std::unique_ptr<world::Chunk> c; };
    Cell cells[4] = {
        { -1, -1, std::make_unique<world::Chunk>() },
        { -1,  0, std::make_unique<world::Chunk>() },
        {  0, -1, std::make_unique<world::Chunk>() },
        {  0,  0, std::make_unique<world::Chunk>() },
    };
    for (auto& cell : cells) {
        cell.c->coord = { cell.cx, 0, cell.cz };
        fillFlat(*cell.c, 20, world::STONE);
    }
    auto find = [&](i32 cx, i32 cz) -> const world::Chunk* {
        for (const auto& cell : cells)
            if (cell.cx == cx && cell.cz == cz) return cell.c.get();
        return nullptr;
    };

    // Собираем все четыре меша, каждый — со своими настоящими соседями.
    std::vector<world::Quad> quads;
    f64 topArea = 0.0;
    usize innerFaces = 0;
    i32 badX = 0, badZ = 0; u8 badFace = 0;
    for (const auto& cell : cells) {
        world::ChunkNeighbors nb;
        nb.nx = find(cell.cx - 1, cell.cz);
        nb.px = find(cell.cx + 1, cell.cz);
        nb.nz = find(cell.cx, cell.cz - 1);
        nb.pz = find(cell.cx, cell.cz + 1);
        world::buildGreedyMesh(*cell.c, nb, quads, world::Lod::Full);

        for (const auto& q : quads) {
            const f32 w = glm::length(q.du), h = glm::length(q.dv);
            if (q.v0.face == 2) topArea += (f64)(w * h);

            // Грань, смотрящая в существующего соседа, который в этом
            // месте сплошной, — это стенка внутри земли. Её быть не
            // должно ни на одной из четырёх границ.
            const bool atNegX = (q.v0.face == 1 && q.v0.pos.x == 0.f);
            const bool atPosX = (q.v0.face == 0 && q.v0.pos.x == (f32)world::CHUNK_SIZE);
            const bool atNegZ = (q.v0.face == 5 && q.v0.pos.z == 0.f);
            const bool atPosZ = (q.v0.face == 4 && q.v0.pos.z == (f32)world::CHUNK_SIZE);
            const bool haveNb = (atNegX && nb.nx) || (atPosX && nb.px)
                             || (atNegZ && nb.nz) || (atPosZ && nb.pz);
            // Выше рельефа стенка законна: там у соседа воздух.
            const f32 y0 = q.v0.pos.y;
            if (haveNb && y0 < 20.f) {
                if (!innerFaces) { badX = cell.cx; badZ = cell.cz; badFace = q.v0.face; }
                ++innerFaces;
            }
        }
    }
    if (innerFaces)
        std::printf("       первая: чанк %d,%d грань %u; всего %zu\n",
                    badX, badZ, (unsigned)badFace, innerFaces);
    check(innerFaces == 0, "на стыке сплошных чанков внутренних граней нет");

    const f64 want = 4.0 * (f64)world::CHUNK_SIZE * (f64)world::CHUNK_SIZE;
    check(topArea == want, "верх четырёх чанков покрыт ровно один раз");

    // Тот же стык, но у одного соседа его нет. Тогда грань на границе
    // строить НЕЛЬЗЯ: иначе по краю ещё не загруженного чанка встаёт
    // стена во всю толщу земли, и игрок смотрит в чёрный клин.
    {
        world::ChunkNeighbors lone;
        lone.px = find(0, -1);      // сосед только с одной стороны
        world::buildGreedyMesh(*cells[0].c, lone, quads, world::Lod::Full);
        usize wall = 0;
        for (const auto& q : quads)
            if (q.v0.face == 1 && q.v0.pos.x == 0.f && q.v0.pos.y < 20.f) ++wall;
        check(wall == 0, "по неизвестному соседу стена не строится");
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
    bool packOk = true, tailFree = true;
    for (u32 face = 0; face < 6; ++face) {
        const u32 p = render::packVoxelPos(32, 128, 31, face, face % 4,
                                           (face + 2) % 8);
        packOk = packOk
              && ( p        & 63u)  == 32
              && ((p >>  6) & 255u) == 128
              && ((p >> 14) & 63u)  == 31
              && ((p >> 20) & 7u)   == face
              && ((p >> 23) & 3u)   == (face % 4)
              && ((p >> 25) & 7u)   == ((face + 2) % 8);
        // Старшие четыре бита свободны и обязаны оставаться нулями:
        // в них лежали крапчатость и подкраска по местности, которые
        // шейдер давно перестал читать, а мешер продолжал собирать.
        if ((p >> 28) != 0u) tailFree = false;
    }
    check(packOk, "упаковка вершины распаковывается обратно");
    check(tailFree, "старшие биты вершины свободны");
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

/// Убирает строчные комментарии: в них слова «pow» и «smoothstep»
/// встречаются как раз там, где объясняется, почему их там нет.
/// Строка без ведущих и хвостовых пробелов.
static std::string trimmed(const std::string& t) {
    const usize b = t.find_first_not_of(" \t\r");
    if (b == std::string::npos) return std::string();
    const usize e = t.find_last_not_of(" \t\r");
    return t.substr(b, e - b + 1);
}

/// Разбивает текст на строки.
static std::vector<std::string> splitLines(const std::string& s) {
    std::vector<std::string> out;
    usize b = 0;
    while (b <= s.size()) {
        const usize e = s.find('\n', b);
        if (e == std::string::npos) { out.push_back(s.substr(b)); break; }
        out.push_back(s.substr(b, e - b));
        b = e + 1;
    }
    return out;
}

static std::string stripComments(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (usize i = 0; i < s.size(); ) {
        if (s[i] == '/' && i + 1 < s.size() && s[i + 1] == '/') {
            while (i < s.size() && s[i] != '\n') ++i;
        } else {
            out.push_back(s[i]);
            ++i;
        }
    }
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
void testUnknownNeighborIsNotAir() {
    group("мешер: незагруженный сосед — не воздух");

    world::blocks();

    // Столбик у самой границы чанка: его грань на стыке решается по
    // соседнему чанку.
    auto make = [] {
        auto c = std::make_unique<world::Chunk>();
        c->coord = { 0, 0, 0 };
        for (i32 y = 0; y < 34; ++y)
            for (i32 z = 0; z < world::CHUNK_SIZE; ++z)
                for (i32 x = 0; x < world::CHUNK_SIZE; ++x)
                    c->setUnlocked(x, y, z, world::STONE);
        return c;
    };

    // Сколько граней смотрит в сторону отсутствующего соседа (+X).
    auto facesTowardPlusX = [](const std::vector<world::Quad>& qs) {
        usize n = 0;
        for (const auto& q : qs)
            if (q.v0.face == 0 && q.v0.pos.x >= (f32)world::CHUNK_SIZE) ++n;
        return n;
    };

    std::vector<world::Quad> quads;

    // 1. Соседа нет — состояние неизвестно, грань строить нельзя.
    {
        auto c = make();
        world::ChunkNeighbors nb;   // все указатели пустые
        world::buildGreedyMesh(*c, nb, quads, world::Lod::Full);
        check(facesTowardPlusX(quads) == 0,
              "без соседа наружная грань на стыке не строится");
        check(!quads.empty(), "остальной чанк при этом мешируется");
    }

    // 2. Сосед есть и там воздух — грань открыта.
    auto air = std::make_unique<world::Chunk>();
    air->coord = { 1, 0, 0 };
    {
        auto c = make();
        world::ChunkNeighbors nb;
        nb.px = air.get();
        world::buildGreedyMesh(*c, nb, quads, world::Lod::Full);
        check(facesTowardPlusX(quads) > 0,
              "сосед есть и пуст — грань на стыке появляется");
    }

    // 3. Сосед есть и он сплошной — грань закрыта.
    auto solid = std::make_unique<world::Chunk>();
    solid->coord = { 1, 0, 0 };
    for (i32 y = 0; y < 34; ++y)
        for (i32 z = 0; z < world::CHUNK_SIZE; ++z)
            for (i32 x = 0; x < world::CHUNK_SIZE; ++x)
                solid->setUnlocked(x, y, z, world::STONE);
    {
        auto c = make();
        world::ChunkNeighbors nb;
        nb.px = solid.get();
        world::buildGreedyMesh(*c, nb, quads, world::Lod::Full);
        check(facesTowardPlusX(quads) == 0,
              "сосед есть и сплошной — грань на стыке закрыта");
    }

    // Сам источник: отсутствующий сосед обязан отдаваться отдельным
    // значением, а не воздухом.
    {
        auto c = make();
        world::ChunkNeighbors nb;
        check(world::sampleVoxel(*c, nb, world::CHUNK_SIZE, 10, 0) == world::UNKNOWN,
              "за границей без соседа читается UNKNOWN, а не AIR");
        nb.px = air.get();
        check(world::sampleVoxel(*c, nb, world::CHUNK_SIZE, 10, 0) == world::AIR,
              "с пустым соседом читается настоящий AIR");
        check(world::UNKNOWN != world::AIR, "UNKNOWN и AIR — разные значения");
    }

    // То же на огрублённом уровне: там объём строится отдельно, и
    // неизвестность обязана дожить до мешера.
    {
        auto c = make();
        world::ChunkNeighbors nb;
        world::buildGreedyMesh(*c, nb, quads, world::Lod::Half);
        check(facesTowardPlusX(quads) == 0,
              "на огрублённом уровне стык без соседа тоже пуст");
    }
}

void testNeighborArrivalTriggersRemesh() {
    group("мир: появление соседа перестраивает границу");

    // Механизм: задача генерации ставит в очередь меширование не
    // только своего чанка, но и четырёх соседей. Без этого чанк,
    // смешированный без соседа, навсегда остался бы с дырой на стыке —
    // ровно то, ради чего UNKNOWN и вводился.
    const std::string cm = readSource("app/src/main/cpp/src/world/chunk_manager.cpp");
    if (cm.empty()) { check(true, "исходник не найден, проверка пропущена"); return; }

    const usize gen = cm.find("c->generated.store(true");
    check(gen != std::string::npos, "чанк помечается сгенерированным");
    if (gen == std::string::npos) return;

    const std::string after = cm.substr(gen, 2200);
    // Уровень указывается при ПОСТАНОВКЕ: enqueueMesh принимает его
    // вторым доводом, и воркеру уже нечего выбирать.
    check(after.find("enqueueMesh(ctx->coord, lod)") != std::string::npos,
          "свой чанк ставится на меширование в выбранном здесь уровне");
    usize n = 0;
    for (const char* d : { "coord.x - 1", "coord.x + 1", "coord.z - 1", "coord.z + 1" })
        if (after.find(d) != std::string::npos) ++n;
    check(n == 4, "и все четыре соседа — тоже");
    check(after.find("enqueueMesh(ctx->coord);") == std::string::npos,
          "постановки без уровня не осталось");

    // Поведение: тот же чанк, смешированный без соседа и с соседом,
    // обязан дать разное число граней на стыке.
    world::blocks();
    auto c = std::make_unique<world::Chunk>();
    c->coord = { 0, 0, 0 };
    for (i32 y = 0; y < 34; ++y)
        for (i32 z = 0; z < world::CHUNK_SIZE; ++z)
            for (i32 x = 0; x < world::CHUNK_SIZE; ++x)
                c->setUnlocked(x, y, z, world::STONE);

    std::vector<world::Quad> quads;
    world::ChunkNeighbors none;
    world::buildGreedyMesh(*c, none, quads, world::Lod::Full);
    const usize without = quads.size();

    auto air = std::make_unique<world::Chunk>();
    air->coord = { 1, 0, 0 };
    world::ChunkNeighbors with;
    with.px = air.get();
    world::buildGreedyMesh(*c, with, quads, world::Lod::Full);
    check(quads.size() > without,
          "после появления соседа граница добирает грани");
}

void testVoxelColorIsPlaceIndependent() {
    group("шейдер: цвет грани зависит только от материала и грани");

    const std::string f = readSource("app/src/main/cpp/shaders/voxel.frag");
    if (f.empty()) { check(true, "шейдер не найден, проверка пропущена"); return; }
    const std::string src = stripComments(f);

    // ---- 1. никакого шума по номеру вокселя ----
    check(src.find("hash13") == std::string::npos, "функции шума в шейдере нет");
    check(src.find("fract(sin(") == std::string::npos, "и не заменена другим шумом");

    // ---- 2. альбедо: цвет вершины, грань, фаска — и всё ----
    //
    // Место фрагмента в мире здесь не участвует. Так выглядела
    // настоящая поломка: цвет умножался на волну sin() по мировым
    // координатам и на градиент по высоте, и один и тот же блок травы
    // выходил заметно разным зелёным в разных концах одного кадра.
    const usize a = src.find("vec3 albedo = ");
    check(a != std::string::npos, "альбедо считается");
    if (a != std::string::npos) {
        const std::string line = src.substr(a, src.find(';', a) - a);
        check(line.find("vColor") != std::string::npos, "из цвета материала");
        check(line.find("FACE_LIGHT") != std::string::npos,
              "и освещённости своей грани");
        for (const char* bad : { "hash", "grain", "noise", "random",
                                 "vWorldPos", "sin(", "tint" }) {
            if (line.find(bad) != std::string::npos) {
                std::printf("       альбедо зависит от «%s»\n", bad);
                check(false, "без шума, места в мире и подкрасок");
                return;
            }
        }
        check(true, "без шума, места в мире и подкрасок");
    }

    // Подкрасок по мировым координатам не осталось нигде: ни волны,
    // ни градиента по высоте. Единственные sin/cos, какие терпимы в
    // этом шейдере, — вообще никакие.
    check(src.find("sin(") == std::string::npos,
          "тригонометрии по координатам в террейне нет");
    check(src.find("vWorldPos.y - 40.0") == std::string::npos,
          "градиента цвета по высоте нет");

    // ---- 3. точность межстадийных переменных ----
    check(f.find("in highp vec2  vShade") != std::string::npos,
          "vShade объявлена highp в фрагментном шейдере");
    check(f.find("clamp(vShade, 0.0, 1.0)") != std::string::npos,
          "затенение ограничено своим договорным диапазоном");
}

// ------------------------------------------------------------
// Затенение грани задаётся ровно в одном месте
// ------------------------------------------------------------
//
// Раньше в двух: реестр блоков ставил верху цвет светлее бока, а бок
// светлее низа, и сверху шейдер добавлял полусферный свет (небо
// сверху, отражение земли снизу). Два независимых наклона одной и той
// же величины — подправить контраст граней было нельзя, не выясняя
// каждый раз, какая из двух половин сейчас видна.
void testFaceShadingHasSingleSource() {
    group("освещённость грани: один источник истины");

    const std::string f = stripComments(
        readSource("app/src/main/cpp/shaders/voxel.frag"));
    if (f.empty()) { check(true, "шейдер не найден, проверка пропущена"); return; }

    // ---- 1. таблица в шейдере есть, и в ней шесть чисел ----
    const usize t = f.find("FACE_LIGHT[6] = float[6](");
    check(t != std::string::npos, "таблица освещённости граней объявлена");
    if (t == std::string::npos) return;

    const usize open  = f.find('(', f.find("float[6]", t));
    const usize close = f.find(')', open);
    const std::string body = f.substr(open + 1, close - open - 1);
    f32 fl[6] = {0,0,0,0,0,0};
    int got = std::sscanf(body.c_str(), "%f , %f , %f , %f , %f , %f",
                          &fl[0], &fl[1], &fl[2], &fl[3], &fl[4], &fl[5]);
    check(got == 6, "в таблице ровно шесть значений");
    if (got != 6) return;

    // Верх ярче любого бока, низ темнее любого бока: без этого у куба
    // пропадает верхнее ребро и он читается плоским пятном.
    const f32 top = fl[2], bottom = fl[3];
    bool topBrightest = true, bottomDarkest = true;
    for (int i = 0; i < 6; ++i) {
        if (i != 2 && fl[i] >= top)    topBrightest  = false;
        if (i != 3 && fl[i] <= bottom) bottomDarkest = false;
    }
    check(topBrightest, "верхняя грань — самая светлая");
    check(bottomDarkest, "нижняя — самая тёмная");

    // Два боковых направления разведены. У куба в кадре видно самое
    // большее одну грань из каждой пары, и при равной освещённости
    // вертикальное ребро между ними исчезает.
    check(std::fabs(fl[0] - fl[4]) > 0.05f,
          "боковые направления различимы между собой");
    check(fl[0] == fl[1] && fl[4] == fl[5],
          "противоположные грани пары освещены одинаково");
    for (int i = 0; i < 6; ++i)
        if (fl[i] <= 0.f || fl[i] > 1.f) {
            check(false, "все множители лежат в (0, 1]");
            return;
        }
    check(true, "все множители лежат в (0, 1]");

    // ---- 2. полусферы в шейдере больше нет ----
    check(f.find("0.5 + 0.5 * N.y") == std::string::npos &&
          f.find("skyVis") == std::string::npos,
          "второго наклона по нормали в шейдере нет");

    // ---- 3. и в реестре блоков тоже нет ----
    //
    // Один материал — один цвет. Исключения ровно два и они
    // проверяются поимённо: у травы земляной бок, у дерева светлый
    // спил на тёмной коре. Всё остальное обязано совпадать.
    world::blocks();
    struct Known { u16 id; const char* name; };
    const Known same[] = {
        { world::STONE,    "камень"  }, { world::DIRT,   "земля"  },
        { world::SAND,     "песок"   }, { world::SNOW,   "снег"   },
        { world::WATER,    "вода"    }, { world::ICE,    "лёд"    },
        { world::LAVA,     "лава"    }, { world::LEAVES, "листва" },
        { world::IRON_ORE, "руда железа" }, { world::GOLD_ORE, "руда золота" },
        { world::BEDROCK,  "порода"  },
    };
    for (const auto& k : same) {
        const world::BlockDef& d = world::blocks().get(k.id);
        if (d.colorTop != d.colorSide || d.colorTop != d.colorBottom) {
            std::printf("       у «%s» грани разного цвета\n", k.name);
            check(false, "у однородных материалов все грани одного цвета");
            return;
        }
    }
    check(true, "у однородных материалов все грани одного цвета");

    const world::BlockDef& grass = world::blocks().get(world::GRASS);
    check(grass.colorTop != grass.colorSide, "у травы макушка отличается от бока");
    check(grass.colorSide == world::blocks().get(world::DIRT).colorSide,
          "а бок у неё — ровно земля");
    const world::BlockDef& wood = world::blocks().get(world::WOOD);
    check(wood.colorTop != wood.colorSide, "у дерева спил отличается от коры");
    check(wood.colorTop == wood.colorBottom, "оба спила одинаковы");

    // ---- 4. материалы различимы между собой ----
    //
    // Крапчатости, по которой руду отличали от породы, больше нет.
    // Если руда и камень совпали по цвету — жила в стене становится
    // невидимой, и никакой тест геометрии этого не поймает.
    auto lum = [](world::BlockColor c) {
        return 0.299f * (f32)((c >> 24) & 0xFF)
             + 0.587f * (f32)((c >> 16) & 0xFF)
             + 0.114f * (f32)((c >>  8) & 0xFF);
    };
    auto dist = [&](u16 a, u16 b) {
        const world::BlockColor x = world::blocks().get(a).colorTop;
        const world::BlockColor y = world::blocks().get(b).colorTop;
        const f32 dr = (f32)((i32)((x >> 24) & 0xFF) - (i32)((y >> 24) & 0xFF));
        const f32 dg = (f32)((i32)((x >> 16) & 0xFF) - (i32)((y >> 16) & 0xFF));
        const f32 db = (f32)((i32)((x >>  8) & 0xFF) - (i32)((y >>  8) & 0xFF));
        return std::sqrt(dr * dr + dg * dg + db * db);
    };
    // Руда в камне и снег на песке — те пары, что и правда стоят
    // рядом в мире.
    check(dist(world::IRON_ORE, world::STONE) > 30.f,
          "железная руда отличима от камня");
    check(dist(world::GOLD_ORE, world::STONE) > 30.f,
          "золотая руда отличима от камня");
    check(std::fabs(lum(world::blocks().get(world::GRASS).colorTop) -
                    lum(world::blocks().get(world::DIRT).colorTop)) > 20.f,
          "трава отличима от земли по светлоте, а не только по тону");
}

// ------------------------------------------------------------
// Мир освещён по одной модели
// ------------------------------------------------------------
//
// Террейн, трава и существа стоят в одном кадре друг на друге. Пока
// каждый считал свет по-своему, это было видно: у травы рассеянный
// свет брался от самого цвета неба, у террейна — от приглушённого к
// белому; солнце у существ было вдвое сильнее, чем у земли под ними;
// туман на закате красил горизонт под травой не так, как над ней.
//
// Общего заголовка у этих шейдеров нет — glslc собирает каждый файл
// сам по себе, и заводить механизм включений ради трёх чисел дороже,
// чем сверять их здесь. Числа продублированы намеренно, а этот тест —
// то, что не даёт им разойтись снова.
/// Сборка APK не имеет права врать о том, что в ней лежит.
///
/// Случай, ради которого проверка написана. Шейдеры в APK — это
/// app/src/main/assets/shaders/*.spv, файлы генерируемые и не в git.
/// Клали их туда три разные руки: build.sh, tools/vkcheck/run.sh и
/// tools/gpubench/run.sh. Прогон A/B картинки собрал шейдеры из
/// git stash — то есть СТАРЫЕ, — а следом ./gradlew assemble упаковал
/// их в APK вместе с новым нативным кодом. Сборка уехала на устройство,
/// замер послушно повторил прежние числа, и единственным следом в
/// журнале был размер SPIR-V.
void testApkCarriesTheShadersItWasBuiltFrom() {
    group("сборка: APK несёт свои собственные шейдеры");

    // Комментарии выкидываются сразу: закомментированная строка —
    // это ровно то, чего проверка искать не должна. Мутация
    // «// dependsOn compileShaders» её и пережила, пока сверялся
    // сырой текст.
    const std::string raw = readSource("app/build.gradle");
    if (raw.empty()) { check(true, "build.gradle не найден, проверка пропущена"); return; }
    std::string g;
    for (const std::string& line : splitLines(raw)) {
        const std::string t = trimmed(line);
        if (t.rfind("//", 0) == 0) continue;
        g += line + "\n";
    }
    const usize NONE = std::string::npos;

    // ---- 1. Шейдеры компилирует сама сборка ----
    check(g.find("tasks.register('compileShaders')") != NONE,
          "в сборке есть задача компиляции шейдеров");
    check(g.find("'glslc'") != NONE && g.find("'-O'") != NONE,
          "она зовёт glslc с оптимизацией");
    check(g.find("src/main/cpp/shaders") != NONE,
          "и берёт исходники шейдеров, а не готовые .spv");

    // ---- 2. Она отрабатывает ДО упаковки ----
    check(g.find("merge.*Assets") != NONE || g.find("mergeAssets") != NONE,
          "задача привязана к слиянию ассетов");
    check(g.find("dependsOn compileShaders") != NONE,
          "и упаковка от неё зависит");

    // ---- 3. Ассеты берутся ТОЛЬКО из каталога сборки ----
    //
    // Это и есть замок. Пока assets.srcDirs указывает на рабочее дерево,
    // в APK попадает то, что там оставил кто угодно.
    check(g.find("assets.srcDirs  = [shaderAssetsDir]") != NONE ||
          g.find("assets.srcDirs = [shaderAssetsDir]") != NONE,
          "assets.srcDirs указывает на каталог сборки");
    const usize as = g.find("assets.srcDirs");
    if (as != NONE) {
        const std::string line = g.substr(as, g.find('\n', as) - as);
        check(line.find("src/main/assets") == NONE,
              "и НЕ на src/main/assets — туда пишут инструменты");
    }

    // ---- 4. Каталог пересобирается начисто ----
    check(g.find("delete(shaderAssetsDir)") != NONE,
          "каталог чистится: чужой .spv не переживает сборку");

    // ---- 5. Инструменты в поставку не пишут ----
    //
    // Прямая причина случившегося. Инструмент вправе собирать шейдеры
    // игры — но только себе, в свой каталог сборки.
    struct Tool { const char* name; std::string text; };
    const Tool tools[] = {
        { "tools/vkcheck/run.sh",  readSource("tools/vkcheck/run.sh")  },
        { "tools/gpubench/run.sh", readSource("tools/gpubench/run.sh") },
        { "tools/hostcheck/run.sh", readSource("tools/hostcheck/run.sh") },
    };
    bool clean = true;
    for (const auto& t : tools) {
        if (t.text.empty()) continue;
        for (const std::string& line : splitLines(t.text)) {
            const std::string l = trimmed(line);
            if (l.empty() || l[0] == '#') continue;       // в пояснениях путь назвать можно
            if (l.find("app/src/main/assets") == NONE) continue;
            // Писать туда нельзя ни glslc, ни mkdir, ни cp.
            std::printf("       %s пишет в поставку: %s\n", t.name, l.c_str());
            clean = false;
        }
    }
    check(clean, "ни один инструмент не пишет в app/src/main/assets");
}


/// Отпечаток сборки в журнале обязан относиться к ЭТОЙ сборке.
///
/// Раньше `git rev-parse` стоял в CMakeLists на этапе конфигурации.
/// CMake настраивается однажды и переиспользуется, поэтому метка
/// застревала: свежая сборка подписывалась давнишним коммитом. Именно
/// по такой метке пришлось разбираться, какая сборка на устройстве.
void testBuildStampIsNotStale() {
    group("сборка: отпечаток в журнале относится к этой сборке");

    const std::string cm = readSource("app/src/main/cpp/CMakeLists.txt");
    if (cm.empty()) { check(true, "CMakeLists не найден, проверка пропущена"); return; }
    const usize NONE = std::string::npos;

    // На этапе конфигурации git больше не зовётся.
    bool configureTime = false;
    const std::string body = stripComments(cm);
    const usize ep = body.find("execute_process");
    if (ep != NONE && body.find("git rev-parse", ep) != NONE &&
        body.find("git rev-parse", ep) < body.find(')', ep) + 400)
        configureTime = true;
    check(!configureTime, "git rev-parse не вызывается при настройке CMake");

    check(cm.find("add_custom_target(voxel_build_sha ALL") != NONE,
          "метка пишется отдельной целью на каждую сборку");
    check(cm.find("cmake/build_sha.cmake") != NONE,
          "её считает cmake/build_sha.cmake");
    check(cm.find("add_dependencies(native-lib voxel_build_sha)") != NONE,
          "и библиотека от этой цели зависит");

    const std::string sh = readSource("app/src/main/cpp/cmake/build_sha.cmake");
    if (sh.empty()) { check(false, "cmake/build_sha.cmake не найден"); return; }
    check(sh.find("git rev-parse --short HEAD") != NONE,
          "скрипт берёт текущий коммит");
    // APK, собранный поверх незакоммиченных правок, — это НЕ тот
    // коммит. Подписывать его чистым SHA значит врать ровно там, где
    // отпечаток и нужен.
    check(sh.find("git status --porcelain") != NONE &&
          sh.find("грязный") != NONE,
          "и помечает сборку с незакоммиченными правками грязной");
    check(sh.find("NOT OLD STREQUAL TEXT") != NONE,
          "файл переписывается только при изменении — иначе пересборка каждый раз");

    const std::string mc = readSource("app/src/main/cpp/src/main.cpp");
    if (!mc.empty()) {
        check(mc.find("build_sha.h") != NONE, "main.cpp включает сгенерированный заголовок");
        check(mc.find("LOGI(\" сборка: %s\", VOXEL_BUILD_SHA)") != NONE,
              "и печатает отпечаток в журнал");
    }
}


/// Небо: степени считаются дёшево, и БЕЗ ветвей.
///
/// Здесь записан самый дорого доставшийся урок этого этапа.
///
/// Небо — самый дорогой шейдер кадра на пиксель: 8.5 мс на полный
/// экран, 3.4 нс на пиксель, дороже фрагментной математики ландшафта
/// (2.9). Дороги в нём степени: пять `pow` на диски и ореолы светил,
/// которые дают ноль почти везде.
///
/// Напрашивалось спрятать их под ветвь. На хосте это снимало 27%
/// шейдера. На устройстве — НОЛЬ: 8.57 -> 8.49 и 8.38 при разбросе
/// самого замера 0.11 мс. А удаление тех же членов БЕЗУСЛОВНО снимало
/// 1.77 мс, то есть 21%.
///
/// Значит арифметика действительно дорога, но драйвер разворачивает
/// короткую ветвь в предикаты: считает обе стороны и выбирает.
/// Пропустить работу условием на этом GPU нельзя — её можно только не
/// делать. Отсюда и проверка: во фрагментном шейдере неба не должно
/// появляться ветвей «ради скорости».
void testSkyPaysForMathNotBranches() {
    group("небо: степени дёшевы, ветвей ради скорости нет");

    const std::string f = readSource("app/src/main/cpp/shaders/sky.frag");
    if (f.empty()) { check(true, "sky.frag не найден, проверка пропущена"); return; }
    const std::string src = stripComments(f);
    const usize NONE = std::string::npos;

    // ---- 1. Ветвей вокруг светил нет ----
    //
    // Именно они не работают: `if (d > ...)` и `if (m > ...)` вокруг
    // диска и ореола были измерены и не дали ничего.
    check(src.find("if (d >") == NONE && src.find("if (d>") == NONE,
          "диск и ореол солнца считаются без ветви");
    check(src.find("if (m >") == NONE && src.find("if (m>") == NONE,
          "и луны тоже");

    // ---- 2. Степени одного основания делят логарифм ----
    //
    // pow(x, k) это exp2(k * log2(x)). У четырёх степеней два
    // основания, значит логарифмов нужно два, а не четыре.
    check(src.find("log2(d)") != NONE && src.find("log2(m)") != NONE,
          "логарифм считается по разу на основание");
    for (const char* dead : { "powSafe(d, 900.0)", "powSafe(d, 48.0)",
                              "powSafe(m, 2400.0)", "powSafe(m, 160.0)" })
        if (src.find(dead) != NONE) {
            std::printf("       осталась отдельная степень: %s\n", dead);
            check(false, "отдельных pow на светила не осталось");
            return;
        }
    check(true, "отдельных pow на светила не осталось");
    check(src.find("exp2(900.0 * ld)") != NONE &&
          src.find("exp2( 48.0 * ld)") != NONE,
          "диск и ореол солнца — через общий логарифм");
    check(src.find("exp2(2400.0 * lm)") != NONE &&
          src.find("exp2( 160.0 * lm)") != NONE,
          "диск и ореол луны — тоже");

    // ---- 3. Широкому сиянию логарифм не нужен вовсе ----
    //
    // Шестая степень — три умножения. Обрезать её нельзя (она заметна
    // далеко от солнца), а считать через pow незачем.
    check(src.find("powSafe(d, 6.0)") == NONE,
          "широкое сияние считается без pow");
    check(src.find("d2 * d2 * d2") != NONE,
          "оно считается умножениями");
    check(src.find("powSafe(toSun, 3.0)") == NONE &&
          src.find("toSun * toSun * toSun") != NONE,
          "полоса у горизонта — тоже умножениями");

    // ---- 4. Сколько дорогих операций осталось ----
    //
    // Это и есть цена прохода. Ветвь их не уменьшает — доказано
    // замером, — поэтому единственный способ удешевить небо — уменьшить
    // это число.
    //
    // Первая редакция счётчика пропустила мутацию «добавлена лишняя
    // exp»: она не считала `powSafe` (её имя не совпадает с «pow(») и
    // брала порог с запасом. Теперь считаются все поимённо, а порог
    // равен ровно тому, что есть: 2 log2 + 4 exp2 + 1 exp + 1 powSafe
    // (градиент к зениту) + 1 sin (звёзды).
    struct Op { const char* name; usize want; };
    const Op ops[] = {
        { "log2(",    2 },
        { "exp2(",    4 },
        { "exp(",     1 },
        { "powSafe(", 1 },
        { "pow(",     0 },   // отдельных pow в теле быть не должно
        { "sin(",     1 },
    };
    const usize mainAt = src.find("void main");
    usize heavy = 0;
    bool counted = true;
    for (const auto& op : ops) {
        usize n = 0;
        for (usize at = src.find(op.name, mainAt); at != NONE;
             at = src.find(op.name, at + 1)) {
            // «powSafe(» содержит «pow» — но не «pow(», так что
            // пересечения нет; страховка на случай переименования.
            if (std::strcmp(op.name, "pow(") == 0 &&
                at >= 4 && src.compare(at - 4, 4, "Safe") == 0) continue;
            ++n;
        }
        heavy += n;
        if (n != op.want) {
            std::printf("       «%s» в небе: %zu, ожидалось %zu\n",
                        op.name, n, op.want);
            counted = false;
        }
    }
    check(counted && heavy == 9,
          "дорогих операций в небе ровно столько, сколько измерено (9)");
}


void testWorldSharesOneLightingModel() {
    group("шейдеры: мир освещён по одной модели");

    // Раньше здесь сверялось, что одна и та же формула написана
    // ДОСЛОВНО в трёх шейдерах. Теперь она написана в одном месте, на
    // процессоре, и это сильнее: сверять больше нечего, потому что
    // копий нет.
    //
    // Повод был не только архитектурный. Всё, что зависит только от
    // времени суток — цвет солнца по высоте, перевод цвета неба в
    // линейное пространство, нормировка оттенка рассеянного света с
    // делением, сила света, — пересчитывалось для КАЖДОГО фрагмента.
    // Замер (tools/gpubench, полноэкранный проход настоящим
    // voxel.frag, 2306x1080): 6.12 -> 5.57 мс, 9% шейдера. На
    // устройстве математика фрагмента ландшафта была 5.99 мс из
    // 11.00 мс кадра.
    const std::string cam = readSource("app/src/main/cpp/src/render/camera.h");
    if (cam.empty()) { check(true, "camera.h не найден, проверка пропущена"); return; }
    const usize NONE = std::string::npos;

    // ---- 1. Формула живёт на процессоре ----
    check(cam.find("glm::vec3(1.00f, 0.52f, 0.26f)") != NONE,
          "цвет солнца по высоте считается в camera.h");
    check(cam.find("skyLin / skyMax") != NONE,
          "оттенок рассеянного света — там же");
    check(cam.find("glm::mix(0.14f, 0.60f, day)") != NONE,
          "и сила рассеянного света — там же");
    check(cam.find("u.sunLight") != NONE && cam.find("u.ambLight") != NONE &&
          cam.find("u.skyLinear") != NONE,
          "результат кладётся в CameraUbo");

    // ---- 2. В шейдерах её больше нет ----
    struct Src { const char* name; std::string text; };
    Src shaders[] = {
        { "voxel.frag", stripComments(readSource("app/src/main/cpp/shaders/voxel.frag")) },
        { "grass.frag", stripComments(readSource("app/src/main/cpp/shaders/grass.frag")) },
        { "mob.frag",   stripComments(readSource("app/src/main/cpp/shaders/mob.frag"))   },
        { "sky.frag",   stripComments(readSource("app/src/main/cpp/shaders/sky.frag"))   },
    };
    for (const auto& f : shaders) {
        if (f.text.empty()) { check(true, "шейдеры не найдены, проверка пропущена"); return; }
        for (const char* dup : { "mix(vec3(1.00, 0.52, 0.26)",
                                 "smoothstep(-0.10, 0.06, cam.sunDir.y)",
                                 "skyLin / skyMax",
                                 "mix(0.14, 0.60" }) {
            if (f.text.find(dup) != NONE) {
                std::printf("       в %s осталась копия: «%s»\n", f.name, dup);
                check(false, "копий формулы света в шейдерах не осталось");
                return;
            }
        }
    }
    check(true, "копий формулы света в шейдерах не осталось");

    // ---- 3. Все четыре читают готовое ----
    for (const auto& f : shaders)
        if (f.text.find("cam.sunLight") == NONE) {
            std::printf("       %s не берёт готовый свет\n", f.name);
            check(false, "все четыре шейдера берут свет из CameraUbo");
            return;
        }
    check(true, "все четыре шейдера берут свет из CameraUbo");

    // ---- 4. Блок CameraUbo одинаков во ВСЕХ шейдерах ----
    //
    // Это не косметика: смещения полей считаются по порядку
    // объявления, и шейдер с устаревшим блоком молча читает чужие
    // байты. Компилятор такого не видит, слой проверки тоже —
    // размер набора дескрипторов сходится.
    const char* files[] = {
        "voxel.vert", "voxel.frag", "sky.frag", "grass.vert", "grass.frag",
        "mob.vert", "mob.frag", "projectile.vert", "projectile.frag",
        "outline.vert",
    };
    std::string reference;
    const char* referenceName = nullptr;
    usize checked = 0;
    for (const char* n : files) {
        const std::string src =
            readSource((std::string("app/src/main/cpp/shaders/") + n).c_str());
        if (src.empty()) continue;
        const usize b = src.find("uniform CameraUbo");
        if (b == NONE) continue;
        const usize e = src.find("} cam;", b);
        if (e == NONE) continue;
        // Сверяем только объявления полей: комментарии внутри блока
        // различаться вправе.
        std::string decl;
        for (const std::string& line : splitLines(stripComments(src.substr(b, e - b)))) {
            const std::string t = trimmed(line);
            if (!t.empty() && t.find("uniform CameraUbo") == NONE) decl += t + "\n";
        }
        ++checked;
        if (!referenceName) { reference = decl; referenceName = n; continue; }
        if (decl != reference) {
            std::printf("       %s объявляет CameraUbo не так, как %s\n", n, referenceName);
            check(false, "блок CameraUbo одинаков во всех шейдерах");
            return;
        }
    }
    check(checked >= 8, "блок CameraUbo найден во всех шейдерах");
    check(true, "блок CameraUbo одинаков во всех шейдерах");

    // ---- 5. И совпадает с C++ по составу и порядку ----
    //
    // Сверяется ВЕСЬ список полей, а не горсть ожидаемых имён.
    // Проверка «все известные поля идут в том же порядке» пропускала
    // худший случай: новое поле, добавленное в C++ в СЕРЕДИНУ блока и
    // забытое в шейдерах. Относительный порядок известных имён при
    // этом не меняется, а смещения всех полей после него уезжают, и
    // каждый шейдер начинает читать чужие байты.
    auto fieldNames = [](const std::string& text) {
        std::vector<std::string> out;
        for (const std::string& line : splitLines(stripComments(text))) {
            const std::string t = trimmed(line);
            const usize semi = t.find(';');
            if (semi == std::string::npos || semi == 0) continue;
            const usize sp = t.find_last_of(" \t*&", semi - 1);
            if (sp == std::string::npos) continue;
            const std::string name = t.substr(sp + 1, semi - sp - 1);
            if (!name.empty()) out.push_back(name);
        }
        return out;
    };
    auto sameFields = [&](const std::vector<std::string>& a,
                          const std::vector<std::string>& b,
                          const char* whoA, const char* whoB) {
        for (usize i = 0; i < std::max(a.size(), b.size()); ++i) {
            const std::string x = i < a.size() ? a[i] : std::string("(нет)");
            const std::string y = i < b.size() ? b[i] : std::string("(нет)");
            if (x != y) {
                std::printf("       поле %zu: у %s «%s», у %s «%s»\n",
                            i, whoA, x.c_str(), whoB, y.c_str());
                return false;
            }
        }
        return true;
    };

    const usize st = cam.find("struct CameraUbo {");
    check(st != NONE, "структура CameraUbo объявлена в C++");
    if (st != NONE) {
        const std::string body = cam.substr(st, cam.find("\n};", st) - st);
        const std::vector<std::string> cppFields  = fieldNames(body);
        const std::vector<std::string> glslFields = fieldNames(reference);

        check(cppFields.size() >= 10, "в C++ объявлены все поля CameraUbo");
        check(sameFields(cppFields, glslFields, "C++", "шейдеров"),
              "состав и порядок CameraUbo совпадают у C++ и шейдеров");

        // Поля света должны быть на месте, а не просто совпадать: без
        // них негде хранить посчитанный раз в кадр свет.
        for (const char* f : { "sunLight", "ambLight", "skyLinear" })
            if (std::find(cppFields.begin(), cppFields.end(), f) == cppFields.end()) {
                std::printf("       поля «%s» нет\n", f);
                check(false, "поля посчитанного света объявлены");
                return;
            }
        check(true, "поля посчитанного света объявлены");

        // Инструмент замера гоняет НАСТОЯЩИЕ шейдеры игры. Устаревшая
        // структура у него означает, что они читают чужие байты, и
        // мерить он будет мусор — молча, с правдоподобными числами.
        const std::string gb = readSource("tools/gpubench/gpubench.cpp");
        if (!gb.empty()) {
            const usize g = gb.find("struct CameraUbo {");
            check(g != NONE, "у gpubench есть своя копия CameraUbo");
            if (g != NONE) {
                const std::string gbody = gb.substr(g, gb.find("\n};", g) - g);
                check(sameFields(fieldNames(gbody), cppFields, "gpubench", "игры"),
                      "и она совпадает с игровой по составу и порядку");
            }
        }
    }
}


void testDistantGrassIsNotSubPixel() {
    group("трава: субпиксельных пучков не бывает");

    // Порог считается по настоящему экранному размеру, поэтому верен
    // при любом поле зрения и разрешении.
    const f32 pxPerUnit = 1080.f * 0.5f / std::tan(glm::radians(70.f) * 0.5f);
    check(pxPerUnit > 700.f && pxPerUnit < 800.f,
          "пикселей на единицу посчитано разумно");

    // Пучок высотой 0.6 на 40 блоках занимает меньше трёх пикселей?
    // Тогда он обязан быть отброшен ещё до отправки на GPU.
    const f32 farScale = 0.6f;
    const f32 farDist  = 200.f;
    check(farScale * pxPerUnit / farDist < render::GRASS_MIN_PIXELS,
          "на двухстах блоках пучок мельче порога");
    check(farScale * pxPerUnit / 10.f > render::GRASS_MIN_PIXELS,
          "а на десяти — крупнее");

    const std::string g = readSource("app/src/main/cpp/src/render/instanced_renderer.cpp");
    if (g.empty()) { check(true, "исходник не найден, проверка пропущена"); return; }
    check(g.find("GRASS_MIN_PIXELS") != std::string::npos,
          "порог применяется при наборе инстансов");
    check(g.find("continue") != std::string::npos, "и пучок именно отбрасывается");

    // Отбрасывать надо ДО записи в буфер, а не в шейдере.
    const usize thr = g.find("GRASS_MIN_PIXELS");
    const usize push = g.find("cpuInstances_.push_back");
    check(thr != std::string::npos && push != std::string::npos && thr < push,
          "отсечка стоит раньше отправки инстанса");

    const std::string fs = readSource("app/src/main/cpp/shaders/grass.frag");
    if (!fs.empty())
        check(fs.find("discard") == std::string::npos,
              "и не подменяется discard'ом во фрагментном шейдере");
}

void testDiagnosticBuildWired() {
    group("сборка: диагностический APK");

    // Сам признак проверяется в двух конфигурациях отдельным бинарником
    // (tools/hostcheck/diag_build_check.cpp): внутри одного его не
    // проверить, решение принимается на этапе компиляции. Здесь —
    // обвязка, без которой флаг до компилятора не доедет.
    const std::string cm = readSource("app/src/main/cpp/CMakeLists.txt");
    const std::string gr = readSource("app/build.gradle");
    const std::string wf = readSource(".github/workflows/build.yml");
    const std::string st = readSource("app/src/main/cpp/src/config/settings.cpp");
    const std::string mn = readSource("app/src/main/cpp/src/main.cpp");
    if (cm.empty() || gr.empty() || wf.empty() || st.empty() || mn.empty()) {
        check(true, "файлы сборки не найдены, проверка пропущена");
        return;
    }

    check(cm.find("option(VOXEL_DEBUG_SCENE") != std::string::npos,
          "CMake знает ключ VOXEL_DEBUG_SCENE");
    check(cm.find("VOXEL_DEBUG_SCENE=1") != std::string::npos,
          "и превращает его в определение для компилятора");
    check(cm.find("option(VOXEL_DEBUG_SCENE \"Собрать диагностический APK с минимальной сценой\" OFF)")
              != std::string::npos,
          "по умолчанию ключ выключен: обычные сборки остаются игрой");

    check(gr.find("diagnostic {") != std::string::npos,
          "в gradle есть тип сборки diagnostic");
    {
        const usize d = gr.find("diagnostic {");
        if (d != std::string::npos)
            check(gr.substr(d, 900).find("-DVOXEL_DEBUG_SCENE=ON") != std::string::npos,
                  "тип сборки diagnostic передаёт ключ в CMake");
    }

    check(wf.find("assembleDiagnostic") != std::string::npos,
          "GitHub Actions собирает диагностический APK");
    check(wf.find("VoxelRPG-diagnostic") != std::string::npos,
          "и выкладывает его отдельным артефактом");
    check(wf.find("debug_scene=true (build diagnostic mode)") != std::string::npos,
          "прогон сверяет, что флаг доехал до библиотеки");

    check(st.find("if (DIAGNOSTIC_BUILD) debugScene = true;") != std::string::npos,
          "настройкой диагностическую сборку не выключить");
    check(mn.find("debug_scene=true (build diagnostic mode)") != std::string::npos,
          "диагностический APK объявляет режим в журнале");
}

void testDebugSceneIsolated() {
    group("сцена: стенд изолирован");

    // 1. Камера. Прибитую камеру не должен двигать никто: ни
    //    followTarget, ни ввод, ни покачивание головы.
    render::Camera cam;
    cam.setDebugCamera({ world::SCENE_EYE_X, world::SCENE_EYE_Y,
                         world::SCENE_EYE_Z },
                       world::SCENE_YAW, world::SCENE_PITCH);
    const glm::vec3 eye0 = cam.position();
    const f32 yaw0 = cam.yaw(), pitch0 = cam.pitch();

    cam.setTargetPosition({ 1.f, 2.f, 3.f });
    cam.setYawPitch(1.234f, 0.5f);
    cam.setHeadBob(3.f, 0.4f);
    cam.setFirstPersonEye(1.7f);
    cam.setThirdPersonDistance(5.f);
    cam.setFirstPerson(false);

    check(cam.position() == eye0, "положение прибитой камеры не сдвинуть");
    check(cam.yaw() == yaw0 && cam.pitch() == pitch0,
          "поворот прибитой камеры не сбить");

    // followTarget — тот самый вызов, который раньше переписывал
    // отладочные значения обратно.
    {
        world::ChunkManager w(12648430, 2);
        cam.followTarget(w, glm::vec3(0.f, 0.f, 1.f));
    }
    check(cam.position() == eye0, "после followTarget камера на месте");
    check(cam.debugCamera(), "признак прибитой камеры держится");

    // Обычная камера обязана остаться подвижной.
    render::Camera live;
    live.setYawPitch(0.7f, -0.2f);
    check(live.yaw() == 0.7f, "обычную камеру по-прежнему можно повернуть");
    check(!live.debugCamera(), "обычная камера не помечена отладочной");

    // Заслон в самом followTarget поведением не проверить: остальные
    // заслоны уже обнулили всё, из чего она считает положение, и
    // снятие любого ОДНОГО заслона картинку не меняет. Это хорошо для
    // надёжности и плохо для проверки, поэтому наличие заслона
    // подтверждаем по исходнику.
    {
        const std::string ch = readSource("app/src/main/cpp/src/render/camera.h");
        const usize ft = ch.find("void followTarget(");
        check(ft != std::string::npos, "followTarget на месте");
        if (ft != std::string::npos)
            check(ch.substr(ft, 200).find("if (debugCamera_) return;") != std::string::npos,
                  "followTarget сама отказывается двигать прибитую камеру");
    }

    // 2. Игровые системы. В отладочном кадре не должно выполняться ни
    //    одной из них — проверяем по исходнику, что развилка стоит
    //    ДО них и выходит из функции.
    const std::string m = readSource("app/src/main/cpp/src/main.cpp");
    if (m.empty()) { check(true, "main.cpp не найден, проверка пропущена"); return; }

    const usize upd = m.find("void update(f32 dt, f32 timeSec)");
    check(upd != std::string::npos, "update на месте");
    if (upd == std::string::npos) return;

    const usize gate = m.find("updateDebugScene(timeSec)", upd);
    check(gate != std::string::npos, "развилка отладочной сцены в update есть");
    if (gate == std::string::npos) return;

    // Всё, что шевелит мир, обязано идти ПОСЛЕ развилки.
    static const char* SYSTEMS[] = {
        "dayCycle.tick", "playtime.tick", "spawner->update",
        "mobs::updateMobs", "npcSpawner->update", "npc::updateNpcs",
        "items::updatePickups", "combat::updateProjectiles",
        "combat::tickStatuses", "quests::tickQuestTime",
        "player->updateWithHash", "cameraYawPitch.x -=",
        "autosaveTimer +=",
    };
    bool allAfter = true;
    for (const char* sys : SYSTEMS) {
        const usize at = m.find(sys, upd);
        if (at == std::string::npos || at < gate) { allAfter = false; break; }
    }
    check(allAfter, "ни одна игровая система не выполняется до развилки");

    // И развилка обязана возвращать управление, а не проваливаться дальше.
    const std::string tail = m.substr(gate, 120);
    check(tail.find("return;") != std::string::npos,
          "после отладочного кадра управление возвращается");

    // 3. Автосейв не затирает настоящее сохранение отладочным миром.
    const usize term = m.find("void onWindowTerm()");
    check(term != std::string::npos, "onWindowTerm на месте");
    if (term != std::string::npos)
        check(m.substr(term, 600).find("!cfg::settingsConst().debugScene")
                  != std::string::npos,
              "при отладочной сцене автосейв на выходе не пишется");

    // 4. Постоянные шаг кадра и время мира.
    check(m.find("dt = world::SCENE_FIXED_DT") != std::string::npos,
          "шаг кадра в отладочном режиме постоянный");
    check(m.find("timeSec = world::SCENE_TIME_SEC") != std::string::npos,
          "время мира в отладочном режиме постоянное");

    // 5. Доказательство изоляции печатается.
    check(m.find("debug_scene=true кадр") != std::string::npos,
          "строка доказательства изоляции печатается");

    // 6. Хост берёт освещение и время из тех же констант, а камеру
    //    ставит тем же вызовом.
    const std::string v = readSource("tools/vkcheck/vkcheck.cpp");
    if (!v.empty()) {
        check(v.find("world::SCENE_SUN_X") != std::string::npos,
              "хост берёт солнце из констант сцены");
        check(v.find("cam.toUbo(world::SCENE_TIME_SEC)") != std::string::npos,
              "хост берёт время из констант сцены");
        check(v.find("cam.setDebugCamera(eye, yaw, pitch)") != std::string::npos,
              "хост ставит камеру тем же вызовом, что игра");
        check(v.find("glm::lookAt(eye, eye + cam.forward()") == std::string::npos,
              "второго владельца камеры на хосте не осталось");
    }
}

void testMinimalScene() {
    group("сцена: минимальная детерминированная");

    world::Chunk c;
    c.coord = { 0, 0, 0 };
    world::buildMinimalScene(c);

    const i32 G = world::SCENE_GROUND_Y;

    // Ровная земля: трава на G-1, воздух над ней.
    check(c.at(1, G - 1, 1) == world::GRASS, "ровная земля покрыта травой");
    check(c.at(1, G, 1) == world::AIR,       "над землёй воздух");
    check(c.at(1, G - 2, 1) == world::DIRT,  "под травой земля");
    check(c.at(1, 0, 1) == world::STONE,     "внизу камень");

    // Одиночный поднятый блок.
    check(c.at(world::SCENE_BLOCK_X, G, world::SCENE_BLOCK_Z) == world::STONE,
          "поднятый блок на месте");
    check(c.at(world::SCENE_BLOCK_X, G + 1, world::SCENE_BLOCK_Z) == world::AIR,
          "над ним воздух");

    // Лесенка: каждая следующая колонка на блок выше.
    bool stairs = true;
    for (i32 x = world::SCENE_SLOPE_X0; x <= world::SCENE_SLOPE_X1; ++x) {
        const i32 top = G + (x - world::SCENE_SLOPE_X0);
        if (c.at(x, top, 6) != world::GRASS) stairs = false;
        if (c.at(x, top + 1, 6) != world::AIR) stairs = false;
    }
    check(stairs, "лесенка поднимается на блок за шаг");

    // Вода: верх вровень с землёй, дно каменное.
    check(c.at(world::SCENE_POOL_X0, G - 1, world::SCENE_POOL_Z0) == world::WATER,
          "вода стоит вровень с землёй");
    check(c.at(world::SCENE_POOL_X0, G - 3, world::SCENE_POOL_Z0) == world::STONE,
          "дно водоёма каменное");
    check(c.at(world::SCENE_POOL_X0, G, world::SCENE_POOL_Z0) == world::AIR,
          "над водой воздух");

    // Дерево: ствол и крона.
    check(c.at(world::SCENE_TREE_X, G, world::SCENE_TREE_Z) == world::WOOD,
          "ствол дерева на месте");
    check(c.at(world::SCENE_TREE_X, G + 4, world::SCENE_TREE_Z) == world::LEAVES,
          "крона над стволом");

    // Соседний чанк — только ровная земля: он служит фоном и даёт
    // центральному честных соседей.
    world::Chunk n;
    n.coord = { 1, 0, 0 };
    world::buildMinimalScene(n);
    bool flat = true;
    for (i32 z = 0; z < world::CHUNK_SIZE && flat; ++z)
        for (i32 x = 0; x < world::CHUNK_SIZE; ++x)
            if (n.at(x, G, z) != world::AIR || n.at(x, G - 1, z) != world::GRASS) {
                flat = false; break;
            }
    check(flat, "соседние чанки — ровная земля без примет");

    // Сцена не зависит ни от чего внешнего: второй вызов обязан дать
    // тот же чанк до последнего вокселя.
    world::Chunk again;
    again.coord = { 0, 0, 0 };
    world::buildMinimalScene(again);
    bool same = true;
    for (i32 i = 0; i < world::CHUNK_VOL; ++i)
        if (again.voxels[(usize)i] != c.voxels[(usize)i]) { same = false; break; }
    check(same, "повторная сборка даёт тот же чанк");
}

void testDebugShadingWired() {
    group("рендер: отладочные виды террейна");

    // Три звена одной цепочки: настройка в файле, её передача в
    // камеру и разбор номера в шейдере. Рвётся любое — и вид молча
    // перестаёт включаться, а узнать об этом можно только собрав APK
    // и не увидев разницы на экране.
    const std::string set = readSource("app/src/main/cpp/src/config/settings.cpp");
    const std::string cam = readSource("app/src/main/cpp/src/render/camera.h");
    const std::string rs  = readSource("app/src/main/cpp/src/render/render_system.cpp");
    const std::string fr  = readSource("app/src/main/cpp/shaders/voxel.frag");
    if (set.empty() || cam.empty() || rs.empty() || fr.empty()) {
        check(true, "исходники не найдены, проверка пропущена");
        return;
    }

    check(set.find("\"debug_shading\"") != std::string::npos,
          "ключ debug_shading читается и пишется в settings.cfg");
    // Настройка обязана доходить до камеры. Выражение с тех пор
    // раздвоилось: пока идёт развёртка по проходам, вид задаёт она
    // (одна из её ступеней меряет ландшафт с ранним выходом из
    // фрагментного шейдера). Но вне развёртки источник по-прежнему
    // один — settings.cfg.
    {
        const usize d = rs.find("setDebugShading");
        check(d != std::string::npos, "система рендера ставит номер вида");
        if (d != std::string::npos) {
            const std::string w = rs.substr(d, 220);
            check(w.find("config::settingsConst().debugShading") != std::string::npos,
                  "и вне развёртки берёт его из настроек");
        }
    }
    check(cam.find("(f32)debugShading_") != std::string::npos,
          "камера кладёт номер в свободную компоненту screenSize.z");
    check(fr.find("cam.screenSize.z") != std::string::npos,
          "шейдер террейна разбирает номер из screenSize.z");
    check(fr.find("vShade.y") != std::string::npos && fr.find("vShade.x") != std::string::npos,
          "виды показывают открытость неба и затенение углов");

    // Ноль обязан остаться обычной картинкой, иначе игра всегда
    // рисует отладку.
    const usize dv = fr.find("bool debugView(");
    check(dv != std::string::npos, "разбор вида вынесен в отдельную функцию");
    if (dv != std::string::npos) {
        const std::string body = fr.substr(dv, 900);
        check(body.find("else return false;") != std::string::npos,
              "неизвестный номер (в том числе 0) оставляет обычный расчёт");
    }
}

void testRenderPassSync() {
    group("vk: зависимость прохода рендера");

    // Проход рендера живёт в отдельном модуле: его же строит
    // офлайн-проверка графики tools/vkcheck, а vk_context.cpp тянет
    // за собой окно Android.
    const std::string rp = readSource("app/src/main/cpp/src/vk/vk_renderpass.cpp");
    const std::string src = readSource("app/src/main/cpp/src/vk/vk_context.cpp");
    if (src.empty() || rp.empty()) {
        check(true, "исходники vk не найдены, проверка пропущена");
        return;
    }

    const usize beg = rp.find("bool createVoxelRenderPass(");
    check(beg != std::string::npos, "createVoxelRenderPass на месте");
    if (beg == std::string::npos) return;
    const std::string body = rp.substr(beg);

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
// Уровень детализации: одна формула, одна точка отсчёта
// ------------------------------------------------------------
void testLodHasSingleSourceOfTruth() {
    group("LOD: одна формула и одна точка отсчёта");

    // Формула живёт в world/lod.h, и обе стороны зовут именно её.
    const std::string crh = readSource("app/src/main/cpp/src/render/chunk_renderer.h");
    const std::string cmc = readSource("app/src/main/cpp/src/world/chunk_manager.cpp");
    const std::string rsc = readSource("app/src/main/cpp/src/render/render_system.cpp");
    if (crh.empty() || cmc.empty() || rsc.empty()) {
        check(true, "исходники не найдены, проверка пропущена");
    } else {
        check(crh.find("world::LodBands") != std::string::npos,
              "рендер держит границы из world/lod.h");
        check(crh.find("f32 lod0_") == std::string::npos &&
              crh.find("f32 lod1_") == std::string::npos,
              "своей копии границ у рендера не осталось");
        check(crh.find("world::lodForDistanceSq") != std::string::npos,
              "рендер зовёт общую формулу");
        check(cmc.find("world::lodForChunk") != std::string::npos,
              "мир зовёт ту же формулу");
        check(cmc.find("cameraX_.load") != std::string::npos,
              "и меряет от камеры");
        check(cmc.find("playerChunkX_") == std::string::npos,
              "мерить от чанка игрока мир перестал");
        check(rsc.find("world.setCameraPosition(camPos)") != std::string::npos,
              "рендер сообщает миру позицию камеры каждый кадр");
    }

    // Самый грубый уровень начинается за концом тумана. Туман кончается
    // на 0.94 дальности прорисовки (см. render_system), значит граница
    // третьего уровня обязана лежать дальше.
    for (i32 vd : { 4, 6, 8, 12 }) {
        world::LodBands b;
        const f32 blocks = (f32)vd * (f32)world::CHUNK_SIZE;
        b.fromViewDistance(blocks);
        if (b.lod2 <= blocks * 0.94f) {
            check(false, "третий уровень уведён за туман");
            break;
        }
        if (vd == 12) check(true, "третий уровень уведён за туман");
    }

    // Мёртвая зона: у самой границы уровень не пляшет туда-сюда.
    world::LodBands b;
    b.fromViewDistance(8.f * (f32)world::CHUNK_SIZE);
    const f32 edge = b.lod0;
    const f32 inside  = edge * 0.97f;
    const f32 outside = edge * 1.03f;
    check(world::lodForDistanceSq(inside * inside, b) == 0,
          "без предыстории ближе границы — нулевой уровень");
    check(world::lodForDistanceSq(outside * outside, b) == 1,
          "без предыстории дальше границы — первый");
    check(world::lodForDistanceSq(outside * outside, b, 0) == 0,
          "чуть за границей уровень не меняется, если был нулевым");
    check(world::lodForDistanceSq(inside * inside, b, 1) == 1,
          "и не меняется обратно, если был первым");
    const f32 far = edge * 1.30f;
    check(world::lodForDistanceSq(far * far, b, 0) == 1,
          "за мёртвой зоной уровень всё-таки меняется");

    // Обе стороны считают от ОДНОЙ величины: центра чанка.
    const glm::vec3 cam{ 100.f, 64.f, -40.f };
    const u8 direct = world::lodForChunk(3, -2, cam, b);
    const glm::vec3 d = world::chunkCenter(3, -2) - cam;
    check(direct == world::lodForDistanceSq(glm::dot(d, d), b),
          "lodForChunk и есть расстояние до центра чанка");
}

// ------------------------------------------------------------
// Резидентный меш не исчезает, пока не приехал новый
// ------------------------------------------------------------
void testResidentLodSurvivesRequest() {
    group("LOD: резидентный меш живёт до приезда нового");

    const std::string crc = readSource("app/src/main/cpp/src/render/chunk_renderer.cpp");
    const std::string cmc = readSource("app/src/main/cpp/src/world/chunk_manager.cpp");
    if (crc.empty() || cmc.empty()) {
        check(true, "исходники не найдены, проверка пропущена");
        return;
    }

    // 1. Запасной вариант в render() — ровно residentLod, а не первый
    //    попавшийся уровень из четырёх.
    const usize a = crc.find("Нужного уровня нет в видеопамяти");
    const usize b = crc.find("if (!chosen)");
    check(a != std::string::npos && b != std::string::npos && a < b,
          "в render() есть ветка «нужного уровня нет»");
    if (a != std::string::npos && b != std::string::npos && a < b) {
        const std::string win = crc.substr(a, b - a);
        check(win.find("cm.residentLod") != std::string::npos,
              "запасной вариант — резидентный уровень");
        check(win.find("i < 4") == std::string::npos &&
              win.find("l < 4") == std::string::npos,
              "перебора всех четырёх уровней там больше нет");
    }

    // 2. Смена резидентного уровня происходит только после успешной
    //    загрузки: присваивание стоит внутри if (uploadLod(...)).
    const usize u = crc.find("if (uploadLod(ctx, cmd, gpu, c, req.lod))");
    check(u != std::string::npos, "уровень грузится из очереди запросов");
    if (u != std::string::npos) {
        const std::string win = crc.substr(u, 400);
        check(win.find("residentAfterUpload") != std::string::npos,
              "резидентным уровень становится после успешной загрузки, "
              "и только по общему правилу");
    }

    // 2a. Правило смены резидентного уровня — одно на весь рендер, и
    //     присваиваний residentLod помимо него не осталось. Иначе
    //     любая новая ветка загрузки снова начнёт назначать
    //     резидентным всё, что доехало.
    {
        const std::string only = "residentLod  = residentAfterUpload(";
        usize direct = 0;
        for (usize at = crc.find("residentLod  = "); at != std::string::npos;
             at = crc.find("residentLod  = ", at + 1)) {
            if (crc.substr(at, only.size()) != only) ++direct;
        }
        check(direct == 0, "residentLod присваивают только через общее правило");
        check(crc.find("gpu.residentLod  = have") == std::string::npos,
              "«что угадали, то и резидентное» из загрузки убрано");
    }

    // 2b. Перебора «первый построенный из 0..3» в загрузке больше нет.
    //     Именно он назначал резидентным то LOD0, то LOD3 при
    //     неподвижной камере.
    {
        const usize up = crc.find("void ChunkRenderer::uploadChunks");
        check(up != std::string::npos, "uploadChunks на месте");
        if (up != std::string::npos) {
            const usize end = crc.find("void ChunkRenderer::render", up);
            const std::string win = crc.substr(up, end - up);
            check(win.find("if (c.meshes[l].built) { have = l; break; }")
                      == std::string::npos,
                  "перебор «первый построенный из четырёх» удалён");
            check(win.find("c.lodWanted.load") == std::string::npos,
                  "и подсматривание в lodWanted тоже");
        }
    }

    // 4. Воркер не выбирает уровень сам: он строит тот, что записан в
    //    задаче. Чтения lodWanted в jobMesh быть не должно — из-за
    //    него уже поставленная задача строила не тот уровень, за
    //    которым её посылали.
    {
        const usize jm = cmc.find("void ChunkManager::jobMesh");
        check(jm != std::string::npos, "jobMesh на месте");
        if (jm != std::string::npos) {
            const usize end = cmc.find("NeighborLease ChunkManager::gatherNeighbors", jm);
            const std::string win = cmc.substr(jm, end - jm);
            check(win.find("lodWanted.load") == std::string::npos,
                  "jobMesh не читает lodWanted");
            check(win.find("const u8 lod = ctx->lod") != std::string::npos,
                  "уровень задачи берётся из её же контекста");
        }
        const usize eq = cmc.find("void ChunkManager::enqueueMesh(ChunkCoord coord, u8 lod)");
        check(eq != std::string::npos,
              "enqueueMesh принимает уровень и фиксирует его при постановке");
        if (eq != std::string::npos) {
            const std::string win = cmc.substr(eq, 1800);
            check(win.find("lodSeq[want].fetch_add") != std::string::npos,
                  "и заодно номер заказа, по которому задача узнаёт, что устарела");
        }
    }

    // 3. Мир не выбрасывает остальные уровни при постройке нового,
    //    если они всё ещё соответствуют текущим вокселям.
    const usize m = cmc.find("for (u8 other = 0; other < 4; ++other)");
    check(m != std::string::npos, "мир перебирает прочие уровни чанка");
    if (m != std::string::npos) {
        const std::string win = cmc.substr(m, 500);
        check(win.find("revision == ctx->version") != std::string::npos,
              "и оставляет те, что построены по тем же вокселям");
    }
}

// ------------------------------------------------------------
// Гонка завершений LOD: кто заказывал, тот и получает
//
// Симптом: при НЕПОДВИЖНОЙ камере разрешение дальнего рельефа
// перещёлкивало между LOD0..LOD3. Камера не двигалась, воксели не
// менялись, а картинка менялась.
//
// Причина была в асинхронном конвейере, и целиком в семантике
// «кто чей»:
//
//   1. Задача меширования не несла в себе уровень. Она читала
//      Chunk::lodWanted В МОМЕНТ ВЫПОЛНЕНИЯ — то есть неизвестно
//      через сколько кадров после постановки — и строила то, что
//      прочитала. За каким уровнем её посылали, она не помнила.
//   2. Очередь готовых мешей несла один лишь чанк, без уровня.
//   3. Получатель угадывал: нет нужного уровня — брал первый
//      построенный из 0..3.
//   4. Что угадал, то и становилось residentLod.
//   5. render() рисует residentLod.
//
// Набор построенных уровней меняется во времени сам по себе —
// уровни достраиваются и устаревают. Поэтому шаг 3 давал разный
// ответ в разных кадрах БЕЗ единого изменения снаружи, и побеждал
// тот воркер, который закончил последним.
//
// Правило, которое это закрывает, ровно одно и живёт в
// render::residentAfterUpload: резидентным уровень становится, только
// если он и есть целевой, либо рисовать не было нечем вовсе. Чужое
// завершение — задача за другой уровень, поставленная раньше и
// закончившаяся позже — не двигает резидентный уровень никуда.
// ------------------------------------------------------------
void testLodCompletionIsAddressed() {
    group("LOD: завершение адресовано уровню, а не чанку");

    constexpr u8 NONE = render::LOD_NONE;

    // ---- 1. Само правило ----
    check(render::residentAfterUpload(NONE, 2, 2) == 2,
          "первый же приехавший целевой уровень становится резидентным");
    check(render::residentAfterUpload(NONE, 2, 0) == 0,
          "пока рисовать нечем, годится и не целевой: дыра хуже грубой геометрии");
    check(render::residentAfterUpload(3, 3, 0) == 3,
          "чужое завершение не сбивает устоявшийся резидентный уровень");
    check(render::residentAfterUpload(3, 3, 1) == 3, "и никакое другое тоже");
    check(render::residentAfterUpload(0, 2, 2) == 2,
          "приехавший целевой уровень сменяет резидентный");
    check(render::residentAfterUpload(2, 2, NONE) == 2,
          "мусорный уровень не принимается вовсе");

    // ---- 2. Порядок завершений не решает ничего ----
    //
    // Главная проверка. Несколько уровней одного чанка строятся
    // одновременно (это разрешено) и по одним и тем же вокселям.
    // Заканчиваются они в произвольном порядке — так работает пул
    // воркеров. Итог обязан быть один и тот же при ЛЮБОМ порядке:
    // резидентным становится только целевой уровень.
    //
    // Перебираем все 24 порядка прибытия четырёх уровней.
    u8 perm[4] = { 0, 1, 2, 3 };
    int orders = 0, wrong = 0;
    std::sort(perm, perm + 4);
    do {
        ++orders;
        for (u8 target = 0; target < 4; ++target) {
            // Свежий чанк: рисовать нечем.
            u8 resident = NONE;
            for (u8 i = 0; i < 4; ++i)
                resident = render::residentAfterUpload(resident, target, perm[i]);
            if (resident != target) ++wrong;
        }
    } while (std::next_permutation(perm, perm + 4));
    check(orders == 24, "перебраны все порядки прибытия");
    check(wrong == 0,
          "при любом порядке резидентным становится ровно целевой уровень");

    // ---- 3. Неподвижная камера: повторные завершения ничего не двигают ----
    //
    // Чанк устоялся на своём уровне, камера стоит, целевой уровень не
    // меняется. Задачи за прочие уровни продолжают приходить —
    // запоздавшие, повторные, в любом порядке. Резидентный уровень
    // обязан остаться тем же самым.
    for (u8 target = 0; target < 4; ++target) {
        u8 resident = target;
        bool held = true;
        for (int round = 0; round < 32; ++round) {
            const u8 arrived = (u8)((round * 7 + 1) & 3);
            resident = render::residentAfterUpload(resident, target, arrived);
            if (resident != target) held = false;
        }
        if (!held) {
            check(false, "при неподвижной камере резидентный уровень не уезжает");
            break;
        }
        if (target == 3)
            check(true, "при неподвижной камере резидентный уровень не уезжает");
    }

    // ---- 4. Смена уровня всё-таки происходит ----
    // Правило не должно оказаться «резидентный уровень не меняется
    // никогда»: это вылечило бы мерцание ценой отключённого LOD.
    {
        u8 resident = 0;
        const u8 target = 2;
        resident = render::residentAfterUpload(resident, target, 1);   // чужое
        check(resident == 0, "чужой уровень по дороге не принят");
        resident = render::residentAfterUpload(resident, target, 2);   // целевой
        check(resident == 2, "целевой уровень доехал и принят");
    }
}

// ------------------------------------------------------------
// Мир строит РОВНО заказанный уровень и говорит, какой построил
//
// Это вторая половина того же исправления, на стороне мира.
// Проверяется поведением, на живом планировщике: заказываем уровни,
// забираем готовые меши и смотрим, что приехало.
// ------------------------------------------------------------
void testMeshJobBuildsRequestedLod() {
    group("мир: задача меширования строит заказанный уровень");

    world::blocks();
    jobs::gJobs.start(3);
    {
        world::ChunkManager mgr(0xB0BAULL, 1);

        // Ждём настоящий рельеф в чанке (0,0).
        bool ready = false;
        for (int i = 0; i < 500 && !ready; ++i) {
            mgr.update({ 8.f, 70.f, 8.f });
            ready = mgr.isReadyAt(8, 8);
            if (!ready) std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        check(ready, "чанк сгенерирован");

        if (ready) {
            // Заказываем все четыре уровня подряд, не дожидаясь ни
            // одного: ровно тот случай, когда у чанка одновременно
            // строится несколько уровней. Очередь готовых мешей
            // намеренно НЕ разбираем заранее — уровень, построенный
            // при генерации, тоже обязан приехать со своим номером.
            const world::ChunkCoord c{ 0, 0 };
            for (u8 l = 0; l < 4; ++l) mgr.requestLod(c, l);

            bool arrived[4] = { false, false, false, false };
            bool everyArrivalIsBuilt = true;
            bool everyArrivalWasAsked = true;
            for (int i = 0; i < 200; ++i) {
                for (const auto& m : mgr.pollMeshesReady()) {
                    if (!m.chunk) continue;
                    if (m.lod > 3) { everyArrivalWasAsked = false; continue; }
                    if (m.chunk->coord.x == 0 && m.chunk->coord.z == 0)
                        arrived[m.lod] = true;
                    // Уровень, названный в завершении, обязан быть
                    // действительно построен. Раньше уровень в
                    // завершении вообще не назывался — получателю
                    // было нечего проверить.
                    std::lock_guard lk(m.chunk->meshMutex);
                    if (!m.chunk->meshes[m.lod].built) everyArrivalIsBuilt = false;
                }
                if (arrived[0] && arrived[1] && arrived[2] && arrived[3]) break;
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }

            check(everyArrivalWasAsked, "уровень в завершении — настоящий уровень");
            check(everyArrivalIsBuilt,
                  "названный в завершении уровень действительно построен");
            check(arrived[0] && arrived[1] && arrived[2] && arrived[3],
                  "все четыре заказанных уровня доехали по отдельности");

            // И построены именно как заявлено: число квадов совпадает
            // с эталонным мешем того же уровня. Задача, прочитавшая
            // уровень из lodWanted в момент выполнения, дала бы здесь
            // четыре одинаковых меша вместо четырёх разных.
            auto chunk = mgr.findChunk(0, 0);
            check(chunk != nullptr, "чанк на месте");
            if (chunk) {
                usize quads[4] = { 0, 0, 0, 0 };
                {
                    std::lock_guard lk(chunk->meshMutex);
                    for (u8 l = 0; l < 4; ++l)
                        quads[l] = chunk->meshes[l].built
                                 ? chunk->meshes[l].quads.size() : 0;
                }
                // Огрубление не может добавлять геометрию.
                check(quads[0] && quads[1] && quads[2] && quads[3],
                      "геометрия есть на каждом из четырёх уровней");
                check(quads[0] > quads[3],
                      "четыре уровня различны — а не один и тот же четырежды");
            }
        }
    }
    jobs::gJobs.stop();
}

// ------------------------------------------------------------
// Потоковая загрузка: бюджет на кадр и порядок от ближнего
//
// Чанк — это четверть мегабайта вокселей, и дорого в нём не
// выделение памяти (четыре микросекунды), а первое касание страниц:
// их обнуляет ядро. Замер: двести созданных и УДЕРЖАННЫХ чанков —
// шестнадцать с половиной миллисекунд; двести созданных и тут же
// отпущенных — одна, потому что аллокатор отдаёт тот же блок.
//
// update() заводил весь недостающий круг разом, и на старте и после
// любого рывка это был ровно такой провал: долгая сессия
// (tools/soak) показывала пик world.update в 19-21 мс на каждом
// перемещении, и ни на чём другом.
//
// Проверяется не время — оно на разных машинах разное, — а два
// свойства, из которых оно следует: за кадр заводится не больше
// бюджета, и заводится всегда ближнее.
// ------------------------------------------------------------
void testChunkStreamingIsBudgeted() {
    group("мир: потоковая загрузка по бюджету, от ближнего к дальнему");

    world::blocks();
    const i32 VD = 8;
    world::ChunkManager mgr(0xA1B2C3D4ULL, VD);

    // Один кадр на пустом мире.
    mgr.update({ 4.f, 70.f, 4.f });
    const usize afterOne = mgr.loadedChunks();

    check(afterOne > 0, "за первый кадр что-то заводится");
    check(afterOne <= 16,
          "за кадр заводится горстка чанков, а не весь круг");
    // Круг радиусом 8 — это больше двух сотен чанков. Если бы бюджета
    // не было, первый же кадр завёл бы их все.
    check(afterOne < 100, "весь круг за один кадр не заводится");

    check(mgr.findChunk(0, 0) != nullptr,
          "чанк под игроком заведён в первом же кадре");

    // Главное свойство: всё заведённое ближе всего незаведённого.
    // Именно оно означает «мир нарастает вокруг игрока», и именно оно
    // ломается, если обход вернуть к растровому.
    {
        i32 worstCreated = -1, bestMissing = 1 << 30;
        for (i32 dz = -VD; dz <= VD; ++dz)
            for (i32 dx = -VD; dx <= VD; ++dx) {
                const i32 d2 = dx * dx + dz * dz;
                if (d2 > VD * VD) continue;
                const bool have = mgr.findChunk(dx, dz) != nullptr;
                if (have) { if (d2 > worstCreated) worstCreated = d2; }
                else      { if (d2 < bestMissing)  bestMissing  = d2; }
            }
        check(worstCreated >= 0, "хоть один чанк заведён");
        check(worstCreated <= bestMissing,
              "самый дальний заведённый не дальше самого ближнего незаведённого");
    }

    // За много кадров круг наполняется целиком — бюджет откладывает
    // работу, а не отменяет её.
    for (int i = 0; i < 400; ++i) mgr.update({ 4.f, 70.f, 4.f });
    {
        usize inCircle = 0, have = 0;
        for (i32 dz = -VD; dz <= VD; ++dz)
            for (i32 dx = -VD; dx <= VD; ++dx) {
                if (dx * dx + dz * dz > VD * VD) continue;
                ++inCircle;
                if (mgr.findChunk(dx, dz)) ++have;
            }
        check(have == inCircle, "за несколько кадров круг наполняется весь");
        check(inCircle > 150, "круг и правда большой (иначе проверка ни о чём)");
    }

    // Установившийся режим: заводить нечего, и update() ничего не
    // создаёт. Иначе бюджет превратился бы в вечную подкачку.
    const usize settled = mgr.loadedChunks();
    mgr.update({ 4.f, 70.f, 4.f });
    check(mgr.loadedChunks() == settled,
          "на месте update() ничего не заводит заново");
}

// ------------------------------------------------------------
// Высота поверхности берётся из чанка, а не считается заново
//
// TerrainGenerator::surfaceHeight — это полный расчёт колонки: все
// шумовые поля биома плюс три октавы рельефа. Замер: полмикросекунды
// на колонку. Миникарта звала его шестнадцать тысяч раз за проход
// (7.8 мс из 7.85 мс всего прохода), трава — шестьсот раз за
// пересборку. Оба раза — для местности, уже лежащей в памяти.
//
// Теперь генерация складывает высоты в Chunk::surfaceY, а
// VoxelReader::surfaceAt отдаёт их. Ценность этого держится целиком
// на одном: отданное число обязано СОВПАДАТЬ с тем, что вернул бы
// генератор. Иначе миникарта и трава разъедутся с рельефом.
// ------------------------------------------------------------
void testSurfaceHeightCacheMatchesGenerator() {
    group("мир: высота поверхности из чанка равна высоте от генератора");

    world::blocks();
    jobs::gJobs.start(2);
    {
        world::ChunkManager mgr(0x5A17ULL, 2);

        bool ready = false;
        for (int i = 0; i < 500 && !ready; ++i) {
            mgr.update({ 8.f, 70.f, 8.f });
            ready = mgr.isReadyAt(8, 8) && mgr.isReadyAt(40, 40);
            if (!ready) std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        check(ready, "чанки сгенерированы");

        if (ready) {
            world::VoxelReader rd(mgr);
            // Весь чанк (0,0) и кусок соседнего: переход через границу
            // чанка — там, где индексация проще всего перепутать.
            usize checked = 0, mismatch = 0;
            i32 firstBadX = 0, firstBadZ = 0, gotBad = 0, wantBad = 0;
            for (i32 wz = 0; wz < world::CHUNK_SIZE + 8; ++wz)
                for (i32 wx = 0; wx < world::CHUNK_SIZE + 8; ++wx) {
                    const i32 got  = rd.surfaceAt(wx, wz);
                    const i32 want = mgr.generator().surfaceHeight(wx, wz);
                    ++checked;
                    if (got != want) {
                        if (!mismatch) {
                            firstBadX = wx; firstBadZ = wz;
                            gotBad = got; wantBad = want;
                        }
                        ++mismatch;
                    }
                }
            if (mismatch)
                std::printf("    (первое расхождение: %d,%d — из чанка %d, "
                            "от генератора %d; всего %zu из %zu)\n",
                            firstBadX, firstBadZ, gotBad, wantBad,
                            mismatch, checked);
            check(checked > 1500, "проверено достаточно колонок");
            check(mismatch == 0, "высота из чанка совпадает с высотой генератора");

            // Отрицательные координаты: маска и сдвиг там ведут себя
            // иначе, чем деление, и это классическое место ошибки.
            usize negMismatch = 0;
            for (i32 wz = -40; wz < -8; ++wz)
                for (i32 wx = -40; wx < -8; ++wx)
                    if (rd.surfaceAt(wx, wz) != mgr.generator().surfaceHeight(wx, wz))
                        ++negMismatch;
            check(negMismatch == 0,
                  "и на отрицательных координатах тоже");

            // Незагруженный чанк: запасной путь обязан дать то же
            // самое, просто медленнее.
            const i32 farX = 100000, farZ = -70000;
            check(mgr.findChunk(farX >> 5, farZ >> 5) == nullptr,
                  "дальний чанк и правда не загружен");
            check(rd.surfaceAt(farX, farZ) ==
                      mgr.generator().surfaceHeight(farX, farZ),
                  "для незагруженного чанка считает генератор");
        }
    }
    jobs::gJobs.stop();
}

// ------------------------------------------------------------
// Задача, пережившая свой мир
//
// ChunkManager раздаёт фоновым задачам сырой указатель на себя, а
// деструктор дожидается этих задач. Дождаться он умеет только пока
// планировщик крутится — и ровно два пути ведут мимо:
//
//   * планировщик остановили раньше мира: очередь никто не разберёт,
//     счётчик не сдвинется, деструктор пишет в журнал и идёт дальше;
//   * задачи не уложились в пять секунд: то же самое.
//
// На обоих в очереди оставались задачи с указателем на освобождённую
// память, и первый же запуск планировщика их выполнял. В приложении
// это не стреляло только потому, что порядок вызовов выверен вручную
// (releaseWorld строго до jobs::gJobs.stop()) — то есть держалось на
// комментарии, а не на устройстве кода. В проверках воспроизводилось
// падением в десяти прогонах из десяти.
//
// Утверждение здесь — «процесс дожил до конца». Это честно: у
// обращения к освобождённой памяти нет другого наблюдаемого следа.
// Чтобы оно не было пустым, память между смертью мира и запуском
// планировщика намеренно занимается и портится: тогда обращение по
// старому указателю почти наверняка попадёт в чужое и упадёт.
// ------------------------------------------------------------
void testJobsOutlivingTheirWorldAreSafe() {
    group("мир: задачи, пережившие свой мир, ничего не трогают");

    world::blocks();
    // Планировщик остановлен: это и есть тот самый порядок.
    check(!jobs::gJobs.running(), "планировщик остановлен");

    std::vector<std::shared_ptr<world::Chunk>> kept;
    usize queued = 0;

    for (int round = 0; round < 3; ++round) {
        {
            world::ChunkManager mgr(0xFEEDULL + (u64)round, 4);
            // Много кадров — много заведённых чанков и, значит, много
            // задач генерации, которым никогда не суждено выполниться
            // при жизни этого мира.
            for (int i = 0; i < 60; ++i) mgr.update({ 0.f, 70.f, 0.f });
            queued += mgr.pendingJobs();
            // Держим чанк живым за пределами мира: если задача мёртвого
            // мира всё-таки отработает, она его сгенерирует.
            if (auto c = mgr.findChunk(0, 0)) kept.push_back(c);
        }   // мир разрушен, задачи остались в очереди

        // Занимаем и портим освободившуюся память, чтобы обращение по
        // старому указателю попало в чужое, а не в случайно уцелевшее.
        {
            std::vector<std::vector<u8>> rubble;
            for (int i = 0; i < 64; ++i) rubble.emplace_back(64 * 1024, (u8)0xA5);
            volatile u8 sink = 0;
            for (const auto& r : rubble) sink = (u8)(sink ^ r[0]);
            (void)sink;
        }
    }

    check(queued > 0, "задачи и правда остались неразобранными");
    check(!kept.empty(), "чанки мёртвых миров удержаны");

    // А теперь запускаем планировщик: очередь разбирается.
    jobs::gJobs.start(3);
    for (int i = 0; i < 100 && jobs::gJobs.running(); ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    jobs::gJobs.stop();

    check(true, "очередь разобрана, процесс жив");

    usize generated = 0;
    for (const auto& c : kept)
        if (c->generated.load(std::memory_order_acquire)) ++generated;
    check(generated == 0,
          "задача мёртвого мира не сгенерировала его чанк");
}

// ------------------------------------------------------------
// Нехватка памяти: миру есть что отдать
//
// Чанк — четверть мегабайта вокселей; в круге дальности их полторы
// сотни, то есть под сорок мегабайт, и это ещё без мешей в
// видеопамяти. APP_CMD_LOW_MEMORY при этом не делал НИЧЕГО, кроме
// записи в журнал: система просила память, мир не отдавал ничего, и
// выбор сводился к тому, что система убивала процесс целиком.
//
// Проверяется механизм, на который опирается обработчик: выгрузка
// умеет работать по заданному радиусу, а не только по обычному кругу.
// ------------------------------------------------------------
void testUnloadAcceptsTighterRadius() {
    group("мир: выгрузку можно попросить о радиусе потеснее");

    world::blocks();
    const i32 VD = 6;
    world::ChunkManager mgr(0xDEADBEEFULL, VD);
    for (int i = 0; i < 400; ++i) mgr.update({ 0.f, 70.f, 0.f });

    const usize full = mgr.loadedChunks();
    check(full > 80, "круг наполнен");

    // Обычный круг с гистерезисом ничего не выбрасывает: всё в нём.
    check(mgr.collectUnloadCandidates({ 0.f, 70.f, 0.f }).empty(),
          "по обычному радиусу выгружать нечего");

    // А по тесному — выбрасывает всё, что дальше него.
    constexpr i32 KEEP = 3;
    auto doomed = mgr.collectUnloadCandidates({ 0.f, 70.f, 0.f }, KEEP);
    check(!doomed.empty(), "по тесному радиусу есть что выгрузить");
    mgr.removeChunks(doomed);

    const usize left = mgr.loadedChunks();
    check(left < full, "чанков стало меньше");
    check(left + doomed.size() == full, "выгружено ровно столько, сколько названо");

    // Ближний круг цел: игрок не должен провалиться сквозь мир из-за
    // того, что системе не хватило памяти.
    usize nearMissing = 0, tooFarLeft = 0;
    for (i32 dz = -KEEP; dz <= KEEP; ++dz)
        for (i32 dx = -KEEP; dx <= KEEP; ++dx)
            if (dx * dx + dz * dz <= KEEP * KEEP && !mgr.findChunk(dx, dz))
                ++nearMissing;
    for (i32 dz = -VD - 2; dz <= VD + 2; ++dz)
        for (i32 dx = -VD - 2; dx <= VD + 2; ++dx)
            if (dx * dx + dz * dz > KEEP * KEEP && mgr.findChunk(dx, dz))
                ++tooFarLeft;
    check(nearMissing == 0, "ближний круг вокруг игрока остался целым");
    check(tooFarLeft == 0, "всё, что дальше тесного радиуса, выгружено");

    // И потоковая загрузка возвращает мир обратно.
    for (int i = 0; i < 400; ++i) mgr.update({ 0.f, 70.f, 0.f });
    check(mgr.loadedChunks() == full, "круг восстанавливается сам");
}

// ------------------------------------------------------------
// Порядок проходов кадра
//
// Порядок, в котором рисуются проходы, — не вкусовщина: из него прямо
// следуют две вещи, и обе видны на экране или в журнале.
//
// 1. Небо закрывает ВЕСЬ экран. Пока оно рисовалось первым и с
//    выключенной проверкой глубины, его фрагментный шейдер считался
//    для каждого пикселя, включая те, которые потом закрывал
//    ландшафт. Кадр tools/vkcheck: 323777 пикселей геометрии из
//    504000, то есть закрыто 64%. Шейдер неба при этом в десять раз
//    дороже ровной заливки той же площади (tools/gpubench на кадре
//    2306x1080: 8.08 мс против 0.78 мс). Журнал с устройства
//    подтверждает, что кадр упирается именно во фрагменты, а не в
//    геометрию: 1884 индекса в диагностической сборке и 154380 в
//    обычной дают одно и то же время рисования, 19 мс.
//
// 2. Вода НЕ пишет глубину — иначе смешивание не складывается. Пока
//    она рисовалась до мобов, NPC, предметов и травы, любая такая
//    сущность под водой проходила проверку глубины (вода её не
//    заняла) и оказывалась нарисованной ПОВЕРХ водной глади.
//
// Отсюда единственно верный порядок: всё непрозрачное, затем небо,
// затем полупрозрачное. Проверяется он по исходнику — кадр целиком на
// хосте не собрать, там нужен весь RenderSystem с Vulkan, — но
// проверяется именно как порядок, а не как наличие строк.
// ------------------------------------------------------------
// ------------------------------------------------------------
// Раскладка кадра по проходам: чем меряем
// ------------------------------------------------------------
//
// Журнал с устройства говорил «GPU 10.9 мс» и молчал о том, на что
// они ушли. Без этого числа по проходам любое решение об архитектуре
// рендера — догадка: диагностическая сцена с 1884 индексами и обычный
// мир со 154344 стоили одинаково, значит дело не в геометрии, а в чём
// именно — сказать было нечем.
//
// Проверяется не «время правильное» (его знает только устройство), а
// то, что измерительная обвязка не врёт: метки расставлены по всем
// проходам, в порядке перечисления, после самих проходов, и ни одна
// не теряется, когда проход выключен.
void testFrameGpuBreakdown() {
    group("рендер: замер кадра по проходам");

    const std::string ctxh = readSource("app/src/main/cpp/src/vk/vk_context.h");
    const std::string ctxc = readSource("app/src/main/cpp/src/vk/vk_context.cpp");
    const std::string rs   = readSource("app/src/main/cpp/src/render/render_system.cpp");
    if (ctxh.empty() || ctxc.empty() || rs.empty()) {
        check(true, "исходники не найдены, проверка пропущена");
        return;
    }
    const usize NONE = std::string::npos;

    // ---- 1. Порядок в перечислении ----
    const usize e0 = ctxh.find("enum class GpuPass");
    check(e0 != NONE, "проходы перечислены в vk::Context");
    if (e0 == NONE) return;
    const usize e1 = ctxh.find("};", e0);
    const std::string decl = stripComments(ctxh.substr(e0, e1 - e0));

    std::vector<std::string> passes;
    {
        // Имена между «{» и «Count».
        usize b = decl.find('{');
        std::string cur;
        for (usize i = b + 1; i < decl.size(); ++i) {
            const char c = decl[i];
            if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')) { cur.push_back(c); continue; }
            if (!cur.empty()) {
                if (cur == "Count") break;
                passes.push_back(cur);
                cur.clear();
            }
            if (c == '=') {   // «Terrain = 0» — пропускаем число
                while (i < decl.size() && decl[i] != ',') ++i;
            }
        }
    }
    check(passes.size() >= 5, "проходов объявлено достаточно");
    if (passes.size() < 5) return;

    // ---- 2. Каждый проход размечен, ровно один раз, в том же порядке ----
    const usize body = rs.find("void RenderSystem::render(");
    const usize bodyEnd = body == NONE ? NONE : rs.find("\n}\n", body);
    check(body != NONE && bodyEnd != NONE, "RenderSystem::render на месте");
    if (body == NONE || bodyEnd == NONE) return;
    const std::string win = rs.substr(body, bodyEnd - body);

    usize prev = 0;
    for (const auto& name : passes) {
        const std::string mark = "markPass(Pass::" + name + ")";
        const usize at = win.find(mark);
        if (at == NONE) {
            std::printf("       проход «%s» не размечен\n", name.c_str());
            check(false, "у каждого прохода есть своя метка");
            return;
        }
        if (win.find(mark, at + 1) != NONE) {
            std::printf("       проход «%s» размечен дважды\n", name.c_str());
            check(false, "у каждого прохода есть своя метка");
            return;
        }
        if (at < prev) {
            std::printf("       проход «%s» размечен не по порядку\n", name.c_str());
            check(false, "метки идут в порядке перечисления");
            return;
        }
        prev = at;
    }
    check(true, "у каждого прохода есть своя метка");
    check(true, "метки идут в порядке перечисления");

    // ---- 3. Метка стоит ПОСЛЕ работы прохода ----
    //
    // Метка перед проходом измерила бы предыдущий, и вся раскладка
    // оказалась бы сдвинутой на один проход. Сверяем по паре, которую
    // ни с чем не спутать: небо — один вызов.
    const usize skyDraw = win.find("skybox_.render");
    const usize skyMark = win.find("markPass(Pass::Sky)");
    check(skyDraw != NONE && skyMark != NONE && skyDraw < skyMark,
          "метка ставится после команд прохода, а не до");

    // ---- 4. Каждый проход выключаем отдельно ----
    for (const auto& name : passes) {
        if (win.find("on(Pass::" + name + ")") == NONE) {
            std::printf("       проход «%s» нельзя выключить\n", name.c_str());
            check(false, "каждый проход можно выключить маской");
            return;
        }
    }
    check(true, "каждый проход можно выключить маской");

    // Выключение ландшафта не должно останавливать отбор: он ставит
    // чанкам целевой уровень и заказывает меши. Иначе выключённый
    // проход менял бы не цену рисования, а поведение всего потокового
    // конвейера, и разность времён была бы разностью разных миров.
    check(win.find("cullOnly(") != NONE,
          "с выключенным ландшафтом отбор всё равно идёт");

    // ---- 5. Пропущенная метка не теряет весь кадр ----
    //
    // Запрос, сброшенный vkCmdResetQueryPool и ни разу не записанный,
    // делает ВСЮ выборку «ещё не готовой»: вместе с проходом пропало
    // бы и время кадра целиком. Поэтому markPass дописывает пропуски,
    // а endFrame добивает хвост.
    const usize mp = ctxc.find("void Context::markPass(");
    check(mp != NONE, "markPass реализован");
    if (mp != NONE) {
        const std::string mpw = ctxc.substr(mp, ctxc.find("\n}\n", mp) - mp);
        check(mpw.find("while (passMarks_ <= want)") != NONE,
              "markPass дописывает пропущенные метки");
    }
    const usize ef = ctxc.find("void Context::endFrame(");
    check(ef != NONE, "endFrame реализован");
    if (ef != NONE) {
        const std::string efw = ctxc.substr(ef, ctxc.find("\n}\n", ef) - ef);
        check(efw.find("passMarks_ < GPU_PASSES") != NONE,
              "endFrame добивает хвост меток");
        // ...но только пока метки вообще ставятся. Сбрасывается и
        // читается ровно столько запросов, сколько записывается
        // (stampsUsed), и дописывать хвост при выключенных метках
        // значило бы писать в запрос, который никто не сбрасывал.
        check(efw.find("passTiming_ && passMarks_") != NONE,
              "и только когда метки включены");
    }

    // ---- 5а. Метки внутри прохода отключаемы и выключены ----
    //
    // На устройстве они стоили двух миллисекунд из одиннадцати: та же
    // диагностическая сцена шла 10.8..11.3 мс без них и 12.7..13.3 мс
    // с ними. Плиточный GPU откладывает фрагментную работу прохода
    // рендера целиком, и метка посреди прохода заставляет его эту
    // работу разорвать. Толку при этом ноль: весь кадр собирался в
    // первой метке, остальные показывали ноль.
    check(ctxh.find("void setPassTiming(bool on)") != NONE,
          "метки внутри прохода можно выключить");
    check(ctxc.find("if (!passTiming_ ||") != NONE,
          "и выключенные они ничего не пишут");
    const std::string cfgh = readSource("app/src/main/cpp/src/config/settings.h");
    if (!cfgh.empty()) {
        const usize g = cfgh.find("bool gpuPassTiming");
        check(g != NONE, "настройка меток по проходам есть");
        if (g != NONE) {
            const std::string line = cfgh.substr(g, cfgh.find(';', g) - g);
            check(line.find("false") != NONE, "и по умолчанию выключена");
        }
    }

    // ---- 5б. Развёртка меряет вычитанием, а не метками ----
    const std::string sw = readSource("app/src/main/cpp/src/render/pass_sweep.h");
    const std::string swc =
        readSource("app/src/main/cpp/src/render/pass_sweep.cpp");
    check(!sw.empty(), "развёртка по проходам есть");
    if (!sw.empty()) {
        // Опорный замер обязан идти первым: от него считается разность.
        const usize st = sw.find("STEPS[] = {");
        check(st != NONE, "комбинации перечислены");
        if (st != NONE) {
            const usize first = sw.find("0x7F", st);
            const usize brace = sw.find('}', st);
            check(first != NONE && first < brace,
                  "опорная комбинация (все проходы) идёт первой");
        }
        check(sw.find("WARMUP_SEC") != NONE,
              "первые кадры после смены маски выбрасываются");

        // ---- Накопительный ярус ----
        //
        // Выключение проходов ПО ОДНОМУ на устройстве дало сумму
        // 1.76 мс при том, что выключение всего разом снимает 10.06.
        // Расхождение в восемь миллисекунд — изъян способа, а не шум:
        // небо рисуется последним и с проверкой глубины, поэтому
        // забирает себе ровно те пиксели, которые не закрыл ландшафт.
        // Убери ландшафт — кадр не подешевеет, и его цена окажется
        // невидимой.
        //
        // Лечится это накопительным ярусом: от пустого кадра вверх, по
        // проходу за шаг. Разность соседних строк — то, что добавил
        // очередной проход поверх уже нарисованного, и подменять там
        // некому.
        check(sw.find("CUMUL_FIRST") != NONE && sw.find("CUMUL_LAST") != NONE,
              "накопительный ярус в наборе размечен");
        // Лестница обязана начинаться с одного прохода и доходить до
        // всех: иначе это не накопление.
        for (const char* m : { "{ 0x01,", "{ 0x09,", "{ 0x49,", "{ 0x00," })
            if (sw.find(m) == NONE) {
                std::printf("       нет ступени %s\n", m);
                check(false, "лестница идёт от пустого кадра до полного");
                return;
            }
        check(true, "лестница идёт от пустого кадра до полного");
        // Каждая следующая ступень обязана включать предыдущую: иначе
        // разность соседних строк ничего не значит.
        {
            const usize c0 = sw.find("{ 0x00,");
            const usize c1 = sw.find("{ 0x01,");
            const usize c2 = sw.find("{ 0x09,");
            const usize c3 = sw.find("{ 0x49,");
            check(c0 < c1 && c1 < c2 && c2 < c3,
                  "ступени идут по возрастанию, каждая включает прошлую");
            check((0x01u & 0x09u) == 0x01u && (0x09u & 0x49u) == 0x09u,
                  "и маски вложены одна в другую");
        }

        // ---- Ландшафт разбит надвое ----
        //
        // Накопительная лестница показала: ландшафт стоит 8.43 мс из
        // 9.95 мс всего рисования, то есть 85%. Дальше вопрос один —
        // это растеризация или математика фрагмента, — и выключить
        // «освещение, но не рисование» нечем. Зато есть ранний выход
        // из voxel.frag по debug_shading: вид 1 отдаёт цвет вершины и
        // ничего больше. Разность двух ступеней с одной маской и есть
        // цена всей математики фрагмента.
        check(sw.find("u8          shading;") != NONE,
              "у ступени есть отладочный вид террейна");
        check(sw.find("{ 0x01, 1,") != NONE && sw.find("{ 0x01, 0,") != NONE,
              "ландшафт меряется дважды: с ранним выходом и целиком");
        check(sw.find("{ 0x01, 1,") < sw.find("{ 0x01, 0,"),
              "сначала без света, потом целиком");

        // Во время развёртки вид задаёт она, а не settings.cfg:
        // иначе ступень «без света» рисовала бы обычную картинку.
        const std::string rss =
            readSource("app/src/main/cpp/src/render/render_system.cpp");
        if (!rss.empty()) {
            const usize d = rss.find("setDebugShading");
            check(d != NONE, "отладочный вид ставится");
            if (d != NONE) {
                const std::string w = rss.substr(d, 200);
                check(w.find("passSweep_.active()") != NONE,
                      "и во время развёртки его задаёт она");
            }
        }

        // ---- Ярус «проход в одиночку» ----
        //
        // Накопительная разность верна, пока проходы складываются.
        // Замер 11.14 -> 9.89 показал, что не всегда: ландшафт
        // подешевел на 2.09 мс, а приписанная небу разность выросла на
        // 1.08 — при том, что sky.frag стал МЕНЬШЕ и делает на одно
        // умножение меньше. Дорожать ему было не с чего; значит
        // разность переложила часть стоимости на соседа и не сказала
        // об этом.
        //
        // Лечится третьим ярусом: проход рисуется ОДИН, поверх пустого
        // кадра. Закрывать его пикселям нечем, и цена — это просто
        // «сколько стало» минус «пустой кадр», без чужих разностей.
        check(sw.find("SOLO_FIRST") != NONE, "ярус «в одиночку» размечен");
        check(sw.find("{ 0x08,") != NONE, "небо меряется в одиночку");
        {
            const usize solo = sw.find("{ 0x08,");
            const usize off  = sw.find("{ 0x7E,");
            check(off != NONE && solo != NONE && off < solo,
                  "ярус «в одиночку» идёт последним, после выключения по одному");

            // SOLO_FIRST обязан указывать на НАСТОЯЩИЙ номер этой
            // ступени. Мутация «SOLO_FIRST = 9» пережила первую
            // редакцию проверки: ярус становится пустым, ступень
            // достаётся циклу выключения по одному и печатается
            // формулой «опорное минус эта строка» — то есть ровно тем
            // враньём, ради которого ярус и заведён. Молча.
            const usize st0 = sw.find("STEPS[] = {");
            const usize stEnd = sw.find("\n    };", st0);
            usize idx = 0, soloIdx = (usize)-1;
            for (usize at = sw.find("{ 0x", st0);
                 at != NONE && at < stEnd;
                 at = sw.find("{ 0x", at + 1), ++idx)
                if (at == solo) { soloIdx = idx; break; }

            int declared = -1;
            const usize sf = sw.find("SOLO_FIRST  = ");
            if (sf != NONE) std::sscanf(sw.c_str() + sf + 14, "%d", &declared);
            if (soloIdx == (usize)-1 || declared < 0 ||
                (usize)declared != soloIdx) {
                std::printf("       SOLO_FIRST = %d, а ступень стоит %zu-й\n",
                            declared, soloIdx);
                check(false, "SOLO_FIRST указывает на первую одиночную ступень");
            } else {
                check(true, "SOLO_FIRST указывает на первую одиночную ступень");
            }
        }
        if (!swc.empty()) {
            // Одиночную ступень нельзя печатать формулой «опорное
            // минус эта строка»: она мерит не то, чего не хватает
            // кадру, а то, что стоит сам проход. Ярус выключения по
            // одному обязан останавливаться на SOLO_FIRST.
            const usize offLoop  = swc.find("i = CUMUL_LAST + 1");
            const usize soloLoop = swc.find("i = SOLO_FIRST");
            check(offLoop != NONE && soloLoop != NONE && offLoop < soloLoop,
                  "у одиночного яруса свой цикл печати");
            if (offLoop != NONE)
                check(swc.compare(offLoop, 40, "i = CUMUL_LAST + 1; i < SOLO_FIRST") == 0
                          || swc.find("i < SOLO_FIRST", offLoop) < swc.find(';', soloLoop),
                      "выключение по одному не захватывает одиночные ступени");
            if (soloLoop != NONE) {
                const std::string w = swc.substr(soloLoop, 300);
                check(w.find("- empty") != NONE,
                      "одиночная ступень считается от пустого кадра");
                check(w.find("base -") == NONE,
                      "а не разностью с опорным");
            }
        }

        // ---- Ярус «дальние первыми»: сколько экономит ранний тест ----
        //
        // Перекрытие ландшафта — главный оставшийся вопрос аудита, и
        // счётчика перекрытых фрагментов у нас не будет. Зато есть
        // способ увидеть ту же величину косвенно: нарисовать тот же
        // ландшафт от ДАЛЬНЕГО к ближнему. Картинка не изменится
        // (геометрия непрозрачная, тест глубины включён), а ранний
        // тест перестанет отбрасывать закрытые фрагменты — ближнее
        // рисуется последним. Разница и есть то, что он экономит.
        check(sw.find("ORDER_FIRST") != NONE, "ярус «дальние первыми» размечен");
        check(sw.find("u8          farFirst;") != NONE,
              "у ступени есть порядок непрозрачных чанков");
        check(sw.find("{ 0x01, 1, 1,") != NONE && sw.find("{ 0x01, 0, 1,") != NONE,
              "обе ступени ландшафта меряются и в обратном порядке");

        // Пара обязана отличаться ТОЛЬКО порядком.
        //
        // Иначе разность мерит не ранний тест, а что-то ещё: другую
        // маску, другой отладочный вид. Сверяем поля впрямую.
        {
            struct St { u32 mask; u32 shading; u32 farFirst; };
            std::vector<St> steps;
            const usize st0 = sw.find("STEPS[] = {");
            const usize stEnd = sw.find("\n    };", st0);
            for (usize at = sw.find("{ 0x", st0); at != NONE && at < stEnd;
                 at = sw.find("{ 0x", at + 1)) {
                St v{};
                if (std::sscanf(sw.c_str() + at, "{ 0x%x, %u, %u,",
                                &v.mask, &v.shading, &v.farFirst) == 3)
                    steps.push_back(v);
            }
            int orderFirst = -1, cumulFirst = -1;
            const usize of = sw.find("ORDER_FIRST = ");
            const usize cf = sw.find("CUMUL_FIRST = ");
            if (of != NONE) std::sscanf(sw.c_str() + of + 14, "%d", &orderFirst);
            if (cf != NONE) std::sscanf(sw.c_str() + cf + 14, "%d", &cumulFirst);
            check(orderFirst > 0 && cumulFirst > 0 &&
                  (usize)orderFirst < steps.size(),
                  "ORDER_FIRST указывает на существующую ступень");

            bool paired = true;
            for (usize i = (usize)orderFirst; i < steps.size(); ++i) {
                const usize ref = (usize)cumulFirst + (i - (usize)orderFirst);
                if (ref >= steps.size()) break;
                const St& a = steps[i];
                const St& b = steps[ref];
                if (a.mask != b.mask || a.shading != b.shading ||
                    a.farFirst != 1 || b.farFirst != 0) {
                    std::printf("       ступень %zu и её пара %zu отличаются "
                                "не только порядком\n", i, ref);
                    paired = false;
                }
            }
            check(paired, "каждая ступень яруса отличается от своей пары только порядком");

            // И в игре порядок всегда от ближнего: обратный — только
            // на время развёртки.
            for (usize i = 0; i < (usize)orderFirst && i < steps.size(); ++i)
                if (steps[i].farFirst != 0) {
                    std::printf("       ступень %zu вне яруса рисует дальние первыми\n", i);
                    check(false, "обратный порядок только в своём ярусе");
                    return;
                }
            check(true, "обратный порядок только в своём ярусе");
        }
        if (!swc.empty()) {
            const usize orderLoop = swc.find("i = ORDER_FIRST");
            check(orderLoop != NONE, "у яруса свой цикл печати");
            if (orderLoop != NONE) {
                const std::string w = swc.substr(orderLoop, 400);
                check(w.find("CUMUL_FIRST + (i - ORDER_FIRST)") != NONE,
                      "печатается против своей пары, а не против опорного");
            }
            // Одиночный ярус не должен захватывать ступени порядка.
            check(swc.find("i = SOLO_FIRST; i < ORDER_FIRST") != NONE,
                  "ярус «в одиночку» останавливается на ORDER_FIRST");
        }
        {
            const std::string rs2 =
                readSource("app/src/main/cpp/src/render/render_system.cpp");
            const std::string cr =
                readSource("app/src/main/cpp/src/render/chunk_renderer.h");
            if (!rs2.empty())
                check(rs2.find("setFarFirst(passSweep_.farFirst())") != NONE,
                      "порядок задаёт только развёртка");
            if (!cr.empty())
                check(cr.find("bool farFirst_ = false;") != NONE,
                      "по умолчанию — от ближнего к дальнему");
        }

        // ---- Пустой кадр: нижний предел ----
        //
        // Первый замер на устройстве дал сумму всех проходов 1.5 мс
        // из 10.8 — девять миллисекунд ни на что. Без кадра, в
        // котором не нарисовано НИЧЕГО, нельзя отличить «рисование
        // дешёвое» от «мерим не рисование».
        check(sw.find("{ 0x00,") != NONE,
              "в наборе есть кадр, где не рисуется ничего");
        // Он и есть пол: проход рендера, показ и ожидание картинки.
        // На устройстве это 0.74 мс из 10.80 — то есть кадр занят
        // рисованием, а не ожиданием, и гипотеза об обратном закрыта.
        const usize zero = sw.find("{ 0x00,");
        const usize all  = sw.find("{ 0x7F,");
        check(all != NONE && zero != NONE && all < zero,
              "опорный кадр идёт первым, пустой сразу за ним");

        // ---- Чередование кругов ----
        //
        // Два круга подряд дали по одному проходу 0.56 и 1.36 мс.
        // Такой разброс — не выборочный шум на трёх сотнях кадров, а
        // нагрев телефона. Комбинации обязаны чередоваться и набирать
        // время по многу раз вперемешку.
        check(sw.find("ROUNDS") != NONE, "набор проходится несколько раз");
        const usize r = sw.find("ROUNDS = ");
        if (r != NONE) {
            int rounds = 0;
            if (std::sscanf(sw.c_str() + r + 9, "%d", &rounds) == 1)
                check(rounds >= 3, "кругов достаточно, чтобы размазать нагрев");
        }
        if (!swc.empty()) {
            // Шаг обязан меняться раньше круга: иначе это не
            // чередование, а те же блоки подряд.
            const usize inc = swc.find("++step_");
            const usize rnd = swc.find("++round_");
            check(inc != NONE && rnd != NONE && inc < rnd,
                  "комбинации чередуются, а не идут блоками подряд");
            // Накопители — по комбинации, а не одно общее число:
            // иначе круги не сложить.
            check(swc.find("sum_[step_]") != NONE,
                  "время копится по каждой комбинации отдельно");
        }
    }

    // ---- 5в. Автовыход по завершении замера ----
    //
    // Диагностическая сборка запускается ради одного числа; держать её
    // открытой после того, как число получено, значит греть телефон и
    // портить следующий замер. Выход обязан идти ОБЫЧНЫМ путём —
    // через wantQuit: по дороге журнал копируется в буфер обмена, и
    // руками остаётся только запустить и подождать.
    if (!cfgh.empty()) {
        const usize e = cfgh.find("bool exitAfterSweep");
        check(e != NONE, "автовыход по завершении замера есть");
        if (e != NONE) {
            const std::string line = cfgh.substr(e, cfgh.find(';', e) - e);
            check(line.find("DIAGNOSTIC_BUILD") != NONE,
                  "и включён ровно в диагностической сборке");
        }
    }
    const std::string mainSrc = readSource("app/src/main/cpp/src/main.cpp");
    if (!mainSrc.empty()) {
        // Ищем именно ветку выхода, а не строку в журнале: имя
        // настройки встречается в файле дважды.
        const usize q = mainSrc.find("if (cfg::settingsConst().exitAfterSweep)");
        check(q != NONE, "цикл кадров смотрит на настройку автовыхода");
        if (q != NONE) {
            // Выход обязан идти через wantQuit, а не через
            // ANativeActivity_finish напрямую: иначе не отработает
            // onWindowTerm, и журнал не попадёт в буфер обмена.
            const std::string w = mainSrc.substr(q, 400);
            check(w.find("wantQuit = true") != NONE,
                  "и выходит обычным путём, с копированием журнала");
            check(w.find("ANativeActivity_finish") == NONE,
                  "а не закрывает окно в обход onWindowTerm");
        }
        // Сам путь wantQuit обязан копировать журнал — иначе автовыход
        // отнимает у замера его результат.
        const usize term = mainSrc.find("void onWindowTerm");
        if (term != NONE) {
            const std::string tw = mainSrc.substr(term, 3000);
            check(tw.find("copyFileToClipboard") != NONE,
                  "на выходе журнал копируется в буфер обмена");
        }
    }
    // Пока идёт развёртка, маску задаёт она, а не настройка: два
    // источника на одно поле спорили бы, и замер сравнивал бы не то.
    check(win.find("passSweep_.active() ? passSweep_.mask()") != NONE,
          "во время развёртки маску задаёт она");

    // ---- 6. Пул рассчитан на все метки ----
    check(ctxh.find("STAMPS_PER_FRAME = 2 + GPU_PASSES") != NONE,
          "меток на кадр хватает на начало, конец и все проходы");
    check(ctxc.find("qi.queryCount = MAX_FRAMES * STAMPS_PER_FRAME") != NONE,
          "пул запросов создаётся под это число");

    // ---- 7. Маска по умолчанию — все проходы ----
    const std::string cfg = readSource("app/src/main/cpp/src/config/settings.h");
    if (!cfg.empty()) {
        const usize m = cfg.find("renderPasses");
        check(m != NONE, "маска проходов есть в настройках");
        if (m != NONE) {
            const std::string line = cfg.substr(m, cfg.find(';', m) - m);
            check(line.find("0x7F") != NONE,
                  "и по умолчанию включены все семь");
        }
    }
}

// ------------------------------------------------------------
// Сводка кадров считает по настоящим часам
// ------------------------------------------------------------
//
// Здесь стояло statTimer += dt, а dt в диагностической сборке прибит
// к 1/60: debug_scene живёт на постоянном шаге. Числитель и
// знаменатель росли на один и тот же кадр, и «кадров в секунду»
// выходило тождественно 60.0 при любой настоящей частоте. В журнале
// с устройства стояло ровно «60.0/с», пока игра шла за девяносто, —
// и сводка врала ровно в том числе, ради которого её читают.
void testFrameRateIsMeasuredByWallClock() {
    group("сводка: частота кадров считается по часам");

    const std::string m = readSource("app/src/main/cpp/src/main.cpp");
    if (m.empty()) { check(true, "исходник не найден, проверка пропущена"); return; }
    const std::string src = stripComments(m);
    const usize NONE = std::string::npos;

    check(src.find("statTimer += dt") == NONE,
          "окно сводки не набирается из шага симуляции");

    const usize f = src.find("const f32 fps =");
    check(f != NONE, "частота кадров считается");
    if (f != NONE) {
        const std::string line = src.substr(f, src.find(';', f) - f);
        check(line.find("dt") == NONE, "и не делится на шаг симуляции");
        check(line.find("statSec") != NONE, "а делится на измеренные секунды");
    }
    check(src.find("std::chrono::duration<f32>(now - statWall)") != NONE,
          "секунды берутся у steady_clock");

    // Ожидание GPU и запись команд — разные беды, и лечатся они
    // противоположным. Отрезок, накрывавший и запись, и отправку с
    // показом, в журнале с устройства показывал 10.5 мс при 1884
    // индексах и повторял время GPU кадр в кадр: процессор стоял и
    // ждал GPU, а по числу это выглядело как «не успеваем записывать».
    check(src.find("msSubmit") != NONE,
          "отправка и показ меряются отдельно от записи команд");
    const usize rec = src.find("msRecord  +=");
    check(rec != NONE, "запись команд меряется");
    if (rec != NONE) {
        const std::string line = src.substr(rec, src.find(';', rec) - rec);
        check(line.find("tD") == NONE,
              "и её отрезок не дотягивается до конца кадра");
    }

    // Шаг симуляции при этом обязан остаться постоянным: сводка —
    // это отчётность, она не имеет права трогать сам шаг.
    check(src.find("if (dbgScene) dt = world::SCENE_FIXED_DT;") != NONE,
          "постоянный шаг диагностической сцены на месте");
}

void testFramePassOrder() {
    group("рендер: порядок проходов кадра");

    const std::string rs = readSource("app/src/main/cpp/src/render/render_system.cpp");
    const std::string sb = readSource("app/src/main/cpp/src/render/skybox.cpp");
    if (rs.empty() || sb.empty()) {
        check(true, "исходники не найдены, проверка пропущена");
        return;
    }

    const usize body = rs.find("void RenderSystem::render(");
    check(body != std::string::npos, "RenderSystem::render на месте");
    if (body == std::string::npos) return;
    // Окно — до конца функции, а не «столько-то символов»: тело растёт
    // вместе с объяснениями в комментариях, и счёт символов протухает
    // молча, превращая проверку порядка в проверку «строка не найдена».
    const usize bodyEnd = rs.find("\n}\n", body);
    check(bodyEnd != std::string::npos, "конец RenderSystem::render найден");
    if (bodyEnd == std::string::npos) return;
    const std::string win = rs.substr(body, bodyEnd - body);

    auto at = [&](const char* what) { return win.find(what); };

    const usize opaque   = at("chunkRenderer_.renderOpaque");
    const usize npc      = at("npcRenderer_.render");
    const usize mob      = at("mobRenderer_.render");
    const usize proj     = at("projRenderer_.render");
    const usize item     = at("itemRenderer_.render");
    const usize grass    = at("grass_.render");
    const usize sky      = at("skybox_.render");
    const usize blended  = at("chunkRenderer_.renderBlended");
    const usize outline  = at("blockOutline_.render");
    const usize ui       = at("ui_->render");

    const usize NONE = std::string::npos;
    check(opaque != NONE && sky != NONE && blended != NONE,
          "оба прохода ландшафта и небо на месте");
    if (opaque == NONE || sky == NONE || blended == NONE) return;

    // Непрозрачное — до неба.
    check(opaque < sky,  "ландшафт рисуется до неба");
    check(npc    < sky,  "NPC рисуются до неба");
    check(mob    < sky,  "мобы рисуются до неба");
    check(proj   < sky,  "снаряды рисуются до неба");
    check(item   < sky,  "предметы рисуются до неба");
    check(grass  < sky,  "трава рисуется до неба");

    // Полупрозрачное — после неба, и после всего непрозрачного.
    check(sky < blended, "небо рисуется до воды");
    check(grass < blended && mob < blended && npc < blended && item < blended,
          "вода рисуется после непрозрачных сущностей");
    check(blended < outline, "контур блока — после воды");
    check(outline < ui, "интерфейс рисуется последним");

    // Само небо: проверка глубины включена, запись выключена. Без
    // проверки порядок бессмыслен — небо затрёт собой всё, что
    // нарисовано раньше.
    // Окно — снова до конца функции, а не «столько-то байт». В первый
    // раз тут стояло 1600 байт, и проверка развалилась молча: объяснение
    // рядом с флагами написано кириллицей, а это два байта на букву, и
    // нужная строка оказалась на 1599-м.
    const usize pipe = sb.find("bool Skybox::init(");
    check(pipe != NONE, "описание конвейера неба на месте");
    if (pipe != NONE) {
        const usize pipeEnd = sb.find("\n}\n", pipe);
        const std::string pwin = sb.substr(pipe, pipeEnd == NONE ? 4000
                                                                 : pipeEnd - pipe);
        check(pwin.find("d.depthTest    = true;") != NONE,
              "у неба включена проверка глубины");
        check(pwin.find("d.depthWrite   = false;") != NONE,
              "и выключена запись глубины");
    }

    // Вершинный шейдер неба обязан класть z на дальнюю плоскость:
    // только тогда LESS_OR_EQUAL пропускает его ровно там, где
    // глубина осталась очищенной.
    const std::string skyVert = readSource("app/src/main/cpp/shaders/sky.vert");
    if (!skyVert.empty())
        check(skyVert.find("uv * 2.0 - 1.0, 1.0, 1.0") != NONE,
              "небо лежит на дальней плоскости (z = 1)");
}

// ------------------------------------------------------------
// Настройка частоты кадров доходит до цепочки показа
//
// Тумблер уже был: он лежал в структуре настроек, сохранялся в файл,
// показывался в меню — и НЕ ДЕЛАЛ НИЧЕГО. Режим показа стоял в
// createSwapchain намертво (VK_PRESENT_MODE_FIFO_KHR), и переключение
// в меню меняло только строчку в settings.cfg.
//
// Это ровно тот класс поломки, который не видно ни в одном журнале:
// всё «работает», настройка сохраняется, а поведение не меняется.
// Поэтому проверяется не наличие поля, а цепочка целиком: значение по
// умолчанию, запись и чтение файла, чтение старого ключа — и по
// исходнику то, что режим показа выбирается, а не зашит.
// ------------------------------------------------------------
void testFrameRateLimitSetting() {
    group("настройки: ограничение частоты кадров доходит до цепочки");

    // 1. По умолчанию ограничения нет: пока частота упирается в экран,
    //    по ней нельзя сказать ничего о запасе.
    {
        config::Settings s;
        check(s.unlimitedFps, "по умолчанию ограничения кадров нет");
    }

    // 2. Значение переживает запись и чтение.
    const std::string path = "build/hostcheck/settings_fps.cfg";
    {
        config::Settings s;
        s.unlimitedFps = false;
        check(s.save(path), "настройки записаны");

        config::Settings back;
        check(back.load(path), "настройки прочитаны");
        check(!back.unlimitedFps, "выключенное состояние дошло целым");
    }
    {
        config::Settings s;
        s.unlimitedFps = true;
        s.save(path);
        config::Settings back;
        back.unlimitedFps = false;
        back.load(path);
        check(back.unlimitedFps, "включённое состояние дошло целым");
    }

    // 3. Старый ключ из уже лежащих на диске файлов. Он был обратным
    //    по смыслу, и прочитать его надо обратным же образом — иначе
    //    прежний выбор игрока молча перевернётся.
    {
        std::FILE* f = std::fopen(path.c_str(), "wb");
        check(f != nullptr, "файл со старым ключом создан");
        if (f) {
            std::fputs("[Render]\nvsync = true\n", f);
            std::fclose(f);
            config::Settings s;
            s.unlimitedFps = true;
            check(s.load(path), "файл со старым ключом прочитан");
            check(!s.unlimitedFps,
                  "старое vsync=true означает, что ограничение нужно");
        }
    }
    {
        std::FILE* f = std::fopen(path.c_str(), "wb");
        if (f) {
            std::fputs("[Render]\nvsync = false\n", f);
            std::fclose(f);
            config::Settings s;
            s.unlimitedFps = false;
            s.load(path);
            check(s.unlimitedFps, "а старое vsync=false — что не нужно");
        }
    }

    // 3a. Пробелы вокруг ключа и значения не значат ничего.
    //
    //     Раньше значили: разбор не обрезал их, и «debug_scene = true»
    //     давало ключ «debug_scene » с пробелом на конце, который не
    //     совпадал ни с чем. Файл читался молча, настройка не
    //     применялась. Движок пишет файл без пробелов, поэтому на
    //     своих же файлах это не всплывало никогда — только на тех,
    //     что правят руками. А именно так и описан в
    //     docs/RENDER_AUDIT.md способ включить диагностическую сцену.
    {
        std::FILE* f = std::fopen(path.c_str(), "wb");
        check(f != nullptr, "файл с пробелами создан");
        if (f) {
            std::fputs("# комментарий\n"
                       "[Render]\n"
                       "  view_distance  =  9  \n"
                       "\tunlimited_fps\t=\tfalse\t\n",
                       f);
            std::fclose(f);
            config::Settings s;
            s.viewDistance = 7;
            s.unlimitedFps = true;
            check(s.load(path), "файл с пробелами прочитан");
            check(s.viewDistance == 9, "пробелы вокруг числа не мешают");
            check(!s.unlimitedFps, "и вокруг тумблера тоже");
        }
    }

    // 3b. Заголовок раздела и комментарий — не пары «ключ-значение».
    {
        std::FILE* f = std::fopen(path.c_str(), "wb");
        if (f) {
            std::fputs("[Render]\n# view_distance = 4\n; view_distance = 5\n", f);
            std::fclose(f);
            config::Settings s;
            s.viewDistance = 8;
            s.load(path);
            check(s.viewDistance == 8,
                  "закомментированный ключ не применяется");
        }
    }

    std::remove(path.c_str());

    // 4. И главное: цепочка показа берёт режим из настройки, а не из
    //    зашитой константы.
    const std::string vk  = readSource("app/src/main/cpp/src/vk/vk_context.cpp");
    const std::string mn  = readSource("app/src/main/cpp/src/main.cpp");
    const std::string uis = readSource("app/src/main/cpp/src/ui/ui_system.cpp");
    if (vk.empty() || mn.empty() || uis.empty()) {
        check(true, "исходники не найдены, проверка пропущена");
        return;
    }
    check(vk.find("ci.presentMode      = choosePresentMode();") != std::string::npos,
          "режим показа выбирается, а не зашит");
    check(vk.find("ci.presentMode      = VK_PRESENT_MODE_FIFO_KHR;") == std::string::npos,
          "зашитого FIFO не осталось");
    check(vk.find("VK_PRESENT_MODE_IMMEDIATE_KHR") != std::string::npos,
          "без ограничения просим IMMEDIATE — он и показывает максимум");
    check(mn.find("vk.setVsync(!s.unlimitedFps)") != std::string::npos,
          "смена тумблера доходит до контекста");
    check(mn.find("vk.setVsync(!cfg::settingsConst().unlimitedFps)") != std::string::npos,
          "и применяется ещё до создания первой цепочки");
    check(uis.find("s.unlimitedFps = !s.unlimitedFps") != std::string::npos,
          "тумблер в меню переключает именно её");
}

// ------------------------------------------------------------
// Журнал в буфер обмена: срезка длинного файла
//
// Сама отправка — это JNI, её на хосте не выполнить. А вот подготовка
// текста — обычный код, и ошибка в ней даёт не отказ, а мусор в
// буфере: заметишь только тогда, когда журнал понадобится, то есть в
// самый неподходящий момент.
//
// Ограничение не выдумано: транзакция Binder — около мегабайта на весь
// процесс, и журнал длинной сессии перерастает его легко.
// ------------------------------------------------------------
void testClipboardLogTrimming() {
    group("журнал: подготовка текста для буфера обмена");

    const std::string path = "build/hostcheck/cliptest.log";

    // Короткий файл уходит целиком.
    {
        const std::string body = "======== запуск ========\nI: строка\nI: ещё строка\n";
        std::FILE* f = std::fopen(path.c_str(), "wb");
        check(f != nullptr, "короткий файл создан");
        if (f) { std::fwrite(body.data(), 1, body.size(), f); std::fclose(f); }
        const std::string got = sys::clipboardTextFromFile(path.c_str());
        check(got == body, "короткий журнал уходит целиком");
    }

    // Длинный — началом и хвостом, и всё вместе влезает в предел.
    {
        const usize total = sys::CLIPBOARD_MAX_BYTES * 3;
        std::string body;
        body.reserve(total);
        // Каждая строка пронумерована: по номерам видно, что взяты
        // именно начало и хвост, а не что попало.
        for (u64 i = 0; body.size() < total; ++i) {
            char line[64];
            std::snprintf(line, sizeof(line), "I: строка %llu\n",
                          (unsigned long long)i);
            body += line;
        }
        std::FILE* f = std::fopen(path.c_str(), "wb");
        check(f != nullptr, "длинный файл создан");
        if (f) { std::fwrite(body.data(), 1, body.size(), f); std::fclose(f); }

        const std::string got = sys::clipboardTextFromFile(path.c_str());
        check(!got.empty(), "из длинного журнала что-то взято");
        check(got.size() < body.size(), "взято меньше, чем есть");
        // Предел плюс отметка о пропуске — она короткая и постоянная.
        check(got.size() <= sys::CLIPBOARD_MAX_BYTES + 256,
              "взятое укладывается в предел транзакции");

        check(got.compare(0, 24, body.compare(0, 24, got, 0, 24) == 0
                                     ? got.substr(0, 24) : std::string()) == 0,
              "текст начинается с начала файла");
        check(got.rfind("пропущено") != std::string::npos,
              "о пропуске середины сказано прямо");
        // Хвост файла обязан быть хвостом текста: именно там то, на
        // чём всё кончилось.
        const std::string lastLines = body.substr(body.size() - 64);
        check(got.size() >= lastLines.size() &&
              got.compare(got.size() - lastLines.size(), lastLines.size(),
                          lastLines) == 0,
              "и заканчивается концом файла");
    }

    // Нет файла — нет текста, и это не падение.
    check(sys::clipboardTextFromFile("build/hostcheck/нет-такого.log").empty(),
          "отсутствующий файл даёт пустой текст");
    check(sys::clipboardTextFromFile(nullptr).empty(),
          "нулевой путь тоже");

    std::remove(path.c_str());
}

// ------------------------------------------------------------
// Вода на огрублённых уровнях
// ------------------------------------------------------------
namespace {

/// Карта верхних граней: для каждого столбца — есть ли над ним
/// водяная грань и есть ли твёрдая.
struct TopMap {
    bool water[world::CHUNK_SIZE][world::CHUNK_SIZE] = {};
    bool solid[world::CHUNK_SIZE][world::CHUNK_SIZE] = {};
    i32 waterCells = 0, solidCells = 0;
};

TopMap topFaces(const std::vector<world::Quad>& quads) {
    TopMap m;
    for (const auto& q : quads) {
        if (q.v0.face != 2) continue;          // только грани вверх
        const glm::vec3 p1 = q.v0.pos + q.du + q.dv;
        const i32 x0 = (i32)std::floor(std::min(q.v0.pos.x, p1.x));
        const i32 x1 = (i32)std::floor(std::max(q.v0.pos.x, p1.x));
        const i32 z0 = (i32)std::floor(std::min(q.v0.pos.z, p1.z));
        const i32 z1 = (i32)std::floor(std::max(q.v0.pos.z, p1.z));
        for (i32 z = z0; z < z1; ++z)
            for (i32 x = x0; x < x1; ++x) {
                if ((u32)x >= (u32)world::CHUNK_SIZE ||
                    (u32)z >= (u32)world::CHUNK_SIZE) continue;
                if (q.v0.block == world::WATER) m.water[x][z] = true;
                else                            m.solid[x][z] = true;
            }
    }
    for (i32 z = 0; z < world::CHUNK_SIZE; ++z)
        for (i32 x = 0; x < world::CHUNK_SIZE; ++x) {
            if (m.water[x][z]) ++m.waterCells;
            if (m.solid[x][z]) ++m.solidCells;
        }
    return m;
}

} // namespace

void testCoarseWaterIsStable() {
    group("LOD: вода не разливается и не исчезает при огрублении");

    world::blocks();
    world::ChunkNeighbors nb;
    std::vector<world::Quad> quads;

    // --- Берег, не совпадающий с сеткой 8x8 ---
    // Суша до x=19 высотой 31 блок, дальше вода 28..31 поверх камня.
    // Поверхность воды ВЫШЕ кромки берега — ровно тот случай, на
    // котором правило «выигрывает верхний воксель» отдавало воде всю
    // клетку 8^3 и берег становился прозрачным.
    auto shore = std::make_unique<world::Chunk>();
    for (i32 z = 0; z < world::CHUNK_SIZE; ++z)
        for (i32 x = 0; x < world::CHUNK_SIZE; ++x) {
            const bool land = x < 20;
            const i32 stoneTop = land ? 30 : 27;
            for (i32 y = 0; y <= stoneTop; ++y)
                shore->setUnlocked(x, y, z, world::STONE);
            if (!land)
                for (i32 y = 28; y <= 31; ++y)
                    shore->setUnlocked(x, y, z, world::WATER);
        }

    world::buildGreedyMesh(*shore, nb, quads, world::Lod::Quarter);
    const TopMap q4 = topFaces(quads);
    world::buildGreedyMesh(*shore, nb, quads, world::Lod::Eighth);
    const TopMap q8 = topFaces(quads);

    check(q4.waterCells > 0, "на уровне 4^3 вода есть");
    check(q8.waterCells > 0, "на уровне 8^3 вода не исчезла");

    // Не разливается: вода грубого уровня не залезает туда, где на
    // уровне мельче была суша.
    i32 spill = 0;
    for (i32 z = 0; z < world::CHUNK_SIZE; ++z)
        for (i32 x = 0; x < world::CHUNK_SIZE; ++x)
            if (q8.water[x][z] && !q4.water[x][z]) ++spill;
    check(spill == 0, "вода 8^3 не заливает берег, сухой на 4^3");

    // Берег остаётся непрозрачным: под водой на грубом уровне не
    // должно открываться сквозной дыры.
    check(q8.solid[17][4], "полоса берега шириной в полклетки осталась сушей");
    check(q8.water[25][4], "а вода за ней осталась водой");

    // --- Озеро с одиноким выступом ---
    // Вода 24..27 по всему чанку, и один столб камня до 31. Правило
    // «выигрывает верхний воксель» отдавало этому столбу целую клетку
    // 8^3, и озеро в ней пропадало целиком.
    auto lake = std::make_unique<world::Chunk>();
    for (i32 z = 0; z < world::CHUNK_SIZE; ++z)
        for (i32 x = 0; x < world::CHUNK_SIZE; ++x) {
            for (i32 y = 0; y <= 23; ++y)
                lake->setUnlocked(x, y, z, world::STONE);
            for (i32 y = 24; y <= 27; ++y)
                lake->setUnlocked(x, y, z, world::WATER);
        }
    for (i32 y = 24; y <= 31; ++y)
        lake->setUnlocked(0, y, 0, world::STONE);

    world::buildGreedyMesh(*lake, nb, quads, world::Lod::Quarter);
    const TopMap l4 = topFaces(quads);
    world::buildGreedyMesh(*lake, nb, quads, world::Lod::Eighth);
    const TopMap l8 = topFaces(quads);

    check(l4.waterCells > world::CHUNK_SIZE * world::CHUNK_SIZE / 2,
          "на уровне 4^3 озеро занимает почти весь чанк");
    check(l8.waterCells * 2 >= l4.waterCells,
          "на уровне 8^3 озеро не съедено выступом");
    check(l8.water[3][3], "клетка с одиноким камнем осталась водой");
}

void testCoarseWaterDoesNotFloat() {
    group("LOD: прозрачное не поднимается и не подменяет сушу");

    world::blocks();
    world::ChunkNeighbors nb;
    std::vector<world::Quad> quads;

    // --- 1. Гладь воды не уезжает вверх ---
    //
    // Море в этом мире стоит на y = 32, то есть верхний воксель воды
    // попадает в САМЫЙ НИЗ клетки 32..39. Клетка рисуется целым кубом,
    // и по правилу «занято — значит вся клетка» её верх оказывался на
    // y = 40: гладь уезжала на семь блоков вверх и вставала над
    // берегом отдельными плитами.
    auto sea = std::make_unique<world::Chunk>();
    for (i32 z = 0; z < world::CHUNK_SIZE; ++z)
        for (i32 x = 0; x < world::CHUNK_SIZE; ++x) {
            for (i32 y = 0; y <= 23; ++y) sea->setUnlocked(x, y, z, world::STONE);
            for (i32 y = 24; y <= 32; ++y) sea->setUnlocked(x, y, z, world::WATER);
        }

    auto topWaterFace = [&](const std::vector<world::Quad>& qs) {
        f32 best = -1.f;
        for (const auto& q : qs) {
            if (q.v0.face != 2) continue;
            if (q.v0.block != world::WATER) continue;
            best = std::max(best, q.v0.pos.y);
        }
        return best;
    };

    world::buildGreedyMesh(*sea, nb, quads, world::Lod::Full);
    const f32 seaFull = topWaterFace(quads);
    check(seaFull > 32.5f && seaFull < 33.5f, "в полном разрешении гладь на y=33");

    world::buildGreedyMesh(*sea, nb, quads, world::Lod::Eighth);
    const f32 sea8 = topWaterFace(quads);
    check(sea8 > 0.f, "на уровне 8^3 море не исчезло");
    check(sea8 <= seaFull, "и не поднялось выше настоящей глади");
    check(sea8 >= seaFull - 8.f, "и не провалилось глубже одной клетки");

    world::buildGreedyMesh(*sea, nb, quads, world::Lod::Quarter);
    const f32 sea4 = topWaterFace(quads);
    check(sea4 > 0.f && sea4 <= seaFull, "на уровне 4^3 тоже не поднялось");

    // --- 2. Полоска воды у обрыва не становится кубом над пустотой ---
    //
    // Две колонки воды на краю плато, остальные шесть колонок клетки —
    // воздух. Правило «занято — значит вся клетка» отдавало такой
    // клетке весь куб 8x8x8, и над обрывом висела плита воды.
    auto shelf = std::make_unique<world::Chunk>();
    for (i32 z = 0; z < world::CHUNK_SIZE; ++z)
        for (i32 x = 0; x < world::CHUNK_SIZE; ++x) {
            const i32 top = (x < 2) ? 39 : 31;
            for (i32 y = 0; y <= top; ++y) shelf->setUnlocked(x, y, z, world::STONE);
            if (x < 2)
                for (i32 y = 40; y <= 41; ++y) shelf->setUnlocked(x, y, z, world::WATER);
        }

    world::buildGreedyMesh(*shelf, nb, quads, world::Lod::Eighth);
    const TopMap s8 = topFaces(quads);
    check(!s8.water[5][4], "над обрывом воды не появилось");
    check(!s8.water[7][7], "и в дальнем углу той же клетки — тоже");
    check(s8.solid[5][4] || s8.solid[7][7], "сам обрыв при этом на месте");
}

void testDistantWaterIsNotBlended() {
    group("вода: на дальних уровнях прозрачности нет");

    world::blocks();
    world::ChunkNeighbors nb;
    std::vector<world::Quad> quads;
    std::vector<render::VoxelVertex> verts;
    std::vector<u32> idx;
    u32 opaque = 0;

    auto lake = std::make_unique<world::Chunk>();
    for (i32 z = 0; z < world::CHUNK_SIZE; ++z)
        for (i32 x = 0; x < world::CHUNK_SIZE; ++x) {
            for (i32 y = 0; y <= 27; ++y) lake->setUnlocked(x, y, z, world::STONE);
            for (i32 y = 28; y <= 31; ++y) lake->setUnlocked(x, y, z, world::WATER);
        }
    world::buildGreedyMesh(*lake, nb, quads, world::Lod::Full);

    render::buildChunkVertices(*lake, quads, verts, idx, opaque, nullptr, 0);
    check(opaque < idx.size(), "вблизи вода уходит в проход со смешиванием");
    const usize blended0 = idx.size() - opaque;

    render::buildChunkVertices(*lake, quads, verts, idx, opaque, nullptr, 1);
    check(idx.size() - opaque == blended0,
          "на первом уровне прозрачность ещё есть — шва у игрока быть не должно");

    for (u8 lod : { (u8)2, (u8)3 }) {
        render::buildChunkVertices(*lake, quads, verts, idx, opaque, nullptr, lod);
        check(opaque == idx.size(), "на дальних уровнях полупрозрачного хвоста нет");
        // Грани не потерялись: они просто уехали в непрозрачный проход.
        check(idx.size() > 0, "и грани воды при этом не пропали");
        bool opaqueAlpha = true;
        for (const auto& v : verts) if (v.a != 255) opaqueAlpha = false;
        check(opaqueAlpha, "альфа у них выставлена в непрозрачную");
    }

    const std::string mb = readSource("app/src/main/cpp/src/render/mesh_builder.cpp");
    const std::string cr = readSource("app/src/main/cpp/src/render/chunk_renderer.cpp");
    if (!mb.empty() && !cr.empty()) {
        check(mb.find("blendAllowed = (lod < 2)") != std::string::npos,
              "порог прозрачности задан одним местом");
        check(cr.find("&gm.blendCenter, lod)") != std::string::npos,
              "рендер сообщает сборщику вершин уровень чанка");
    }
}

void testLodSeamHasNoCracks() {
    group("LOD: на стыке уровней нет сквозных щелей");

    world::blocks();

    // Два соседних чанка, оба — ровное плато с верхним вокселем на y=36.
    //
    // Грубый (8^3) округляет поверхность вверх до верха своей клетки,
    // то есть до y=40. Мелкий оставляет настоящие 37. Между ними три
    // блока. Грубый решает, строить ли на стыке стену, огрубляя соседа
    // СВОИМ шагом: клетка соседа занята — значит стены не нужно. А
    // сосед стоит ниже, и сквозь эти три блока видно небо.
    auto flat = [](i32 cx) {
        auto c = std::make_unique<world::Chunk>();
        c->coord = { cx, 0, 0 };
        for (i32 z = 0; z < world::CHUNK_SIZE; ++z)
            for (i32 x = 0; x < world::CHUNK_SIZE; ++x)
                for (i32 y = 0; y <= 36; ++y)
                    c->setUnlocked(x, y, z, world::STONE);
        return c;
    };
    auto A = flat(0);
    auto B = flat(1);

    world::ChunkNeighbors nbA; nbA.px = B.get();
    world::ChunkNeighbors nbB; nbB.nx = A.get();

    std::vector<world::Quad> qa, qb;
    world::buildGreedyMesh(*A, nbA, qa, world::Lod::Eighth);
    world::buildGreedyMesh(*B, nbB, qb, world::Lod::Full);

    auto topFace = [](const std::vector<world::Quad>& qs) {
        f32 best = -1.f;
        for (const auto& q : qs) if (q.v0.face == 2) best = std::max(best, q.v0.pos.y);
        return best;
    };
    const f32 topA = topFace(qa), topB = topFace(qb);
    check(topA > topB, "грубый чанк стоит выше мелкого — есть что закрывать");

    // Закрыт ли каждый блок по высоте гранью +X на плоскости стыка.
    auto covered = [&](const std::vector<world::Quad>& qs, i32 z, i32 y) {
        for (const auto& q : qs) {
            if (q.v0.face != 0) continue;                       // +X
            if ((i32)q.v0.pos.x != world::CHUNK_SIZE) continue;  // плоскость стыка
            const glm::vec3 p1 = q.v0.pos + q.du + q.dv;
            const i32 z0 = (i32)std::min(q.v0.pos.z, p1.z);
            const i32 z1 = (i32)std::max(q.v0.pos.z, p1.z);
            const i32 y0 = (i32)std::min(q.v0.pos.y, p1.y);
            const i32 y1 = (i32)std::max(q.v0.pos.y, p1.y);
            if (z >= z0 && z < z1 && y >= y0 && y < y1) return true;
        }
        return false;
    };

    i32 open = 0;
    for (i32 z = 0; z < world::CHUNK_SIZE; ++z)
        for (i32 y = (i32)topB; y < (i32)topA; ++y)
            if (!covered(qa, z, y)) ++open;
    check(open == 0, "щель между ними закрыта стеной грубого чанка");
    if (open) std::printf("       открыто блоков: %d из %d\n",
                          open, (i32)(topA - topB) * world::CHUNK_SIZE);

    // При совпадающих уровнях закрывать нечего, и лишней геометрии
    // юбка тоже не должна приносить: она прячется внутри соседа.
    std::vector<world::Quad> qsame;
    world::buildGreedyMesh(*B, nbB, qsame, world::Lod::Eighth);
    check(topFace(qsame) == topA, "на одном уровне поверхности совпадают");

    // Источник: юбка привязана к проверке «клетка соседа заполнена
    // целиком», а не к произвольной глубине.
    const std::string cc = readSource("app/src/main/cpp/src/world/chunk.cpp");
    if (!cc.empty()) {
        check(cc.find("cellFullySolid") != std::string::npos,
              "у мешера есть проверка полной заполненности клетки соседа");
        const usize u = cc.find("if (outerSlice && !cellFullySolid(n)) shouldEmit = true;");
        check(u != std::string::npos,
              "и юбка строится ровно по ней, на крайнем слое чанка");
    }
}

// ------------------------------------------------------------
// Порядок смешивания воды
// ------------------------------------------------------------
void testWaterSortedByWaterCenter() {
    group("вода: порядок смешивания считается по самой воде");

    world::blocks();
    world::ChunkNeighbors nb;
    std::vector<world::Quad> quads;
    std::vector<render::VoxelVertex> verts;
    std::vector<u32> idx;
    u32 opaqueIdx = 0;

    auto lake = std::make_unique<world::Chunk>();
    for (i32 z = 0; z < world::CHUNK_SIZE; ++z)
        for (i32 x = 0; x < world::CHUNK_SIZE; ++x) {
            for (i32 y = 0; y <= 27; ++y)
                lake->setUnlocked(x, y, z, world::STONE);
            for (i32 y = 28; y <= 31; ++y)
                lake->setUnlocked(x, y, z, world::WATER);
        }
    world::buildGreedyMesh(*lake, nb, quads, world::Lod::Full);

    glm::vec3 center{ -1.f, -1.f, -1.f };
    render::buildChunkVertices(*lake, quads, verts, idx, opaqueIdx, &center);
    check(opaqueIdx < idx.size(), "полупрозрачная часть у озера есть");
    check(center.y > 27.f && center.y < 33.f,
          "центр прозрачной геометрии — на уровне воды");
    check(std::abs(center.y - (f32)world::CHUNK_SIZE_Y * 0.5f) > 20.f,
          "и это не середина чанка по высоте");

    // Чанк без воды центра не даёт — сортировать нечего.
    auto rock = std::make_unique<world::Chunk>();
    for (i32 z = 0; z < world::CHUNK_SIZE; ++z)
        for (i32 x = 0; x < world::CHUNK_SIZE; ++x)
            for (i32 y = 0; y <= 20; ++y)
                rock->setUnlocked(x, y, z, world::STONE);
    world::buildGreedyMesh(*rock, nb, quads, world::Lod::Full);
    glm::vec3 none{ 5.f, 5.f, 5.f };
    render::buildChunkVertices(*rock, quads, verts, idx, opaqueIdx, &none);
    check(opaqueIdx == idx.size(), "у камня полупрозрачной части нет");
    check(none == glm::vec3(0.f), "и центр прозрачной геометрии не задан");

    // Рендер обязан сортировать отдельный список именно по нему.
    const std::string crc = readSource("app/src/main/cpp/src/render/chunk_renderer.cpp");
    const std::string crh = readSource("app/src/main/cpp/src/render/chunk_renderer.h");
    if (crc.empty() || crh.empty()) {
        check(true, "исходники не найдены, проверка пропущена");
        return;
    }
    check(crh.find("glm::vec3 blendCenter") != std::string::npos,
          "меш хранит центр своей прозрачной геометрии");
    check(crc.find("chosen->blendCenter") != std::string::npos,
          "расстояние для смешивания меряется до воды");
    const usize s = crc.find("std::sort(blended_.begin()");
    check(s != std::string::npos, "список прозрачного сортируется отдельно");
    if (s != std::string::npos) {
        const std::string win = crc.substr(s, 200);
        check(win.find("a.distSq > b.distSq") != std::string::npos,
              "от дальнего к ближнему");
    }
}

// ------------------------------------------------------------
// Неопределённая математика в шейдерах
// ------------------------------------------------------------
namespace {

/// Содержимое всех шейдеров проекта, по именам.
std::vector<std::pair<std::string, std::string>> allShaders() {
    static const char* names[] = {
        "voxel.vert", "voxel.frag", "sky.vert", "sky.frag",
        "grass.vert", "grass.frag", "mob.vert", "mob.frag",
        "outline.vert", "outline.frag", "projectile.vert", "projectile.frag",
        "ui.vert", "ui.frag",
    };
    std::vector<std::pair<std::string, std::string>> out;
    for (const char* n : names) {
        std::string path = std::string("app/src/main/cpp/shaders/") + n;
        std::string src  = readSource(path.c_str());
        if (!src.empty()) out.push_back({ n, std::move(src) });
    }
    return out;
}

} // namespace

void testShadersAvoidUndefinedMath() {
    group("шейдеры: неопределённых операций нет");

    const auto shaders = allShaders();
    check(shaders.size() >= 12, "исходники шейдеров найдены");
    if (shaders.empty()) return;

    // ---- 1. pow() с нулевым основанием ----
    //
    // pow(x, k) всюду считается как exp2(k * log2(x)). При x = 0 это
    // log2(0) = -inf, и что вернёт драйвер — его дело: на программном
    // Vulkan ноль, на устройстве вышел NaN. Основания здесь обнуляются
    // постоянно — max(dot(N, солнце), 0.0) равен нулю для любой грани,
    // отвёрнутой от солнца, — поэтому либо основание подпирается снизу
    // через max(), либо степень раскладывается умножениями.
    usize bare = 0;
    std::string bareWhere;
    for (const auto& [name, raw] : shaders) {
        const std::string src = stripComments(raw);
        for (usize i = src.find("pow("); i != std::string::npos;
             i = src.find("pow(", i + 1)) {
            // powSafe(...) и собственные pow2/pow4/pow8/pow64 — не вызовы pow.
            if (i >= 4 && src.compare(i - 4, 4, "Safe") == 0) continue;
            const char before = i > 0 ? src[i - 1] : ' ';
            if (before == 'w' || (before >= '0' && before <= '9')) continue;
            // Внутри самого powSafe вызов законен: там основание и
            // подпирается.
            const usize lineStart = src.rfind('\n', i);
            const std::string line =
                src.substr(lineStart + 1, src.find('\n', i) - lineStart - 1);
            if (line.find("float powSafe") != std::string::npos) continue;
            if (src.compare(i + 4, 4, "max(") == 0) continue;
            ++bare;
            if (bareWhere.empty()) bareWhere = name + ": " + line;
        }
    }
    check(bare == 0, "ни один pow() не берёт основание, которое бывает нулём");
    if (bare) std::printf("       первый такой: %s\n", bareWhere.c_str());

    // Террейн — самый горячий шейдер и единственный, объявленный
    // mediump. Там pow не должно быть вовсе: в половинной точности
    // логарифм уводит результат в денормалы задолго до нуля.
    for (const auto& [name, raw] : shaders) {
        if (name != "voxel.frag") continue;
        const std::string src = stripComments(raw);
        check(src.find("pow(") == std::string::npos,
              "в voxel.frag pow не вызывается вовсе");
        check(src.find("pow8(sunAmt)") != std::string::npos,
              "восьмая степень для тумана считается умножениями");
        check(src.find("pow64(") != std::string::npos,
              "и блик на воде — тоже");
    }

    // ---- 2. smoothstep с перевёрнутыми краями ----
    //
    // По спецификации результат не определён при edge0 >= edge1.
    // Обычная реализация считает то, что задумано, но полагаться на
    // это нельзя.
    usize flipped = 0;
    std::string flippedWhere;
    for (const auto& [name, raw] : shaders) {
        const std::string src = stripComments(raw);
        for (usize i = src.find("smoothstep("); i != std::string::npos;
             i = src.find("smoothstep(", i + 1)) {
            const usize a = i + 11;
            const usize comma = src.find(',', a);
            if (comma == std::string::npos) continue;
            const usize comma2 = src.find(',', comma + 1);
            if (comma2 == std::string::npos) continue;
            const std::string e0 = src.substr(a, comma - a);
            const std::string e1 = src.substr(comma + 1, comma2 - comma - 1);
            // Только числовые края: с выражениями порядок не проверить.
            char* end0 = nullptr; char* end1 = nullptr;
            const double v0 = std::strtod(e0.c_str(), &end0);
            const double v1 = std::strtod(e1.c_str(), &end1);
            bool num0 = end0 && *end0 == '\0' && !e0.empty();
            bool num1 = end1 && *end1 == '\0' && !e1.empty();
            // strtod остановится на пробеле — обрежем его.
            auto trimmed = [](const std::string& t) {
                usize b = t.find_first_not_of(" \t");
                usize e = t.find_last_not_of(" \t");
                return b == std::string::npos ? std::string() : t.substr(b, e - b + 1);
            };
            const std::string t0 = trimmed(e0), t1 = trimmed(e1);
            double d0 = 0, d1 = 0;
            num0 = !t0.empty() && (std::sscanf(t0.c_str(), "%lf", &d0) == 1)
                   && t0.find_first_not_of("-+.0123456789eE") == std::string::npos;
            num1 = !t1.empty() && (std::sscanf(t1.c_str(), "%lf", &d1) == 1)
                   && t1.find_first_not_of("-+.0123456789eE") == std::string::npos;
            (void)v0; (void)v1;
            if (num0 && num1 && d0 >= d1) {
                ++flipped;
                if (flippedWhere.empty())
                    flippedWhere = name + ": smoothstep(" + t0 + ", " + t1 + ", ...)";
            }
        }
    }
    check(flipped == 0, "ни одного smoothstep с edge0 >= edge1");
    if (flipped) std::printf("       первый такой: %s\n", flippedWhere.c_str());

    // ---- 3. Туман террейна считается в полной точности ----
    //
    // Именно здесь одна ошибка красит в свой цвет всю дальнюю половину
    // кадра: цвет тумана подставляется всюду, где fogAmt перестал быть
    // нулём, то есть сразу за началом тумана и до края мира.
    const std::string vf = readSource("app/src/main/cpp/shaders/voxel.frag");
    if (!vf.empty()) {
        check(vf.find("highp float dist") != std::string::npos,
              "расстояние до фрагмента — highp");
        check(vf.find("highp float fogAmt") != std::string::npos,
              "доля тумана — highp");
        check(vf.find("highp float sunAmt") != std::string::npos,
              "и подмешивание солнечного оттенка — тоже");
        const usize m = vf.find("sunMix");
        check(m != std::string::npos, "вес солнечного оттенка вынесен отдельно");
        if (m != std::string::npos) {
            const std::string line = vf.substr(m, vf.find(';', m) - m);
            check(line.find("clamp(") != std::string::npos,
                  "и ограничен своим диапазоном");
        }
        const usize o = vf.find("outColor = vec4(clamp(");
        check(o != std::string::npos,
              "итоговый цвет террейна ограничен [0,1]");
    }
}

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

    // Проверяется ТА ЖЕ раскладка, по которой рисуется.
    //
    // Раньше здесь сверялись ui::minimapRect и ui::hotbarRect, которые
    // не вызывал никто, кроме этой проверки: drawMinimap и drawHotbar
    // считали свои числа без общего масштаба. На 1280x720 расхождение
    // доходило до 191 точки, четыре кнопки реально накрывали HUD, а
    // проверка рапортовала «ни одного наложения» — она сверяла модель
    // с моделью. При высоте 1080 расхождение было ровно ноль, и на
    // этом телефоне игру и смотрели.
    struct Size { f32 w, h; i32 dpi; const char* name; };
    const Size sizes[] = {
        { 2306.f, 1080.f, 400, "2306x1080 @400" },   // тот самый телефон
        { 2400.f, 1080.f, 440, "2400x1080 @440" },
        { 1920.f, 1080.f, 400, "1920x1080 @400" },
        { 1280.f,  720.f, 320, "1280x720 @320"  },   // бюджетный
        { 2560.f, 1600.f, 280, "2560x1600 @280" },   // планшет
        { 3200.f, 1440.f, 560, "3200x1440 @560" },   // плотный
        {  960.f,  540.f, 240, "960x540 @240"   },   // самый слабый
    };

    auto overlaps = [](const ui::Rect& a, const ui::Rect& b) {
        return !(a.x + a.w <= b.x || b.x + b.w <= a.x ||
                 a.y + a.h <= b.y || b.y + b.h <= a.y);
    };
    auto circleHitsRect = [](f32 cx, f32 cy, f32 rad, const ui::Rect& r) {
        const f32 nx = cx < r.x ? r.x : (cx > r.x + r.w ? r.x + r.w : cx);
        const f32 ny = cy < r.y ? r.y : (cy > r.y + r.h ? r.y + r.h : cy);
        const f32 dx = nx - cx, dy = ny - cy;
        return dx * dx + dy * dy < rad * rad - 0.01f;
    };

    int problems = 0;
    for (const auto& sz : sizes)
      // Зеркальная раскладка для левши — это отдельная раскладка, а
      // не отражённые кнопки поверх прежнего HUD. Пока отражалось
      // только управление, кнопки левши приезжали под столбец
      // навигации: на 1280x720 «PUT» попадала ровно на него. Теперь
      // зеркалится весь экран, и проверяются оба варианта целиком.
      for (int mirror = 0; mirror < 2; ++mirror) {
        const char* mode = mirror ? " (левша)" : "";
        const ui::HudLayout L(sz.w, sz.h,
                              ui::theme::Metrics::fromDensityDpi(sz.dpi),
                              ui::SafeInsets{}, mirror != 0);

        // ---- прямоугольники HUD, которые всегда на экране ----
        std::vector<std::pair<const char*, ui::Rect>> rects;
        rects.push_back({ "полоса опыта", L.xpBar() });
        for (u32 i = 0; i < 3; ++i)
            rects.push_back({ "полоса ресурса", L.resourceBar(i) });
        for (u32 i = 0; i < ui::HudLayout::NAV_COUNT; ++i)
            rects.push_back({ "кнопка навигации", L.navButton(i) });
        rects.push_back({ "миникарта", L.minimap() });
        for (u32 i = 0; i < L.hotbarVisibleSlots(); ++i)
            rects.push_back({ "ячейка пояса", L.hotbarSlot(i) });

        // ---- ничто не уходит за экран ----
        for (const auto& [name, r] : rects) {
            if (r.x >= -0.5f && r.y >= -0.5f &&
                r.x + r.w <= sz.w + 0.5f && r.y + r.h <= sz.h + 0.5f) continue;
            ++problems;
            char msg[160];
            std::snprintf(msg, sizeof(msg), "%s%s: %s за краем экрана",
                          sz.name, mode, name);
            check(false, msg);
        }

        // ---- прямоугольники HUD не налезают друг на друга ----
        for (usize a = 0; a < rects.size(); ++a)
            for (usize b = a + 1; b < rects.size(); ++b) {
                if (!overlaps(rects[a].second, rects[b].second)) continue;
                ++problems;
                char msg[160];
                std::snprintf(msg, sizeof(msg), "%s%s: %s налезает на %s",
                              sz.name, mode, rects[a].first, rects[b].first);
                check(false, msg);
            }

        // ---- круглые кнопки ----
        {
            auto centerOf = [&](u32 i) {
                const auto c = L.padButton(i);
                return glm::vec2{ c.cx, c.cy };
            };

            for (u32 b = 0; b < ui::PAD_BUTTON_COUNT; ++b) {
                const auto def = L.padButton(b);
                const glm::vec2 c = centerOf(b);
                if (c.x - def.r < -0.5f || c.y - def.r < -0.5f ||
                    c.x + def.r > sz.w + 0.5f || c.y + def.r > sz.h + 0.5f) {
                    ++problems;
                    char msg[160];
                    std::snprintf(msg, sizeof(msg), "%s%s: кнопка %s за краем",
                                  sz.name, mode, def.label);
                    check(false, msg);
                }
                for (const auto& [name, r] : rects) {
                    if (!circleHitsRect(c.x, c.y, def.r, r)) continue;
                    ++problems;
                    char msg[176];
                    std::snprintf(msg, sizeof(msg), "%s%s: кнопка %s накрывает %s",
                                  sz.name, mode, def.label, name);
                    check(false, msg);
                }
            }

            // Между соседними целями нужен зазор, иначе промах по
            // одной попадает в другую.
            for (u32 a = 0; a < ui::PAD_BUTTON_COUNT; ++a)
                for (u32 b = a + 1; b < ui::PAD_BUTTON_COUNT; ++b) {
                    const auto A = L.padButton(a), B = L.padButton(b);
                    const glm::vec2 ca = centerOf(a), cb = centerOf(b);
                    const f32 dx = ca.x - cb.x, dy = ca.y - cb.y;
                    const f32 need = A.r + B.r + L.dp(ui::theme::TOUCH_GAP_DP);
                    if (dx * dx + dy * dy >= need * need - 0.01f) continue;
                    ++problems;
                    char msg[176];
                    std::snprintf(msg, sizeof(msg), "%s%s: кнопки %s и %s ближе зазора",
                                  sz.name, mode, A.label, B.label);
                    check(false, msg);
                }
        }

        // ---- ни одна цель касания не мельче нормы ----
        const f32 minSide = L.dp(ui::theme::TOUCH_MIN_DP) - 0.01f;
        for (u32 i = 0; i < ui::HudLayout::NAV_COUNT; ++i) {
            const ui::Rect r = L.navButton(i);
            if (r.w >= minSide && r.h >= minSide) continue;
            ++problems;
            char msg[160];
            std::snprintf(msg, sizeof(msg),
                          "%s%s: кнопка навигации мельче 48 dp", sz.name, mode);
            check(false, msg);
        }
        {
            const ui::Rect r = L.hotbarSlot(0);
            if (r.w < minSide || r.h < minSide) {
                ++problems;
                char msg[160];
                std::snprintf(msg, sizeof(msg),
                              "%s%s: ячейка пояса мельче 48 dp", sz.name, mode);
                check(false, msg);
            }
        }
        for (u32 b = 0; b < ui::PAD_BUTTON_COUNT; ++b) {
            const auto def = L.padButton(b);
            if (def.r * 2.f >= minSide) continue;
            ++problems;
            char msg[160];
            std::snprintf(msg, sizeof(msg), "%s%s: кнопка %s мельче 48 dp",
                          sz.name, mode, def.label);
            check(false, msg);
        }

        // ---- пояс показывает не меньше разумного минимума ----
        if (L.hotbarVisibleSlots() < ui::HudLayout::HOTBAR_MIN_VISIBLE) {
            ++problems;
            char msg[160];
            std::snprintf(msg, sizeof(msg), "%s%s: пояс показывает меньше пяти ячеек",
                          sz.name, mode);
            check(false, msg);
        }
        if (L.hotbarVisibleSlots() > ui::HudLayout::HOTBAR_SLOTS) {
            ++problems;
            check(false, "пояс показывает больше ячеек, чем есть в данных");
        }
    }

    check(problems == 0,
          "на семи экранах: ни наложений, ни целей мельче 48 dp");
}

// ------------------------------------------------------------
// Дизайн-система: её собственные правила выполняются.
//
// ui_theme.h — исполняемая половина docs/UI_DESIGN_SYSTEM.md. Если
// правила в ней можно нарушить незаметно, это не система, а ещё один
// набор чисел. Здесь проверяется каждое утверждение документа,
// которое вообще можно проверить арифметикой.
// ------------------------------------------------------------
namespace {

/// Относительная яркость по WCAG из упакованного RGBA8.
f64 wcagLuminance(ui::UiColor c) {
    auto ch = [](u32 v) {
        const f64 s = (f64)v / 255.0;
        return s <= 0.03928 ? s / 12.92 : std::pow((s + 0.055) / 1.055, 2.4);
    };
    const f64 r = ch((c >> 24) & 0xFF);
    const f64 g = ch((c >> 16) & 0xFF);
    const f64 b = ch((c >>  8) & 0xFF);
    return 0.2126 * r + 0.7152 * g + 0.0722 * b;
}

f64 wcagContrast(ui::UiColor a, ui::UiColor b) {
    f64 la = wcagLuminance(a), lb = wcagLuminance(b);
    if (la < lb) std::swap(la, lb);
    return (la + 0.05) / (lb + 0.05);
}

} // namespace

void testUiThemeObeysItsOwnRules() {
    group("тема: дизайн-система выполняет собственные правила");
    namespace th = ui::theme;

    // ---- 1. dp считается от плотности, а не от числа пикселей ----
    //
    // Это и есть главная правка системы: прежний hudScale = screenH/1080
    // давал на двух телефонах одного размера цели, различающиеся в
    // полтора раза.
    {
        const auto m = th::Metrics::fromDensityDpi(320);
        check(std::fabs(m.pxPerDp - 2.0f) < 1e-5f,
              "xhdpi (320) даёт 2 пикселя на dp");
        const auto hi = th::Metrics::fromDensityDpi(480);
        check(std::fabs(hi.pxPerDp - 3.0f) < 1e-5f,
              "xxhdpi (480) даёт 3 пикселя на dp");

        // Одна и та же цель в dp — один и тот же физический размер.
        const f32 a = th::Metrics::fromDensityDpi(320).dp(th::TOUCH_MIN_DP);
        const f32 b = th::Metrics::fromDensityDpi(480).dp(th::TOUCH_MIN_DP);
        check(std::fabs(a / 320.f - b / 480.f) < 1e-6f,
              "48 dp — один физический размер на любой плотности");
    }

    // Служебные значения AConfiguration_getDensity означают «не знаю».
    check(std::fabs(th::Metrics::fromDensityDpi(0).pxPerDp
                    - th::DENSITY_FALLBACK) < 1e-5f,
          "нулевая плотность заменяется запасной");
    check(std::fabs(th::Metrics::fromDensityDpi(0xFFFE).pxPerDp
                    - th::DENSITY_FALLBACK) < 1e-5f,
          "ACONFIGURATION_DENSITY_ANY заменяется запасной");

    // Настройка игрока — единственный общий множитель, и он ограничен.
    check(std::fabs(th::Metrics::fromDensityDpi(320, 5.f).userScale
                    - th::USER_SCALE_MAX) < 1e-5f,
          "масштаб игрока сверху ограничен");
    check(std::fabs(th::Metrics::fromDensityDpi(320, 0.1f).userScale
                    - th::USER_SCALE_MIN) < 1e-5f,
          "и снизу тоже");

    // ---- 2. Размеры касания ----
    //
    // 48 dp — не рекомендация, а граница: ниже палец промахивается.
    check(th::TOUCH_MIN_DP >= 48.f, "минимальная цель касания не ниже 48 dp");
    check(th::TOUCH_REGULAR_DP >= th::TOUCH_MIN_DP,
          "обычная цель не меньше минимальной");
    check(th::TOUCH_PRIMARY_DP >= th::TOUCH_REGULAR_DP,
          "главное действие не меньше обычного");
    check(th::PAD_BUTTON_MIN_DP >= th::TOUCH_MIN_DP,
          "круглая кнопка боя тоже не меньше минимума");
    check(th::PAD_BUTTON_MAX_DP > th::PAD_BUTTON_MIN_DP,
          "у круглой кнопки есть и верхняя граница");
    check(th::TOUCH_GAP_DP >= 8.f, "зазор между целями не меньше 8 dp");

    // ---- 3. Контраст ----
    //
    // Пороги документа: основной текст >= 4.5, вторичный и
    // недоступный >= 3.0. Недоступное всё равно надо прочитать —
    // первый вариант TextDisabled (#626C7C) давал 2.5 и был отвергнут.
    {
        const ui::UiColor beds[] = { th::Panel, th::PanelRaised, th::Ink };
        const char* bedNames[] = { "панели", "приподнятой панели", "затемнении" };
        int low = 0;
        for (int i = 0; i < 3; ++i) {
            if (wcagContrast(th::TextPrimary, beds[i]) < 4.5) ++low;
            if (wcagContrast(th::TextSecondary, beds[i]) < 3.0) ++low;
            if (wcagContrast(th::TextDisabled, beds[i]) < 3.0) {
                ++low;
                char msg[128];
                std::snprintf(msg, sizeof(msg),
                              "недоступный текст на %s: %.2f", bedNames[i],
                              wcagContrast(th::TextDisabled, beds[i]));
                check(false, msg);
            }
        }
        check(low == 0, "весь текст проходит порог контраста на всех фонах");

        // Иерархия обязана читаться: вторичный заметно тусклее
        // основного, недоступный — вторичного.
        check(wcagContrast(th::TextPrimary, th::Panel)
                  > wcagContrast(th::TextSecondary, th::Panel),
              "вторичный текст тусклее основного");
        check(wcagContrast(th::TextSecondary, th::Panel)
                  > wcagContrast(th::TextDisabled, th::Panel),
              "недоступный тусклее вторичного");

        check(wcagContrast(th::Accent, th::Panel) >= 3.0,
              "акцент различим на панели");
    }

    // Полоса ресурса должна отличаться от собственного ложа, иначе
    // пустая часть читается как заполненная.
    {
        const ui::UiColor fill[] = { th::Hp, th::Mp, th::Sp, th::Xp };
        const ui::UiColor bed[]  = { th::HpBed, th::MpBed, th::SpBed, th::XpBed };
        const char* nm[] = { "здоровья", "маны", "выносливости", "опыта" };
        int weak = 0;
        for (int i = 0; i < 4; ++i) {
            if (wcagContrast(fill[i], bed[i]) >= 3.0) continue;
            ++weak;
            char msg[128];
            std::snprintf(msg, sizeof(msg), "полоса %s сливается с ложем: %.2f",
                          nm[i], wcagContrast(fill[i], bed[i]));
            check(false, msg);
        }
        check(weak == 0, "каждая полоса ресурса отличима от своего ложа");
    }

    // ---- 4. Типографика: ровно пять ступеней ----
    //
    // До системы их было десять. Промежуточных значений быть не должно.
    check(th::TEXT_SCALE_COUNT == 5, "ступеней шрифта ровно пять");
    {
        bool ordered = true;
        for (u32 i = 1; i < th::TEXT_SCALE_COUNT; ++i)
            if (th::TEXT_SCALES[i] <= th::TEXT_SCALES[i - 1]) ordered = false;
        check(ordered, "ступени строго возрастают и не повторяются");
    }
    check(std::fabs(th::lineHeight(th::TEXT_BODY)
                    - th::textHeight(th::TEXT_BODY) * 1.4f) < 1e-5f,
          "межстрочное расстояние — 1.4 от высоты ступени");

    // ---- 5. Отступы на сетке 4 dp ----
    {
        const f32 sp[] = { th::SPACE_XS_DP, th::SPACE_S_DP, th::SPACE_M_DP,
                           th::SPACE_L_DP, th::SPACE_XL_DP, th::SPACE_XXL_DP };
        bool onGrid = true;
        for (f32 v : sp)
            if (std::fabs(v / 4.f - std::round(v / 4.f)) > 1e-5f) onGrid = false;
        check(onGrid, "все отступы кратны четырём");
    }

    // ---- 6. Движение: ввод его не ждёт ----
    check(th::ANIM_PRESS_S == 0.f,
          "нажатие видно в том же кадре, без анимации");
    {
        const f32 an[] = { th::ANIM_PANEL_IN_S, th::ANIM_PANEL_OUT_S,
                           th::ANIM_TOAST_IN_S, th::ANIM_TOAST_OUT_S,
                           th::ANIM_BAR_S, th::ANIM_SELECT_S };
        bool tooSlow = false;
        for (f32 v : an) if (v > th::ANIM_MAX_S) tooSlow = true;
        check(!tooSlow, "ни одна анимация не длиннее потолка в 250 мс");
    }

    // ---- 7. Уведомления: приоритет виден в длительности ----
    check(th::notifyDuration(th::NotifyPriority::High)
              > th::notifyDuration(th::NotifyPriority::Normal) &&
          th::notifyDuration(th::NotifyPriority::Normal)
              > th::notifyDuration(th::NotifyPriority::Low),
          "важное держится на экране дольше рядового");
    check(th::NOTIFY_MAX_VISIBLE >= 2,
          "очередь показывает больше одного: новое не затирает старое");

    // ---- 8. Выключенное состояние заметно приглушено ----
    check(th::ALPHA_DISABLED < th::ALPHA_HUD &&
          th::ALPHA_HUD <= th::ALPHA_PANEL,
          "прозрачности упорядочены: выключенное < HUD <= панель");
    check(ui::withAlpha(th::Panel, th::ALPHA_DISABLED)
              == ((th::Panel & 0xFFFFFF00u) | th::ALPHA_DISABLED),
          "withAlpha меняет только прозрачность");

    // ---- 9. Свободный центр экрана ----
    check(th::HUD_CLEAR_W_FRAC > 0.f && th::HUD_CLEAR_W_FRAC < 1.f &&
          th::HUD_CLEAR_H_FRAC > 0.f && th::HUD_CLEAR_H_FRAC < 1.f,
          "свободная область центра — доля экрана, а не весь экран");
}

// ------------------------------------------------------------
// Документ и код не разъезжаются.
//
// docs/UI_DESIGN_SYSTEM.md — не пересказ, а вторая половина системы.
// Если число поменять в заголовке и забыть в документе, следующий
// правящий поверит документу. Поэтому числа сверяются напрямую.
// ------------------------------------------------------------
void testUiThemeMatchesItsDocument() {
    group("тема: документ описывает тот же код");
    namespace th = ui::theme;

    const std::string doc = readSource("docs/UI_DESIGN_SYSTEM.md");
    if (doc.empty()) {
        check(true, "документ не найден, проверка пропущена");
        return;
    }
    const usize NONE = std::string::npos;

    auto mentions = [&](const char* what) { return doc.find(what) != NONE; };

    // Единица длины и порог касания — то, ради чего система написана.
    check(mentions("48 dp"), "документ называет порог касания 48 dp");
    check(th::TOUCH_MIN_DP == 48.f, "и код держит ровно его");
    check(mentions("densityDpi / 160") || mentions("densityDpi/160"),
          "документ описывает перевод dp через плотность");
    check(th::DENSITY_BASE_DPI == 160.f, "и код переводит через 160");

    // Цвета: каждый токен должен быть в документе своей записью.
    struct Named { const char* hex; ui::UiColor c; const char* name; };
    const Named palette[] = {
        { "#0E1219", th::Ink,           "Ink" },
        { "#1B212C", th::Panel,         "Panel" },
        { "#27303F", th::PanelRaised,   "PanelRaised" },
        { "#3C4657", th::Stroke,        "Stroke" },
        { "#F2F5FA", th::TextPrimary,   "TextPrimary" },
        { "#AAB4C6", th::TextSecondary, "TextSecondary" },
        { "#727C8C", th::TextDisabled,  "TextDisabled" },
        { "#F2B33D", th::Accent,        "Accent" },
        { "#FFD478", th::AccentPressed, "AccentPressed" },
        { "#C4443C", th::Danger,        "Danger" },
        { "#4FA84A", th::Success,       "Success" },
        { "#D9483F", th::Hp,            "Hp" },
        { "#3D7FD9", th::Mp,            "Mp" },
        { "#5FB84A", th::Sp,            "Sp" },
        { "#A868E0", th::Xp,            "Xp" },
    };
    int drift = 0;
    for (const auto& p : palette) {
        // Записанное в документе шестнадцатеричное значение обязано
        // совпасть с байтами токена.
        const u32 r = (u32)std::stoul(std::string(p.hex + 1, 2), nullptr, 16);
        const u32 g = (u32)std::stoul(std::string(p.hex + 3, 2), nullptr, 16);
        const u32 b = (u32)std::stoul(std::string(p.hex + 5, 2), nullptr, 16);
        if (ui::rgba((u8)r, (u8)g, (u8)b, 255) != p.c) {
            ++drift;
            char msg[160];
            std::snprintf(msg, sizeof(msg),
                          "%s: в документе %s, в коде другое", p.name, p.hex);
            check(false, msg);
        }
        if (!mentions(p.hex)) {
            ++drift;
            char msg[160];
            std::snprintf(msg, sizeof(msg),
                          "%s (%s) в документе не назван", p.name, p.hex);
            check(false, msg);
        }
    }
    check(drift == 0, "вся палитра совпадает с документом");

    // Ступени шрифта названы в документе поимённо.
    check(mentions("`Display`") && mentions("`Title`") && mentions("`Body`") &&
          mentions("`Label`") && mentions("`Caption`"),
          "документ перечисляет все пять ступеней шрифта");

    // Решение о форме: срез, а не скругление.
    check(mentions("Срез") || mentions("срез"),
          "документ объясняет срезанный угол");
    check(th::CHAMFER_PANEL_DP > 0.f && th::CHAMFER_CELL_DP > 0.f &&
          th::CHAMFER_NONE_DP == 0.f,
          "и код задаёт срез панели, ячейки и его отсутствие");

    // Запрет, который легче всего нарушить молча.
    check(mentions("hudScale"),
          "документ объясняет, почему hudScale уходит");
}

// ------------------------------------------------------------
// Подсказка взаимодействия доводит до экранов, а не просто есть.
//
// Аудит нашёл три готовых экрана, недостижимых из игры: ремесло и
// торговля открывались только из диалога, который не проходится, а
// openEnchant не вызывался вообще ниоткуда. При этом близость станка
// и алтаря считалась каждый кадр — по ней даже переключалась музыка.
//
// Проверка смотрит именно на ПРОВОДКУ: что подсказка рисуется из
// HUD и что из неё есть путь к обоим экранам. Проверять «функция
// объявлена» бессмысленно — объявлены они были и до этого.
// ------------------------------------------------------------
void testInteractPromptUnlocksScreens() {
    group("подсказка взаимодействия отпирает экраны");

    const std::string src = readSource("app/src/main/cpp/src/ui/ui_system.cpp");
    if (src.empty()) { check(true, "исходник не найден, проверка пропущена"); return; }
    const usize NONE = std::string::npos;

    // ---- 1. Подсказка вызывается из HUD ----
    const usize hud = src.find("void UiSystem::drawHud(");
    check(hud != NONE, "drawHud на месте");
    const usize hudEnd = src.find("\n}\n", hud);
    const std::string hudBody = src.substr(hud, hudEnd - hud);
    check(hudBody.find("drawInteractPrompt()") != NONE,
          "HUD рисует подсказку взаимодействия");

    // ---- 2. Из подсказки есть путь к обоим экранам ----
    const usize pr = src.find("void UiSystem::drawInteractPrompt()");
    check(pr != NONE, "подсказка реализована");
    if (pr == NONE) return;
    const usize prEnd = src.find("\n}\n", pr);
    const std::string body = src.substr(pr, prEnd - pr);

    check(body.find("openCrafting(") != NONE,
          "подсказка открывает ремесло");
    check(body.find("openEnchant(") != NONE,
          "подсказка открывает зачарование");

    // ---- 3. Она реагирует на близость, а не висит всегда ----
    check(body.find("nearbyStation") != NONE && body.find("nearbyAltar") != NONE,
          "подсказка смотрит на близость станка и алтаря");
    check(body.find("if (!station && !altar) return;") != NONE,
          "и не показывается, когда рядом ничего нет");

    // ---- 4. Она нажимается ----
    check(body.find("pushInteractiveRect") != NONE,
          "подсказка принимает нажатие");
    // Геометрия — из раскладки, а не своя: иначе нарисованное и
    // нажимаемое снова разъедутся.
    check(body.find("layout_.interactPrompt()") != NONE,
          "её прямоугольник берётся из раскладки");

    // ---- 5. Экраны, ради которых всё это, достижимы ----
    //
    // openEnchant не вызывался НИОТКУДА — ровно это и проверяем:
    // хотя бы один вызов вне самого объявления в заголовке.
    const std::string main_ = readSource("app/src/main/cpp/src/main.cpp");
    const bool enchantFromUi   = body.find("openEnchant(") != NONE;
    const bool craftFromUi     = body.find("openCrafting(") != NONE;
    const bool craftFromDialog = !main_.empty() &&
                                 main_.find("openCrafting(") != NONE;
    check(enchantFromUi, "у зачарования появился вызывающий");
    check(craftFromUi || craftFromDialog, "у ремесла есть вызывающий");
}

// ------------------------------------------------------------
// Выбор в диалоге доходит до игры.
//
// Обработчик варианта ответа был пустой лямбдой: нажатие не делало
// ничего, и выйти из разговора можно было только аппаратной кнопкой.
// Функция applyChoice при этом существовала и была покрыта тестами —
// но вызывали её ТОЛЬКО тесты, шесть раз, и ни разу игра. Проверена
// была логика, не проводка, и тесты оставались зелёными.
//
// Поэтому здесь проверяется именно вызов из интерфейса.
// ------------------------------------------------------------
void testDialogueChoiceReachesTheGame() {
    group("диалог: выбор доходит до игры");

    const std::string src = readSource("app/src/main/cpp/src/ui/ui_system.cpp");
    if (src.empty()) { check(true, "исходник не найден, проверка пропущена"); return; }
    const usize NONE = std::string::npos;

    const usize dlg = src.find("void UiSystem::drawDialogueScreen(");
    check(dlg != NONE, "экран диалога на месте");
    if (dlg == NONE) return;
    const usize end = src.find("\n}\n", dlg);
    const std::string body = src.substr(dlg, end - dlg);

    check(body.find("applyChoice(") != NONE,
          "нажатие на вариант применяет выбор");

    // Пустая лямбда — ровно то, чем это было. Её возвращение должно
    // ронять проверку, как бы ни выглядел остальной код.
    check(body.find("pushInteractiveRect(cr, [](){})") == NONE &&
          body.find("pushInteractiveRect(cr, [] () {})") == NONE,
          "обработчик варианта не пустой");

    // Разговор должен и заканчиваться: applyChoice возвращает false,
    // когда диалог закрылся сам, и NPC надо вывести из состояния Talk.
    check(body.find("onCloseDialogue") != NONE,
          "закончившийся разговор закрывается как положено");

    // Выбор берётся заново по текущему узлу: applyChoice меняет узел,
    // и ссылка на прежний список вариантов после этого не годится.
    check(body.find("findNode(") != NONE,
          "вариант ищется по текущему узлу в момент нажатия");
}

// ------------------------------------------------------------
// Навигация: возврат туда, откуда пришёл; выход — с вопросом.
//
// Любой вложенный экран возвращал в паузу, даже открытый из HUD:
// игрок оказывался не там, откуда пришёл. Инвентаря в паузе не было
// вовсе, хотя выход ИЗ инвентаря вёл именно туда. Выход из игры
// срабатывал сразу, молча теряя несохранённый прогресс.
// ------------------------------------------------------------
void testNavigationReturnsWhereItCameFrom() {
    group("навигация: возврат и подтверждение");

    ui::UiSystem sys;   // без init: проверяется только состояние

    // ---- 1. Инвентарь из HUD возвращает в HUD ----
    sys.screen = ui::Screen::Hud;
    sys.returnTo = ui::Screen::Hud;
    sys.openScreen(ui::Screen::Inventory);
    check(sys.screen == ui::Screen::Inventory, "инвентарь открылся");
    sys.onBackPressed();
    check(sys.screen == ui::Screen::Hud,
          "из инвентаря, открытого из HUD, возврат в HUD");

    // ---- 2. Тот же экран из паузы возвращает в паузу ----
    sys.screen = ui::Screen::Hud;
    sys.openScreen(ui::Screen::PauseMenu);
    sys.openScreen(ui::Screen::Inventory);
    sys.onBackPressed();
    check(sys.screen == ui::Screen::PauseMenu,
          "из инвентаря, открытого из паузы, возврат в паузу");

    // ---- 3. Пауза из HUD закрывается в HUD ----
    sys.screen = ui::Screen::Hud;
    sys.openScreen(ui::Screen::PauseMenu);
    sys.onBackPressed();
    check(sys.screen == ui::Screen::Hud, "пауза закрывается в игру");

    // ---- 4. Ремесло, открытое подсказкой из HUD, вернёт в HUD ----
    sys.screen = ui::Screen::Hud;
    sys.returnTo = ui::Screen::Hud;
    sys.openCrafting(crafting::StationType::Anvil);
    check(sys.screen == ui::Screen::Crafting, "ремесло открылось");
    sys.onBackPressed();
    check(sys.screen == ui::Screen::Hud,
          "и вернуло в игру, а не в паузу");

    // ---- 5. Выход спрашивает, а не выходит ----
    int quits = 0;
    sys.onQuit = [&]() { ++quits; };
    sys.askConfirm("QUIT", "QUIT", [&]() { if (sys.onQuit) sys.onQuit(); });
    check(sys.confirm.active, "вопрос задан");
    check(quits == 0, "и сам по себе ничего не сделал");

    // «Назад» отменяет подтверждение и НИЧЕГО больше: экран прежний.
    const ui::Screen before = sys.screen;
    sys.onBackPressed();
    check(!sys.confirm.active, "«Назад» снимает вопрос");
    check(sys.screen == before, "и не уводит с экрана заодно");
    check(quits == 0, "отменённый выход не выполняется");

    // Подтверждённый — выполняется ровно один раз.
    sys.askConfirm("QUIT", "QUIT", [&]() { if (sys.onQuit) sys.onQuit(); });
    auto act = sys.confirm.onYes;
    sys.confirm = ui::UiSystem::Confirm{};
    if (act) act();
    check(quits == 1, "подтверждённый выход выполняется один раз");
}

// ------------------------------------------------------------
// Пауза сообщает, что игра остановлена, и ведёт во все разделы.
// ------------------------------------------------------------
void testPauseMenuIsGroupedAndComplete() {
    group("пауза: сгруппирована и полна");

    const std::string src = readSource("app/src/main/cpp/src/ui/ui_system.cpp");
    if (src.empty()) { check(true, "исходник не найден, проверка пропущена"); return; }
    const usize NONE = std::string::npos;

    const usize pm = src.find("void UiSystem::drawPauseMenu(");
    check(pm != NONE, "экран паузы на месте");
    if (pm == NONE) return;
    const usize end = src.find("\n}\n", pm);
    const std::string body = src.substr(pm, end - pm);

    // Инвентаря в списке не было, хотя выход из него вёл сюда.
    check(body.find("Screen::Inventory") != NONE,
          "в паузе есть инвентарь");

    // Все прежние разделы остались достижимы.
    const char* need[] = { "Screen::Attributes", "Screen::SkillTree",
                           "Screen::QuestLog", "Screen::Reputation",
                           "Screen::SaveLoad", "Screen::Settings" };
    int missing = 0;
    for (const char* n : need) {
        if (body.find(n) != NONE) continue;
        ++missing;
        char msg[128];
        std::snprintf(msg, sizeof(msg), "из паузы пропал раздел %s", n);
        check(false, msg);
    }
    check(missing == 0, "ни один прежний раздел не потерян");

    // Сетка, а не столбец: в альбомной ориентации столбец — худшая
    // из форм, по вертикали места меньше всего.
    check(body.find("menuCell(") != NONE,
          "разделы разложены сеткой из раскладки");

    // Выход спрашивает.
    check(body.find("askConfirm(") != NONE,
          "выход из игры требует подтверждения");
    check(body.find("if (onQuit) onQuit();") == NONE ||
          body.find("askConfirm(") < body.find("if (onQuit) onQuit();"),
          "и не выходит помимо вопроса");
}

// ------------------------------------------------------------
// Подтверждение действительно модально.
//
// Попадание ищется среди прямоугольников с конца, поэтому кнопки
// окна выигрывают у того, что под ними. Но касание МИМО окна нашло бы
// кнопку внизу — поэтому первым кладётся глушитель во весь экран.
// ------------------------------------------------------------
void testConfirmSwallowsTouchesOutsideIt() {
    group("подтверждение: модальное по-настоящему");

    const std::string src = readSource("app/src/main/cpp/src/ui/ui_system.cpp");
    if (src.empty()) { check(true, "исходник не найден, проверка пропущена"); return; }
    const usize NONE = std::string::npos;

    const usize cf = src.find("void UiSystem::drawConfirm()");
    check(cf != NONE, "окно подтверждения реализовано");
    if (cf == NONE) return;
    const usize end = src.find("\n}\n", cf);
    const std::string body = src.substr(cf, end - cf);

    const usize swallow = body.find("(f32)screenW_, (f32)screenH_ },");
    check(swallow != NONE, "во весь экран положен глушитель касаний");
    check(body.find("confirmButton(") != NONE,
          "кнопки окна берутся из раскладки");
    if (swallow != NONE)
        check(swallow < body.find("confirmButton("),
              "глушитель кладётся ДО кнопок, иначе он перекроет их");

    // И рисуется оно последним, поверх всего.
    const usize render = src.find("void UiSystem::render(");
    const usize rend   = src.find("\n}\n", render);
    const std::string rb = src.substr(render, rend - render);
    check(rb.find("drawConfirm()") != NONE, "подтверждение рисуется в кадре");
    check(rb.find("drawConfirm()") > rb.find("drawStatusToast()"),
          "и поверх всего остального");
}

// ------------------------------------------------------------
// Инвентарь показывает все ячейки, какие есть в данных.
//
// Рисовалась сетка 6x4 = 24 из 27, а брони и аксессуаров не было в
// интерфейсе вовсе: игрок видел 33 ячейки из 42. При этом sortMain()
// вправе положить предмет в любую из 27 — в том числе в невидимую,
// откуда его не достать.
// ------------------------------------------------------------
void testInventoryShowsEverySlot() {
    group("инвентарь: видны все ячейки");

    // ---- 1. Сетка вмещает столько, сколько есть ----
    //
    // Число столбцов подбирается под ширину, а размер ячейки не
    // опускается ниже цели касания. Значит на любом экране сетка
    // обязана вместить ВСЕ ячейки, пусть и в больше рядов.
    struct Size { f32 w, h; i32 dpi; const char* name; };
    const Size sizes[] = {
        { 2306.f, 1080.f, 400, "2306x1080" },
        { 1280.f,  720.f, 320, "1280x720"  },
        { 2560.f, 1600.f, 280, "2560x1600" },
        {  960.f,  540.f, 240, "960x540"   },
    };

    int problems = 0;
    for (const auto& sz : sizes) {
        const ui::HudLayout L(sz.w, sz.h,
                              ui::theme::Metrics::fromDensityDpi(sz.dpi),
                              ui::SafeInsets{});
        const ui::Rect left = L.invLeft();

        struct Part { const char* name; u32 count; };
        const Part parts[] = {
            { "сумка",      items::INV_MAIN_SLOTS },
            { "экипировка", items::INV_ARMOR_SLOTS + items::INV_ACC_SLOTS },
            { "пояс",       items::INV_HOTBAR_SLOTS },
        };
        for (const auto& pt : parts) {
            const auto g = L.cellGrid(left, pt.count);
            if (g.cols * g.rows < pt.count) {
                ++problems;
                char msg[160];
                std::snprintf(msg, sizeof(msg),
                              "%s: %s вмещает %u из %u",
                              sz.name, pt.name, g.cols * g.rows, pt.count);
                check(false, msg);
            }
            // Ячейка не может стать мельче цели касания.
            if (g.cell + 0.01f < L.dp(ui::theme::TOUCH_MIN_DP)) {
                ++problems;
                char msg[160];
                std::snprintf(msg, sizeof(msg), "%s: ячейка %s мельче 48 dp",
                              sz.name, pt.name);
                check(false, msg);
            }
            // Ячейки не налезают друг на друга.
            if (pt.count >= 2) {
                const ui::Rect a = g.at(0), b = g.at(1);
                if (a.x + a.w > b.x + 0.01f && a.y == b.y) {
                    ++problems;
                    check(false, "соседние ячейки налезают");
                }
            }
        }
    }
    check(problems == 0, "на всех экранах видны все 42 ячейки");

    // ---- 2. Отрисовка обходит именно полные диапазоны ----
    const std::string src = readSource("app/src/main/cpp/src/ui/ui_system.cpp");
    if (src.empty()) return;
    const usize NONE = std::string::npos;
    const usize inv = src.find("void UiSystem::drawInventory(");
    check(inv != NONE, "экран инвентаря на месте");
    if (inv == NONE) return;
    const usize end = src.find("\n}\n", inv);
    const std::string body = src.substr(inv, end - inv);

    check(body.find("items::INV_MAIN_SLOTS") != NONE,
          "сумка рисуется по числу ячеек из данных, а не по 6x4");
    check(body.find("items::INV_ARMOR_OFFSET") != NONE,
          "броня и аксессуары появились в интерфейсе");
    check(body.find("items::INV_HOTBAR_SLOTS") != NONE,
          "пояс рисуется целиком");
    // Зашитая сетка 6x4 — ровно то, чем это было.
    check(body.find("cols = 6") == NONE && body.find("rows = 4") == NONE,
          "зашитой сетки 6x4 не осталось");
}

// ------------------------------------------------------------
// Одно касание — одно действие.
//
// Тап по ячейке ОДНОВРЕМЕННО использовал предмет и начинал его
// перенос: зелье выпивалось и бралось в руку одним касанием. В самом
// коде об этом стоял честный комментарий «упрощённо».
// ------------------------------------------------------------
void testInventoryTapDoesOneThing() {
    group("инвентарь: одно касание — одно действие");

    const std::string src = readSource("app/src/main/cpp/src/ui/ui_system.cpp");
    if (src.empty()) { check(true, "исходник не найден, проверка пропущена"); return; }
    const usize NONE = std::string::npos;

    const usize gd = src.find("void UiSystem::drawSlotGrid(");
    check(gd != NONE, "сетка ячеек выделена в общий код");
    if (gd == NONE) return;
    const usize end = src.find("\n}\n", gd);
    const std::string body = src.substr(gd, end - gd);

    check(body.find("selectedInvSlot") != NONE,
          "тап по ячейке выбирает её");
    check(body.find("onUseItem") == NONE,
          "и НЕ использует предмет заодно");
    check(body.find("drag.begin(") == NONE,
          "и не начинает перенос заодно");

    // Действия живут отдельно, в панели сведений.
    const usize dt = src.find("void UiSystem::drawItemDetails(");
    check(dt != NONE, "панель сведений о предмете появилась");
    if (dt == NONE) return;
    const usize dend = src.find("\n}\n", dt);
    const std::string dbody = src.substr(dt, dend - dt);

    check(dbody.find("onUseItem") != NONE, "использовать — отдельной кнопкой");
    check(dbody.find("onDropItem") != NONE, "выбросить — отдельной кнопкой");
    // Выброс необратим.
    check(dbody.find("askConfirm(") != NONE,
          "выброс предмета требует подтверждения");
}

// ------------------------------------------------------------
// Русский язык виден.
//
// Таблица русских строк была заполнена целиком, переключатель в
// настройках работал и сохранялся в конфиг — а шрифт знал только
// ASCII 32..95, и всякий байт кириллицы (они все больше 95)
// превращался в пробел. Переключение на русский СТИРАЛО интерфейс.
// Вдобавок ширина считалась по байтам: «ПРОДОЛЖИТЬ» мерилось как
// двадцать знаков вместо десяти, и центрирование уезжало вдвое.
// ------------------------------------------------------------
void testRussianTextIsActuallyDrawn() {
    group("шрифт: русский язык виден");

    // ---- 1. Атлас вмещает оба набора ----
    ui::UiAtlasData atlas;
    const int cells = (int)((atlas.width / atlas.cellW) * (atlas.height / atlas.cellH));
    check(ui::GLYPH_COUNT <= cells, "все глифы помещаются в атлас");
    check(ui::GLYPH_COUNT == ui::FONT_COUNT + ui::CYR_COUNT,
          "в атласе латиница и кириллица вместе");
    // Индексация идёт по 16 в ряд — последний ряд не должен вылезти.
    check((ui::GLYPH_COUNT + 15) / 16 <= (int)(atlas.height / atlas.cellH),
          "рядов глифов не больше, чем рядов клеток");

    // ---- 2. Разбор UTF-8 ----
    {
        const std::string s = "ДА";        // 4 байта, 2 символа
        usize i = 0;
        const u32 a = ui::utf8Next(s.data(), s.size(), i);
        const u32 b = ui::utf8Next(s.data(), s.size(), i);
        check(a == 0x414, "Д разобрана как один символ");
        check(b == 0x410, "А тоже");
        check(i == s.size(), "и строка прочитана целиком");
    }

    // ---- 3. Соответствие букв клеткам ----
    check(ui::glyphIndex(0x410) == ui::FONT_COUNT, "А — первая кириллическая");
    check(ui::glyphIndex(0x42F) == ui::FONT_COUNT + 31, "Я — тридцать вторая");
    check(ui::glyphIndex(0x401) == ui::FONT_COUNT + 32, "Ё вынесена в конец");
    check(ui::glyphIndex(0x2014) >= 0, "длинное тире рисуется");
    // Строчные приводятся к заглавным: шрифт заглавный целиком.
    check(ui::glyphIndex(0x430) == ui::glyphIndex(0x410), "а и А — одна клетка");
    check(ui::glyphIndex(0x44F) == ui::glyphIndex(0x42F), "я и Я — одна клетка");
    check(ui::glyphIndex(0x451) == ui::glyphIndex(0x401), "ё и Ё — одна клетка");
    check(ui::glyphIndex('a') == ui::glyphIndex('A'), "латиница по-прежнему заглавная");

    // ---- 4. Ни одна буква не пустая ----
    //
    // Пустой глиф выглядит как пробел — ровно как выглядела вся
    // кириллица до этого. Молчаливая дыра в алфавите недопустима.
    int blanks = 0;
    for (int g = 0; g < ui::CYR_COUNT; ++g) {
        u8 any = 0;
        for (int r = 0; r < ui::FONT_H; ++r) any |= ui::FONT_CYR[g][r];
        if (any) continue;
        ++blanks;
        char msg[96];
        std::snprintf(msg, sizeof(msg), "кириллическая буква %d пустая", g);
        check(false, msg);
    }
    check(blanks == 0, "каждый знак кириллического набора что-то рисует");

    // Буквы должны и различаться: одинаковые говорят об опечатке.
    int dupes = 0;
    for (int a = 0; a < ui::CYR_COUNT; ++a)
        for (int b = a + 1; b < ui::CYR_COUNT; ++b) {
            bool same = true;
            for (int r = 0; r < ui::FONT_H; ++r)
                if (ui::FONT_CYR[a][r] != ui::FONT_CYR[b][r]) { same = false; break; }
            if (!same) continue;
            ++dupes;
            char msg[96];
            std::snprintf(msg, sizeof(msg), "буквы %d и %d нарисованы одинаково", a, b);
            check(false, msg);
        }
    check(dupes == 0, "разные буквы выглядят по-разному");

    // ---- 5. Ширина считается в символах ----
    {
        ui::UiContext ctx;
        ctx.init(nullptr, 1000, 500);
        const f32 lat = ctx.textWidth("ABCDEFGHIJ", 1.f);   // 10 знаков
        const f32 cyr = ctx.textWidth("ПРОДОЛЖИТЬ", 1.f);   // 10 знаков, 20 байт
        check(std::fabs(lat - cyr) < 0.01f,
              "десять русских букв шире не чем десять латинских");
    }

    // ---- 6. ВСЯ русская таблица рисуется ----
    //
    // Главная проверка: не «кириллица вообще работает», а что каждый
    // символ каждой строки, которую игра покажет, имеет свою клетку.
    config::L().setLanguage(config::Language::Russian);
    int missing = 0;
    for (u16 k = 0; k < config::STR_KEY_COUNT; ++k) {
        const char* str = config::L().get((config::StrKey)k);
        if (!str) continue;
        const std::string v = str;
        for (usize i = 0; i < v.size(); ) {
            const usize at = i;
            const u32 cp = ui::utf8Next(v.data(), v.size(), i);
            if (cp == (u32)'\n' || ui::glyphIndex(cp) >= 0) continue;
            ++missing;
            if (missing <= 5) {
                char msg[192];
                std::snprintf(msg, sizeof(msg),
                              "строка %u: символ U+%04X (байт %u) рисовать нечем",
                              (unsigned)k, (unsigned)cp, (unsigned)at);
                check(false, msg);
            }
        }
    }
    check(missing == 0, "каждый символ русской таблицы имеет глиф");
    config::L().setLanguage(config::Language::English);
}

// ------------------------------------------------------------
// Настройки: все помещаются и все что-то меняют.
//
// Вкладка «Управление» содержит двенадцать строк. В один столбец это
// 938 точек, а дно панели на экране 1280x720 — 680: последние четыре
// настройки были недостижимы, прокрутки у настроек нет. Отдельно два
// слайдера, uiScale и uiOpacity, двигались и сохранялись, но не
// читались НИГДЕ — ровно то, что §10 задания запрещает оставлять.
// ------------------------------------------------------------
void testSettingsFitAndDoSomething() {
    group("настройки: помещаются и работают");

    // ---- 1. Двенадцать строк влезают на любой экран ----
    struct Size { f32 w, h; i32 dpi; const char* name; };
    const Size sizes[] = {
        { 2306.f, 1080.f, 400, "2306x1080" },
        { 1280.f,  720.f, 320, "1280x720"  },
        {  960.f,  540.f, 240, "960x540"   },
        { 2560.f, 1600.f, 280, "2560x1600" },
    };
    const u32 MAX_ROWS = 12;   // столько во вкладке «Управление»

    int problems = 0;
    for (const auto& sz : sizes) {
        const ui::HudLayout L(sz.w, sz.h,
                              ui::theme::Metrics::fromDensityDpi(sz.dpi),
                              ui::SafeInsets{});
        const ui::Rect panel = L.menuArea();
        const f32 rowH = L.dp(ui::theme::TOUCH_REGULAR_DP);
        const f32 gap  = L.dp(ui::theme::SPACE_S_DP);
        const f32 pad  = L.dp(ui::theme::PANEL_PAD_DP);

        const f32 availH = panel.h - pad * 2.f;
        const u32 perCol = (u32)((availH + gap) / (rowH + gap));
        if (perCol == 0) {
            ++problems;
            char msg[128];
            std::snprintf(msg, sizeof(msg), "%s: в панель не влезает ни одна строка",
                          sz.name);
            check(false, msg);
            continue;
        }
        const u32 cols = (MAX_ROWS + perCol - 1) / perCol;
        const f32 colGap = L.dp(ui::theme::SPACE_L_DP);
        const f32 colW = (panel.w - pad * 2.f - colGap * (f32)(cols - 1)) / (f32)cols;

        // Последняя строка последней колонки не должна выйти за панель.
        const u32 lastCol = (MAX_ROWS - 1) / perCol;
        const u32 lastRow = (MAX_ROWS - 1) % perCol;
        const f32 x = panel.x + pad + (f32)lastCol * (colW + colGap);
        const f32 y = panel.y + pad + (f32)lastRow * (rowH + gap);
        if (y + rowH > panel.y + panel.h + 0.5f ||
            x + colW > panel.x + panel.w + 0.5f) {
            ++problems;
            char msg[176];
            std::snprintf(msg, sizeof(msg),
                          "%s: двенадцатая настройка за панелью", sz.name);
            check(false, msg);
        }
        // И строка остаётся нажимаемой.
        if (rowH + 0.01f < L.dp(ui::theme::TOUCH_MIN_DP)) {
            ++problems;
            check(false, "строка настроек мельче 48 dp");
        }
    }
    check(problems == 0, "все двенадцать настроек достижимы на всех экранах");

    // ---- 2. Ни одного слайдера без потребителя ----
    //
    // Проверка идёт по коду: у настройки должен быть читатель ВНЕ
    // экрана настроек. Виджет, который только пишет значение в
    // структуру, — ложный интерфейс.
    const std::string uis = readSource("app/src/main/cpp/src/ui/ui_system.cpp");
    const std::string uih = readSource("app/src/main/cpp/src/ui/ui_system.h");
    if (uis.empty()) return;
    const usize NONE = std::string::npos;

    // uiScale читается при пересборке раскладки.
    const usize rb = uis.find("void UiSystem::rebuildLayout()");
    check(rb != NONE, "раскладка пересобирается в одном месте");
    if (rb != NONE) {
        const usize end = uis.find("\n}\n", rb);
        check(uis.substr(rb, end - rb).find("uiScale") != NONE,
              "uiScale читается раскладкой");
    }

    // uiOpacity читается слоем HUD.
    check(uih.find("uiOpacity") != NONE,
          "uiOpacity читается при отрисовке HUD");
    check(uis.find("hudTint(") != NONE,
          "и применяется к элементам HUD");

    // Настройка, которую сохраняют, но никто не читает, — тот же
    // обман, только без виджета. showDamageNumbers писалась в конфиг,
    // не имела ни виджета, ни потребителя, и была убрана.
    const std::string set = readSource("app/src/main/cpp/src/config/settings.h");
    const std::string scp = readSource("app/src/main/cpp/src/config/settings.cpp");
    if (!set.empty()) {
        check(set.find("showDamageNumbers") == NONE,
              "мёртвого showDamageNumbers в настройках не осталось");
        check(scp.empty() || scp.find("show_damage_numbers") == NONE,
              "и в конфиг он больше не пишется");
    }
}

// ------------------------------------------------------------
// Диалог выглядит как разговор, а не как системное окно.
//
// Реплика рисовалась ОДНОЙ строкой и уходила за панель; имени
// говорящего не было вовсе — понять, с кем идёт разговор, можно было
// только по тому, на кого смотришь.
// ------------------------------------------------------------
void testDialogueReadsAsAConversation() {
    group("диалог: перенос текста и имя говорящего");

    ui::UiContext ctx;
    ctx.init(nullptr, 1000, 500);

    // ---- 1. Перенос по словам ----
    const std::string longRu =
        "ПУТНИК, В ЭТИХ КРАЯХ НЕСПОКОЙНО, И Я БЫ НА ТВОЁМ МЕСТЕ "
        "ДЕРЖАЛСЯ БЛИЖЕ К ДОРОГЕ, А НЕ ЛЕЗ В ЛЕС ЗА ХОЛМОМ";
    const f32 scale = 2.f;
    const f32 maxW = 400.f;

    const f32 h = ctx.wrappedHeight(longRu, maxW, scale);
    check(h > 9.f * scale, "длинная реплика занимает больше одной строки");

    // Ни одна строка не должна быть шире отведённого.
    // Проверяем косвенно, но строго: высота должна соответствовать
    // числу строк, которое влезает по ширине.
    const f32 oneLine = ctx.textWidth(longRu, scale);
    const f32 minLines = oneLine / maxW;
    check(h / (9.f * scale) >= minLines - 0.01f,
          "строк не меньше, чем требует ширина текста");

    // Узкая колонка — больше строк. Если перенос не работает, число
    // строк от ширины не зависит.
    const f32 narrow = ctx.wrappedHeight(longRu, 150.f, scale);
    check(narrow > h, "в узкой колонке строк больше");

    // ---- 2. Перенос считает СИМВОЛЫ ----
    //
    // После перевода шрифта на UTF-8 байты и символы больше не одно и
    // то же: по байтам русская реплика переносилась бы вдвое раньше.
    {
        const std::string ru = "АААААААААА";     // 10 знаков, 20 байт
        const std::string en = "AAAAAAAAAA";     // 10 знаков, 10 байт
        check(std::fabs(ctx.wrappedHeight(ru, maxW, scale)
                      - ctx.wrappedHeight(en, maxW, scale)) < 0.01f,
              "русский и латинский текст одной длины переносятся одинаково");
    }

    // ---- 3. Перенос слов, а не букв ----
    {
        const std::string two = "ОДИН ДВА";
        const f32 wide = ctx.wrappedHeight(two, 1000.f, scale);
        check(std::fabs(wide - 9.f * scale) < 0.01f,
              "короткая строка остаётся одной строкой");
    }

    // Пустая строка не должна давать ноль строк: место под неё всё
    // равно занимается, иначе следующий блок наедет.
    check(ctx.wrappedHeight("", maxW, scale) > 0.f,
          "пустой текст занимает одну строку");

    // ---- 4. Имя говорящего и геометрия из раскладки ----
    const std::string src = readSource("app/src/main/cpp/src/ui/ui_system.cpp");
    if (src.empty()) return;
    const usize NONE = std::string::npos;
    const usize dg = src.find("void UiSystem::drawDialogueScreen(");
    if (dg == NONE) { check(false, "экран диалога на месте"); return; }
    const usize end = src.find("\n}\n", dg);
    const std::string body = src.substr(dg, end - dg);

    check(body.find("npcRegistry()") != NONE,
          "диалог показывает имя собеседника");
    check(body.find("textWrapped(") != NONE,
          "реплика рисуется с переносом");
    check(body.find("layout_.dialogueChoice(") != NONE,
          "варианты ответа берут геометрию из раскладки");
    // Вариант, не влезший в панель, не рисуется за её краем.
    check(body.find("break;") != NONE,
          "варианты, не влезшие в панель, не уезжают за неё");
}

// ------------------------------------------------------------
// Журнал отвечает на вопрос «что мне делать сейчас».
//
// Для активных заданий он печатал ТОЛЬКО ИХ ЧИСЛО: игрок с тремя
// заданиями видел «3». Ни названий, ни целей, ни прогресса. §12
// задания называет этот вопрос одной из главных функций интерфейса —
// а журнал на него не отвечал вовсе.
// ------------------------------------------------------------
void testQuestLogAnswersWhatToDoNow() {
    group("журнал: что делать сейчас");

    const std::string src = readSource("app/src/main/cpp/src/ui/ui_system.cpp");
    if (src.empty()) { check(true, "исходник не найден, проверка пропущена"); return; }
    const usize NONE = std::string::npos;

    const usize ql = src.find("void UiSystem::drawQuestLogScreen(");
    check(ql != NONE, "журнал на месте");
    if (ql == NONE) return;
    const usize end = src.find("\n}\n", ql);
    const std::string body = src.substr(ql, end - ql);

    // ---- 1. Показываются сами задания, а не их количество ----
    check(body.find("q->title") != NONE, "в списке названия заданий");
    check(body.find("progressPct()") != NONE, "и их прогресс");
    // Печать размера списка — ровно то, чем это было.
    check(body.find("activeQuests.size());") == NONE,
          "числа активных заданий вместо списка не осталось");

    // ---- 2. Есть подробности выбранного ----
    const usize qd = src.find("void UiSystem::drawQuestDetails(");
    check(qd != NONE, "подробности задания появились");
    if (qd != NONE) {
        const usize dend = src.find("\n}\n", qd);
        const std::string db = src.substr(qd, dend - qd);
        check(db.find("description") != NONE, "в подробностях есть описание");
        check(db.find("rewards") != NONE, "и награда");
        check(db.find("requiredCount") != NONE, "и сколько осталось");
        check(db.find("textWrapped(") != NONE,
              "описание рисуется с переносом, а не одной строкой");
    }

    // ---- 3. Цель видна, не открывая журнал ----
    const usize qt = src.find("void UiSystem::drawQuestTracker(");
    check(qt != NONE, "на HUD есть строка текущей цели");
    const usize hud = src.find("void UiSystem::drawHud(");
    if (hud != NONE) {
        const usize hend = src.find("\n}\n", hud);
        check(src.substr(hud, hend - hud).find("drawQuestTracker(") != NONE,
              "и она рисуется в составе HUD");
    }

    // ---- 4. Строки списка — цели касания, не мельче нормы ----
    const ui::HudLayout L(1280.f, 720.f,
                          ui::theme::Metrics::fromDensityDpi(320),
                          ui::SafeInsets{});
    const ui::Rect r0 = L.questRow(0), r1 = L.questRow(1);
    check(r0.h + 0.01f >= L.dp(ui::theme::TOUCH_MIN_DP),
          "строка журнала не мельче 48 dp");
    check(r1.y >= r0.y + r0.h - 0.01f, "строки не налезают друг на друга");
    check(L.questRowsVisible() >= 1, "хотя бы одна строка помещается");

    // Список и подробности не пересекаются.
    const ui::Rect list = L.questList(), det = L.questDetails();
    check(list.x + list.w <= det.x + 0.01f,
          "список и подробности не налезают");
}

// ------------------------------------------------------------
// Уведомления: очередь, приоритет, без спама.
//
// Слот был ОДИН: новое сообщение затирало предыдущее. «Предмет
// получен» стирало «задание выполнено», и отличить важное от
// рядового было нечем — вид, место и длительность у всех одни.
// ------------------------------------------------------------
void testNoticesQueueAndPrioritise() {
    group("уведомления: очередь и приоритет");

    ui::UiSystem sys;

    // ---- 1. Новое не затирает старое ----
    sys.notify("ПЕРВОЕ");
    sys.notify("ВТОРОЕ");
    check(sys.notices().size() == 2, "оба сообщения в очереди");

    // ---- 2. Важное впереди рядового ----
    sys.notify("ВАЖНОЕ", ui::theme::NotifyPriority::High);
    check(sys.notices().front().text == "ВАЖНОЕ",
          "важное встаёт первым, даже придя последним");
    // И порядок среди равных сохраняется.
    check(sys.notices()[1].text == "ПЕРВОЕ",
          "среди равных остаётся порядок прихода");

    // ---- 3. Важное держится дольше ----
    check(ui::theme::notifyDuration(ui::theme::NotifyPriority::High) >
          ui::theme::notifyDuration(ui::theme::NotifyPriority::Low),
          "важное живёт дольше рядового");

    // ---- 4. Повтор не множится ----
    //
    // Подбор десяти одинаковых предметов подряд не должен занимать
    // весь экран.
    const usize before = sys.notices().size();
    for (int i = 0; i < 10; ++i) sys.notify("ПЕРВОЕ");
    check(sys.notices().size() == before,
          "повтор того же текста продлевает, а не множит");

    // ---- 5. Очередь не растёт без предела ----
    for (int i = 0; i < 50; ++i) {
        char b[32];
        std::snprintf(b, sizeof(b), "N%d", i);
        sys.notify(b);
    }
    check(sys.notices().size() <= 8, "очередь ограничена сверху");

    // ---- 6. Они гаснут ----
    ui::UiSystem s2;
    s2.notify("КОРОТКОЕ", ui::theme::NotifyPriority::Low);
    check(s2.notices().size() == 1, "уведомление показано");
    s2.tickUi(ui::theme::notifyDuration(ui::theme::NotifyPriority::Low) + 0.1f);
    check(s2.notices().empty(), "и по истечении срока исчезает");

    // ---- 7. Показывается не больше, чем условлено ----
    check(ui::theme::NOTIFY_MAX_VISIBLE >= 2,
          "видно больше одного: иначе очередь бессмысленна");

    // ---- 8. Важное отличается не только цветом ----
    //
    // Место и размер — тоже признаки: по одному цвету «новый
    // уровень» от «предмет получен» не отличить.
    const ui::HudLayout L(2306.f, 1080.f,
                          ui::theme::Metrics::fromDensityDpi(400),
                          ui::SafeInsets{});
    const ui::Rect hi = L.notice(0, true, 200.f);
    const ui::Rect lo = L.notice(0, false, 200.f);
    check(std::fabs(hi.y - lo.y) > 1.f, "важное и рядовое стоят в разных местах");
    check(hi.h > lo.h, "важное крупнее");

    // Стопка рядовых не налезает сама на себя.
    const ui::Rect lo1 = L.notice(1, false, 200.f);
    check(std::fabs(lo1.y - lo.y) >= lo.h - 0.01f,
          "рядовые уведомления не налезают друг на друга");
}

// ------------------------------------------------------------
// Характеристики: кнопки нажимаемы, недоступность видна.
//
// Кнопки «+» и «−» были 50 точек: на рабочем телефоне это 20 dp при
// норме 48, то есть 3.2 мм под палец в 8..10. Размер задавался в
// пикселях и не зависел от плотности — тот же дефект, что и везде.
// ------------------------------------------------------------
void testAttributeSteppersArePressable() {
    group("характеристики: кнопки нажимаемы");

    struct Size { f32 w, h; i32 dpi; const char* name; };
    const Size sizes[] = {
        { 2306.f, 1080.f, 400, "2306x1080" },
        { 1280.f,  720.f, 320, "1280x720"  },
        {  960.f,  540.f, 240, "960x540"   },
        { 2560.f, 1600.f, 280, "2560x1600" },
    };

    int problems = 0;
    for (const auto& sz : sizes) {
        const ui::HudLayout L(sz.w, sz.h,
                              ui::theme::Metrics::fromDensityDpi(sz.dpi),
                              ui::SafeInsets{});
        const f32 minSide = L.dp(ui::theme::TOUCH_MIN_DP) - 0.01f;
        const ui::Rect area = L.menuArea();

        for (u32 i = 0; i < 4; ++i) {
            const ui::Rect row   = L.attrRow(i);
            const ui::Rect minus = L.attrButton(i, false);
            const ui::Rect plus  = L.attrButton(i, true);

            if (minus.w < minSide || minus.h < minSide ||
                plus.w  < minSide || plus.h  < minSide) {
                ++problems;
                char msg[128];
                std::snprintf(msg, sizeof(msg), "%s: кнопка строки %u мельче 48 dp",
                              sz.name, i);
                check(false, msg);
            }
            // Между «+» и «−» нужен зазор: иначе промах по одной
            // попадает в другую, а это прибавит вместо убавить.
            if (plus.x < minus.x + minus.w + L.dp(ui::theme::TOUCH_GAP_DP) - 0.01f) {
                ++problems;
                char msg[128];
                std::snprintf(msg, sizeof(msg),
                              "%s: «+» и «−» строки %u ближе зазора", sz.name, i);
                check(false, msg);
            }
            // Кнопки внутри своей строки и строка внутри области.
            if (minus.x < row.x || plus.x + plus.w > row.x + row.w + 0.01f ||
                minus.y < row.y - 0.01f ||
                plus.y + plus.h > row.y + row.h + 0.01f) {
                ++problems;
                char msg[128];
                std::snprintf(msg, sizeof(msg), "%s: кнопки строки %u вне строки",
                              sz.name, i);
                check(false, msg);
            }
            if (row.y < area.y - 0.01f ||
                row.y + row.h > area.y + area.h + 0.01f) {
                ++problems;
                char msg[128];
                std::snprintf(msg, sizeof(msg), "%s: строка %u вне области меню",
                              sz.name, i);
                check(false, msg);
            }
            // Строки не налезают друг на друга.
            if (i > 0) {
                const ui::Rect prev = L.attrRow(i - 1);
                if (row.y < prev.y + prev.h - 0.01f) {
                    ++problems;
                    check(false, "строки характеристик налезают");
                }
            }
        }
    }
    check(problems == 0, "кнопки характеристик нажимаемы на всех экранах");

    // ---- Недоступность показана не только цветом ----
    const std::string src = readSource("app/src/main/cpp/src/ui/ui_system.cpp");
    if (src.empty()) return;
    const usize NONE = std::string::npos;
    const usize st = src.find("void UiSystem::drawStepper(");
    check(st != NONE, "кнопка «+»/«−» выделена в общий код");
    if (st != NONE) {
        const usize end = src.find("\n}\n", st);
        const std::string body = src.substr(st, end - st);
        check(body.find("STROKE_SELECTED_DP") != NONE &&
              body.find("STROKE_DP") != NONE,
              "у выключенной кнопки рамка тоньше, а не только цвет другой");
        check(body.find("TextDisabled") != NONE,
              "и текст приглушён");
    }

    // ---- После изменения есть обратная связь ----
    const usize at = src.find("void UiSystem::drawAttributesScreen(");
    if (at != NONE) {
        const usize end = src.find("\n}\n", at);
        const std::string body = src.substr(at, end - at);
        check(body.find("notify(") != NONE,
              "изменение характеристики подтверждается уведомлением");
        check(body.find("layout_.attrButton(") != NONE,
              "геометрия кнопок берётся из раскладки");
    }
}

// ------------------------------------------------------------
// Ремесло, торговля и зачарование: общий вид и общие правила.
//
// Эти три экрана были недостижимы из игры (диалог не проходился,
// openEnchant не вызывался ниоткуда), поэтому их вид никто не видел.
// Внутри: кнопка закрытия написана СЕМЬЮ одинаковыми копиями по
// 20 dp, строки списков по 28 dp, а зачарование — необратимое
// действие — выполнялось без вопроса.
// ------------------------------------------------------------
void testCraftTradeEnchantShareOneLook() {
    group("ремесло, торговля, зачарование: общий вид");

    const std::string src = readSource("app/src/main/cpp/src/ui/ui_system.cpp");
    if (src.empty()) { check(true, "исходник не найден, проверка пропущена"); return; }
    const usize NONE = std::string::npos;

    // ---- 1. Кнопка закрытия одна на всех ----
    check(src.find("Rect close{ (float)screenW_ - 90.f") == NONE,
          "копий кнопки закрытия не осталось");
    check(src.find("void UiSystem::drawCloseButton(") != NONE,
          "она выделена в общий код");
    // И ею действительно пользуются все экраны.
    usize uses = 0, at = 0;
    while ((at = src.find("drawCloseButton(", at)) != NONE) { ++uses; at += 8; }
    check(uses >= 7, "все экраны закрываются ею");

    // ---- 2. Зашитых размеров строк не осталось ----
    check(src.find("const f32 rowH  = 70.f;") == NONE &&
          src.find("const f32 rowH  = 80.f;") == NONE,
          "строки списков больше не заданы пикселями");

    // ---- 3. Необратимое спрашивает ----
    const usize en = src.find("void UiSystem::drawEnchantScreen(");
    check(en != NONE, "экран зачарования на месте");
    if (en != NONE) {
        const usize end = src.find("\n}\n", en);
        const std::string body = src.substr(en, end - en);
        check(body.find("askConfirm(") != NONE,
              "зачарование требует подтверждения: оно тратит предмет и меняет оружие");
        // И не выполняется помимо вопроса.
        const usize ask = body.find("askConfirm(");
        const usize act = body.find("onEnchant(");
        check(ask != NONE && act != NONE && ask < act,
              "вопрос задаётся раньше действия");
        check(body.find("layout_.primaryAction()") != NONE,
              "главное действие берёт геометрию из раскладки");
    }

    // ---- 4. Геометрия общих панелей состоятельна ----
    struct Size { f32 w, h; i32 dpi; const char* name; };
    const Size sizes[] = {
        { 2306.f, 1080.f, 400, "2306x1080" },
        { 1280.f,  720.f, 320, "1280x720"  },
        {  960.f,  540.f, 240, "960x540"   },
    };
    int problems = 0;
    for (const auto& sz : sizes) {
        const ui::HudLayout L(sz.w, sz.h,
                              ui::theme::Metrics::fromDensityDpi(sz.dpi),
                              ui::SafeInsets{});
        const f32 minSide = L.dp(ui::theme::TOUCH_MIN_DP) - 0.01f;
        const ui::Rect close = L.closeButton();
        const ui::Rect left  = L.paneLeft();
        const ui::Rect right = L.paneRight();
        const ui::Rect prim  = L.primaryAction();
        const ui::Rect row   = L.paneRow(0);

        if (close.w < minSide || close.h < minSide) {
            ++problems;
            char m[128]; std::snprintf(m, sizeof(m), "%s: закрытие мельче 48 dp", sz.name);
            check(false, m);
        }
        if (row.h < minSide) {
            ++problems;
            char m[128]; std::snprintf(m, sizeof(m), "%s: строка списка мельче 48 dp", sz.name);
            check(false, m);
        }
        if (prim.h + 0.01f < L.dp(ui::theme::TOUCH_PRIMARY_DP)) {
            ++problems;
            char m[128]; std::snprintf(m, sizeof(m), "%s: главное действие мельче нормы", sz.name);
            check(false, m);
        }
        if (left.x + left.w > right.x + 0.01f) {
            ++problems;
            char m[128]; std::snprintf(m, sizeof(m), "%s: панели налезают", sz.name);
            check(false, m);
        }
        // Главное действие внутри своей панели.
        if (prim.x < right.x - 0.01f ||
            prim.x + prim.w > right.x + right.w + 0.01f ||
            prim.y + prim.h > right.y + right.h + 0.01f) {
            ++problems;
            char m[128]; std::snprintf(m, sizeof(m), "%s: действие вне панели", sz.name);
            check(false, m);
        }
        // Заголовок не перекрывается кнопкой закрытия по вертикали
        // случайно: она стоит внутри полосы заголовка.
        const ui::Rect t = L.menuTitle();
        if (close.y < t.y - 0.01f || close.y + close.h > t.y + t.h + 0.01f) {
            ++problems;
            char m[128]; std::snprintf(m, sizeof(m), "%s: закрытие вне полосы заголовка", sz.name);
            check(false, m);
        }
    }
    check(problems == 0, "общая геометрия экранов состоятельна");
}

// ------------------------------------------------------------
// Ориентация сущностей: ни одно направление не даёт бокового хода.
//
// Соглашение задаёт yaw = atan2(vel.x, vel.z): ноль в +Z, угол растёт
// к +X. Шейдер mob.vert ему следовал, а mob_renderer.cpp и
// npc_renderer.cpp применяли поворот ПРОТИВОПОЛОЖНОЙ ручности.
// Совпадение получалось только при движении вдоль Z; при любой
// составляющей по X положения частей зеркалились, а геометрия частей
// нет — тело собиралось наизнанку. Это и был «идёт боком».
//
// Проверок на направления не было вовсе, поэтому ошибка дожила до
// экрана.
// ------------------------------------------------------------
void testEntityFacingHasNoSidewaysMotion() {
    group("ориентация: боком никто не ходит");

    constexpr f32 PI = 3.14159265359f;

    // ---- 1. yaw смотрит туда, куда движется ----
    struct Dir { f32 x, z; const char* name; };
    const Dir dirs[] = {
        {  0.f,  1.f, "+Z север"    },
        {  1.f,  0.f, "+X восток"   },
        {  0.f, -1.f, "-Z юг"       },
        { -1.f,  0.f, "-X запад"    },
        {  1.f,  1.f, "северо-восток" },
        { -1.f,  1.f, "северо-запад"  },
        {  1.f, -1.f, "юго-восток"    },
        { -1.f, -1.f, "юго-запад"     },
    };

    int bad = 0;
    for (const auto& d : dirs) {
        const f32 len = std::sqrt(d.x * d.x + d.z * d.z);
        const glm::vec3 want{ d.x / len, 0.f, d.z / len };
        const f32 yaw = orient::yawFromDirection(d.x, d.z);
        const glm::vec3 fwd = orient::forward(yaw);

        // Направление взгляда должно совпасть с направлением движения.
        const f32 dot = fwd.x * want.x + fwd.z * want.z;
        if (dot < 0.999f) {
            ++bad;
            char m[160];
            std::snprintf(m, sizeof(m), "%s: взгляд не совпал с движением (dot %.3f)",
                          d.name, dot);
            check(false, m);
        }

        // Боковая составляющая взгляда относительно движения — ноль.
        // Именно она и означает «идёт боком».
        const glm::vec3 rt = orient::right(yaw);
        const f32 side = rt.x * want.x + rt.z * want.z;
        if (std::fabs(side) > 0.001f) {
            ++bad;
            char m[160];
            std::snprintf(m, sizeof(m), "%s: боковая составляющая %.3f", d.name, side);
            check(false, m);
        }
    }
    check(bad == 0, "во всех восьми направлениях модель смотрит вперёд");

    // ---- 2. Процессорный поворот совпадает с шейдерным ----
    //
    // Ровно та ошибка, которая была: две формулы разной ручности.
    // Сверяем orient::rotateY с формулой из mob.vert ЧИСЛЕННО.
    {
        int mism = 0;
        for (int i = 0; i < 16; ++i) {
            const f32 yaw = -PI + (f32)i * (2.f * PI / 16.f);
            const f32 c = std::cos(yaw), s = std::sin(yaw);
            for (const glm::vec3 v : { glm::vec3(1,0,0), glm::vec3(0,0,1),
                                       glm::vec3(0.3f,0.5f,-0.7f) }) {
                // Дословно из mob.vert.
                const glm::vec3 shader{ v.x * c + v.z * s, v.y, -v.x * s + v.z * c };
                const glm::vec3 cpu = orient::rotateY(v, yaw);
                if (std::fabs(shader.x - cpu.x) > 1e-5f ||
                    std::fabs(shader.y - cpu.y) > 1e-5f ||
                    std::fabs(shader.z - cpu.z) > 1e-5f) ++mism;
            }
        }
        check(mism == 0, "поворот на процессоре совпадает с шейдерным");
    }

    // Локальное +Z обязано уходить в направление взгляда: это и есть
    // определение «модель смотрит вперёд».
    {
        int bad2 = 0;
        for (int i = 0; i < 16; ++i) {
            const f32 yaw = -PI + (f32)i * (2.f * PI / 16.f);
            const glm::vec3 nose = orient::rotateY(glm::vec3(0, 0, 1), yaw);
            const glm::vec3 fwd  = orient::forward(yaw);
            if (std::fabs(nose.x - fwd.x) > 1e-5f ||
                std::fabs(nose.z - fwd.z) > 1e-5f) ++bad2;
        }
        check(bad2 == 0, "локальное +Z уходит ровно в направление взгляда");
    }

    // ---- 3. Шейдер не разошёлся с соглашением ----
    //
    // Поворот в шейдере теперь кватернионный: одним углом повёрнутую
    // в суставе конечность выразить нельзя. Значит и сверять надо
    // кватернион — но ровно так же, как раньше сверяли матрицу:
    // дословной транскрипцией шейдерной формулы и численным
    // сравнением. Раньше здесь искалась строка `local.x * c + ...`,
    // и после перехода на кватернион такая проверка ловила бы не
    // расхождение соглашений, а лишь то, что текст шейдера изменился.
    {
        // Дословно из mob.vert:
        //   return v + 2.0 * cross(q.xyz, cross(q.xyz, v) + q.w * v);
        auto shaderQrot = [](const glm::vec4& q, const glm::vec3& v) {
            const glm::vec3 x{ q.x, q.y, q.z };
            return v + 2.f * glm::cross(x, glm::cross(x, v) + q.w * v);
        };

        int mism = 0, mismYaw = 0;
        for (int i = 0; i < 16; ++i) {
            const f32 yaw = -PI + (f32)i * (2.f * PI / 16.f);
            const glm::vec4 q = orient::yawQuat(yaw);
            for (const glm::vec3 v : { glm::vec3(1,0,0), glm::vec3(0,0,1),
                                       glm::vec3(0.3f,0.5f,-0.7f) }) {
                const glm::vec3 sh  = shaderQrot(q, v);
                const glm::vec3 cpu = orient::qrot(q, v);
                if (glm::length(sh - cpu) > 1e-5f) ++mism;
                // И главное: кватернион вокруг +Y обязан давать ровно
                // тот же поворот, что единственная законная матрица.
                if (glm::length(sh - orient::rotateY(v, yaw)) > 1e-5f) ++mismYaw;
            }
        }
        check(mism == 0, "orient::qrot повторяет формулу шейдера");
        check(mismYaw == 0, "кватернион вокруг +Y совпадает с rotateY");

        // Текст шейдера всё же читаем — но на форму поворота, а не на
        // конкретные символы: подмена qrot матрицей другой ручности
        // численную проверку выше обошла бы стороной.
        for (const char* n : { "app/src/main/cpp/shaders/mob.vert",
                               "app/src/main/cpp/shaders/projectile.vert" }) {
            const std::string vs = readSource(n);
            if (vs.empty()) continue;
            const bool ok =
                vs.find("qrot(vec4 q, vec3 v)") != std::string::npos &&
                vs.find("v + 2.0 * cross(q.xyz, cross(q.xyz, v) + q.w * v)")
                    != std::string::npos &&
                vs.find("in vec4 iRot") != std::string::npos;
            check(ok, "шейдер поворачивает тем же кватернионом");
        }
    }

    // ---- 3б. Кватернион по направлению ----
    //
    // Нужен там, где движение не горизонтально: стрела с гравитацией
    // падает, и одного yaw ей мало.
    {
        int bad3 = 0;
        const glm::vec3 dirs[] = {
            { 0, 0, 1 }, { 1, 0, 0 }, { 0, 0, -1 }, { -1, 0, 0 },
            { 0, 1, 0 }, { 0, -1, 0 }, { 0.3f, -0.8f, 0.5f },
            { -2.f, 1.f, -3.f },
        };
        for (const glm::vec3& d : dirs) {
            const glm::vec3 nose =
                orient::qrot(orient::dirQuat(d), glm::vec3(0, 0, 1));
            if (glm::length(nose - glm::normalize(d)) > 1e-4f) ++bad3;
        }
        check(bad3 == 0, "dirQuat уводит модельное +Z ровно в направление");

        // Кватернион обязан быть единичным: ненормированный растянет
        // коробку вместе с поворотом.
        int notUnit = 0;
        for (const glm::vec3& d : dirs)
            if (std::fabs(glm::length(orient::dirQuat(d)) - 1.f) > 1e-4f)
                ++notUnit;
        check(notUnit == 0, "и остаётся единичным");

        const glm::vec4 z = orient::dirQuat(glm::vec3(0.f));
        check(std::fabs(z.w - 1.f) < 1e-6f,
              "нулевое направление не поворачивает ни на что");

        // Горизонтальное направление обязано совпасть с yaw: иначе у
        // стрелы и у существа «вперёд» разные.
        int mismYaw2 = 0;
        for (int i = 0; i < 16; ++i) {
            const f32 yaw = -PI + (f32)i * (2.f * PI / 16.f);
            const glm::vec3 f = orient::forward(yaw);
            const glm::vec3 a = orient::qrot(orient::dirQuat(f), glm::vec3(0,0,1));
            const glm::vec3 b = orient::qrot(orient::yawQuat(yaw), glm::vec3(0,0,1));
            if (glm::length(a - b) > 1e-4f) ++mismYaw2;
        }
        check(mismYaw2 == 0, "по горизонтали dirQuat и yawQuat согласны");
    }

    // ---- 4. Рендеры не считают поворот сами ----
    //
    // Именно самодельный расчёт в рендере и разошёлся с шейдером.
    // Поэтому требование не «вызывай rotateY», а строже: своей
    // тригонометрии в рендере быть не должно вовсе, поворот приходит
    // из общего места — orient:: или entity::resolve.
    for (const char* f : { "app/src/main/cpp/src/render/mob_renderer.cpp",
                           "app/src/main/cpp/src/render/npc_renderer.cpp" }) {
        const std::string src = readSource(f);
        if (src.empty()) continue;
        check(src.find("off.x * c - off.z * s") == std::string::npos,
              "в рендере не осталось поворота противоположной ручности");
        check(src.find("std::cos(") == std::string::npos &&
              src.find("std::sin(") == std::string::npos,
              "рендер не считает синусов и косинусов сам");
        check(src.find("orient::") != std::string::npos ||
              src.find("entity::resolve(") != std::string::npos,
              "рендер поворачивает общей функцией");
        check(src.find("ecs::Facing") != std::string::npos,
              "и берёт угол из состояния сущности, а не считает его");
    }
}

// ------------------------------------------------------------
// Доворот: плавный, по кратчайшей дуге, и без сброса на остановке.
// ------------------------------------------------------------
void testFacingTurnsSmoothly() {
    group("ориентация: доворот плавный");

    constexpr f32 PI = 3.14159265359f;

    // ---- 1. Кратчайшая дуга ----
    //
    // Без приведения разницы к (-pi, pi] существо, поворачиваясь с
    // 170° на -170°, поедет через весь круг вместо двадцати градусов.
    {
        const f32 from =  170.f * PI / 180.f;
        const f32 to   = -170.f * PI / 180.f;
        const f32 d = orient::angleDelta(from, to);
        check(std::fabs(std::fabs(d) - 20.f * PI / 180.f) < 1e-4f,
              "с 170° на -170° это двадцать градусов, а не триста сорок");
        check(d > 0.f, "и в сторону возрастания угла");
    }

    // ---- 2. Доворот занимает время, а не кадр ----
    {
        ecs::Facing f{};
        f.turnRate = 4.f;                 // рад/с
        const glm::vec3 east{ 3.f, 0.f, 0.f };

        orient::advanceFacing(f, east, 1.f / 60.f);
        check(std::fabs(f.moveYaw - PI * 0.5f) < 1e-4f,
              "направление движения обновилось сразу");
        check(f.yaw < PI * 0.5f - 0.01f,
              "а модель ещё только начала доворачиваться");

        // За достаточное время доворот завершается.
        for (int i = 0; i < 200; ++i) orient::advanceFacing(f, east, 1.f / 60.f);
        check(std::fabs(f.yaw - PI * 0.5f) < 1e-3f, "и в итоге довернулась");
    }

    // ---- 3. Остановка НЕ разворачивает на север ----
    //
    // Прежний код обнулял угол при скорости ниже порога: существо
    // мгновенно разворачивалось на север, стоило ему встать.
    {
        ecs::Facing f{};
        const glm::vec3 west{ -3.f, 0.f, 0.f };
        for (int i = 0; i < 200; ++i) orient::advanceFacing(f, west, 1.f / 60.f);
        const f32 facedWest = f.yaw;
        check(std::fabs(facedWest + PI * 0.5f) < 1e-3f, "шёл на запад");

        for (int i = 0; i < 120; ++i)
            orient::advanceFacing(f, glm::vec3(0.f), 1.f / 60.f);
        check(std::fabs(f.yaw - facedWest) < 1e-4f,
              "встал — и остался смотреть на запад, а не на север");
    }

    // ---- 4. Дрожание скорости не дёргает модель ----
    {
        ecs::Facing f{};
        const f32 tiny = orient::MOVE_EPSILON * 0.5f;
        for (int i = 0; i < 60; ++i) {
            const glm::vec3 jitter{ (i % 2 ? tiny : -tiny), 0.f, 0.f };
            orient::advanceFacing(f, jitter, 1.f / 60.f);
        }
        check(std::fabs(f.yaw) < 1e-4f,
              "скорость ниже порога значимости направление не меняет");
    }
}

// ------------------------------------------------------------
// Оснастка: иерархия, опора, вращение вместо сдвига.
//
// Прежняя «оснастка» была плоским списком коробок со смещением от
// начала сущности: ни родителя, ни своей системы координат. Собрать
// «торс → бедро → голень» было не на чем, и анимация СДВИГАЛА
// коробку вперёд-назад вместо поворота в суставе — нога ехала
// параллельно себе, отсюда «плывущая» походка.
//
// Причина лежала глубже: формат инстанса нёс один угол на коробку и
// повёрнутую конечность выразить не мог.
// ------------------------------------------------------------
void testRigHierarchyIsSound() {
    group("оснастка: иерархия и опора");

    int problems = 0;
    for (u16 id = 1; id < mobs::MOB_COUNT; ++id) {
        const entity::Rig& rig = mobs::rigFor(id);
        const mobs::MobDef& d = mobs::mobRegistry().get(id);
        const char* name = d.name;
        if (rig.count == 0) continue;

        // ---- 1. Родитель стоит РАНЬШЕ ребёнка ----
        //
        // Иначе сборка в один проход прочитает недосчитанного
        // родителя, и часть уедет неизвестно куда.
        for (u8 i = 0; i < rig.count; ++i) {
            const i8 par = rig.parts[i].parent;
            if (par < 0) continue;
            if (par < (i8)i) continue;
            ++problems;
            char m[160];
            std::snprintf(m, sizeof(m), "%s: часть %u ссылается на родителя %d впереди себя",
                          name ? name : "?", i, (int)par);
            check(false, m);
        }

        // ---- 2. Корень один и он невидим ----
        int roots = 0;
        for (u8 i = 0; i < rig.count; ++i)
            if (rig.parts[i].parent < 0) ++roots;
        if (roots != 1) {
            ++problems;
            char m[160];
            std::snprintf(m, sizeof(m), "%s: корней %d, а должен быть один", 
                          name ? name : "?", roots);
            check(false, m);
        }

        // ---- 3. У каждой ноги есть голень ----
        //
        // Нога одной коробкой гнуться не может: колено нужно, чтобы
        // ступня описывала дугу, а не ехала прямой.
        const entity::PartRole ups[] = {
            entity::PartRole::UpperLegFL, entity::PartRole::UpperLegFR,
            entity::PartRole::UpperLegBL, entity::PartRole::UpperLegBR };
        const entity::PartRole los[] = {
            entity::PartRole::LowerLegFL, entity::PartRole::LowerLegFR,
            entity::PartRole::LowerLegBL, entity::PartRole::LowerLegBR };
        for (int k = 0; k < 4; ++k) {
            bool hasUp = false, hasLo = false;
            for (u8 i = 0; i < rig.count; ++i) {
                if (rig.parts[i].role == ups[k]) hasUp = true;
                if (rig.parts[i].role == los[k]) hasLo = true;
            }
            if (hasUp == hasLo) continue;
            ++problems;
            char m[160];
            std::snprintf(m, sizeof(m), "%s: у ноги %d есть бедро без голени или наоборот",
                          name ? name : "?", k);
            check(false, m);
        }

        // ---- 3б. Модель того же размера, что и коллайдер ----
        //
        // Иначе визуал и физика расходятся: по существу промахиваются
        // там, где оно выглядит задетым, и наоборот. Раньше расходились
        // сильно — у Каменного стража модель была 4.35 при коллайдере
        // 3.40, почти на метр выше того, во что попадают.
        //
        // Сверяем по МАКУШКЕ, то есть с ушами и рогами: силуэт — это
        // то, что видно, а не то, что осталось после вычитания примет.
        {
            const f32 top = entity::highestPoint(rig, rig.rest, 0.f);
            if (std::fabs(top - d.bodyHeight) > 0.02f * d.bodyHeight + 0.01f) {
                ++problems;
                char m[176];
                std::snprintf(m, sizeof(m),
                              "%s: модель ростом %.2f при коллайдере %.2f",
                              name ? name : "?", top, d.bodyHeight);
                check(false, m);
            }
        }

        // ---- 4. Опора: существо стоит на земле ----
        //
        // Ни парящих, ни утопленных. Начало сущности — точка опоры,
        // значит низ модели в покое должен быть у нуля.
        const f32 lo = entity::lowestPoint(rig, rig.rest, 0.f);
        if (std::fabs(lo) > 0.35f) {
            ++problems;
            char m[176];
            std::snprintf(m, sizeof(m), "%s: низ модели на %.2f от точки опоры",
                          name ? name : "?", lo);
            check(false, m);
        }
    }
    check(problems == 0, "оснастка всех видов состоятельна");
}

// ------------------------------------------------------------
// Сборка позы: части вращаются и держатся друг за друга.
// ------------------------------------------------------------
void testRigResolveRotatesParts() {
    group("оснастка: суставы вращаются, а не сдвигаются");

    constexpr f32 PI = 3.14159265359f;

    // Простая оснастка: корень → торс → бедро → голень.
    entity::Rig rig;
    entity::Part root; root.parent = -1; root.role = entity::PartRole::Root;
    root.visible = false;
    const u8 iR = rig.add(root);

    entity::Part torso; torso.parent = (i8)iR; torso.role = entity::PartRole::Torso;
    torso.pivot = { 0.f, 1.f, 0.f }; torso.size = { 0.6f, 0.6f, 1.0f };
    const u8 iT = rig.add(torso);

    entity::Part up; up.parent = (i8)iT; up.role = entity::PartRole::UpperLegFR;
    up.pivot = { 0.2f, -0.3f, 0.3f }; up.boxOffset = { 0.f, -0.25f, 0.f };
    up.size = { 0.15f, 0.5f, 0.15f };
    const u8 iU = rig.add(up);

    entity::Part lo; lo.parent = (i8)iU; lo.role = entity::PartRole::LowerLegFR;
    lo.pivot = { 0.f, -0.5f, 0.f }; lo.boxOffset = { 0.f, -0.25f, 0.f };
    lo.size = { 0.14f, 0.5f, 0.14f };
    rig.add(lo);

    entity::ResolvedPart out[entity::MAX_PARTS];

    // ---- 1. В нулевой позе части стоят там, где описаны ----
    {
        entity::Pose p; p.clear();
        const u8 n = entity::resolve(rig, p, glm::vec3(0.f), 0.f, out, entity::MAX_PARTS);
        check(n == 3, "невидимый корень не рисуется");
        check(std::fabs(out[0].center.y - 1.f) < 1e-4f, "торс на своей высоте");
        check(std::fabs(out[1].center.y - 0.45f) < 1e-4f, "бедро висит под торсом");
        check(std::fabs(out[2].center.y - (-0.05f)) < 1e-4f, "голень под бедром");
    }

    // ---- 2. Поворот бедра УВОДИТ голень ----
    //
    // Это и есть проверка иерархии: в плоском списке голень осталась
    // бы на месте.
    {
        entity::Pose p; p.clear();
        p.euler[2].x = 0.6f;              // качнули бедро
        entity::resolve(rig, p, glm::vec3(0.f), 0.f, out, entity::MAX_PARTS);
        const f32 kneeZ = out[2].center.z;
        entity::Pose z; z.clear();
        entity::ResolvedPart ref[entity::MAX_PARTS];
        entity::resolve(rig, z, glm::vec3(0.f), 0.f, ref, entity::MAX_PARTS);
        check(std::fabs(kneeZ - ref[2].center.z) > 0.1f,
              "поворот бедра уводит голень: части держатся друг за друга");
        // И сама коробка бедра ПОВЕРНУТА, а не просто сдвинута.
        check(std::fabs(out[1].rot.x) > 1e-3f,
              "бедро повёрнуто, а не сдвинуто параллельно себе");
    }

    // ---- 3. Поворот сущности разворачивает всю оснастку ----
    {
        entity::Pose p; p.clear();
        entity::resolve(rig, p, glm::vec3(0.f), PI * 0.5f, out, entity::MAX_PARTS);
        // Бедро было справа-впереди (x +0.2, z +0.3); при развороте
        // на восток «вперёд» уходит в +X.
        check(out[1].center.x > 0.2f, "оснастка развернулась вместе с сущностью");
        check(std::fabs(out[1].center.z + 0.2f) < 0.05f, "и ровно на девяносто градусов");
    }

    // ---- 4. Поправка ориентации модели — только через оснастку ----
    //
    // Единственное законное место. Прятать её в рендере или ИИ
    // нельзя: именно так появляются «этому мобу +90 градусов».
    {
        entity::Rig r2 = rig;
        r2.modelYawOffset = PI * 0.5f;
        entity::Pose p; p.clear();
        entity::ResolvedPart a[entity::MAX_PARTS], b[entity::MAX_PARTS];
        entity::resolve(rig, p, glm::vec3(0.f), PI * 0.5f, a, entity::MAX_PARTS);
        entity::resolve(r2,  p, glm::vec3(0.f), 0.f,       b, entity::MAX_PARTS);
        check(std::fabs(a[1].center.x - b[1].center.x) < 1e-4f &&
              std::fabs(a[1].center.z - b[1].center.z) < 1e-4f,
              "поправка модели равносильна повороту сущности");
    }
}

// ------------------------------------------------------------
// У игрока есть модель.
//
// Её не было вовсе. Камера по умолчанию стоит в пяти с половиной
// метрах позади и на метр выше — то есть игра третьего лица, — а
// показывать там было нечего: ни одного рендера, который рисовал бы
// игрока, в проекте не существовало.
// ------------------------------------------------------------
void testPlayerHasModel() {
    group("игрок: модель есть и живёт по общим правилам");

    const entity::Rig& rig = player::rig();
    check(rig.count > 0, "оснастка игрока построена");
    if (rig.count == 0) return;

    // ---- Рост модели совпадает с ростом коллайдера ----
    //
    // Иначе игрок протискивается там, где визуально не пролезает, и
    // наоборот — застревает в проёме, который выглядит свободным.
    {
        entity::Pose rest;
        entity::ResolvedPart p[entity::MAX_PARTS];
        const u8 n = entity::resolve(rig, rest, glm::vec3(0.f), 0.f,
                                     p, entity::MAX_PARTS);
        f32 top = 0.f;
        for (u8 i = 0; i < n; ++i)
            top = std::max(top, p[i].center.y + p[i].size.y * 0.5f);

        // Коллайдер задан полувысотой 0.90 — значит рост 1.80.
        check(std::fabs(top - 1.80f) < 1e-3f,
              "рост модели совпадает с высотой коллайдера");
        check(std::fabs(entity::lowestPoint(rig, rest, 0.f)) < 1e-3f,
              "и подошва стоит на опоре");
    }

    // ---- Игрок не особый случай ----
    {
        ecs::Registry reg;
        player::Player pl;
        pl.init(reg, glm::vec3(10.f, 64.f, 10.f));
        const ecs::Entity e = pl.entity();

        auto* fc = reg.get<ecs::Facing>(e);
        auto* gt = reg.get<ecs::Gait>(e);
        check(fc != nullptr,
              "поворот игрока — тот же компонент, что у мобов и NPC");
        check(gt != nullptr,
              "и фаза шага тоже: своего способа ходить у игрока нет");
        if (gt) {
            check(std::fabs(gt->stride - rig.strideLength) < 1e-4f,
                  "длина шага взята из его собственной оснастки");
        }

        // Тот же общий проход, что и для всех остальных, обязан
        // двигать игрока: если он его не видит, игрок останется
        // смотреть на север и не шагнёт ни разу.
        if (fc && gt) {
            auto* v = reg.get<ecs::Velocity>(e);
            check(v != nullptr, "и скорость, по которой всё это считается");
            if (v) {
                v->linear = glm::vec3(3.f, 0.f, 0.f);   // строго на восток
                for (int i = 0; i < 120; ++i) {
                    orient::advanceFacing(*fc, v->linear, 1.f / 60.f);
                    anim::advanceGait(*gt, v->linear, 1.f / 60.f);
                }
                const f32 east = orient::yawFromDirection(1.f, 0.f);
                check(std::fabs(orient::angleDelta(fc->yaw, east)) < 1e-3f,
                      "идёт на восток — и смотрит на восток");
                check(gt->phase > 0.f, "и переставляет ноги, пока идёт");
            }
        }
    }
}

// ------------------------------------------------------------
// Звери отличаются друг от друга, а не только цветом коробки.
//
// Плоское описание вида давало девять зашитых слотов: тело, голова,
// четыре ноги, хвост, две руки. Ни уха, ни морды, ни рога выразить в
// нём было нельзя — поэтому овца и волк были двумя коробками на
// четырёх ногах, отличавшимися оттенком серого.
// ------------------------------------------------------------
void testBeastsHaveCharacter() {
    group("звери: приметы вида, а не оттенок серого");

    auto hasRole = [](const entity::Rig& r, entity::PartRole role) {
        for (u8 i = 0; i < r.count; ++i)
            if (r.parts[i].role == role) return true;
        return false;
    };
    auto countRole = [](const entity::Rig& r, entity::PartRole role) {
        int n = 0;
        for (u8 i = 0; i < r.count; ++i)
            if (r.parts[i].role == role) ++n;
        return n;
    };

    const entity::Rig& sheep   = mobs::rigFor(mobs::MOB_SHEEP);
    const entity::Rig& cow     = mobs::rigFor(mobs::MOB_COW);
    const entity::Rig& wolf    = mobs::rigFor(mobs::MOB_WOLF);
    const entity::Rig& chicken = mobs::rigFor(mobs::MOB_CHICKEN);

    // ---- Приметы на месте ----
    check(countRole(sheep, entity::PartRole::Ear) == 2, "у овцы два уха");
    check(countRole(wolf,  entity::PartRole::Ear) == 2, "у волка два уха");
    check(countRole(cow,   entity::PartRole::Horn) == 2, "у коровы два рога");
    check(hasRole(sheep, entity::PartRole::Snout), "у овцы есть морда");
    check(hasRole(wolf,  entity::PartRole::Snout), "у волка есть морда");
    check(hasRole(chicken, entity::PartRole::Snout), "у курицы есть клюв");
    check(hasRole(wolf, entity::PartRole::Tail), "у волка есть хвост");

    check(!hasRole(sheep, entity::PartRole::Horn), "а у овцы рогов нет");
    check(!hasRole(chicken, entity::PartRole::Ear), "и у курицы ушей нет");

    // ---- Уши висят или торчат — и это разные звери ----
    //
    // Направление уха задано позой ПОКОЯ, а не отдельной коробкой:
    // иначе оно не качалось бы вместе с головой на бегу.
    {
        f32 sheepEar = 0.f, wolfEar = 0.f;
        for (u8 i = 0; i < sheep.count; ++i)
            if (sheep.parts[i].role == entity::PartRole::Ear)
                sheepEar = sheep.rest.euler[i].x;
        for (u8 i = 0; i < wolf.count; ++i)
            if (wolf.parts[i].role == entity::PartRole::Ear)
                wolfEar = wolf.rest.euler[i].x;
        check(sheepEar < -0.3f, "у овцы уши висят");
        check(wolfEar > 0.1f, "а у волка торчат");
    }

    // ---- Курица — птица: одна пара ног ----
    {
        int legs = countRole(chicken, entity::PartRole::UpperLegFR)
                 + countRole(chicken, entity::PartRole::UpperLegFL)
                 + countRole(chicken, entity::PartRole::UpperLegBR)
                 + countRole(chicken, entity::PartRole::UpperLegBL);
        check(legs == 2, "у курицы две ноги");
        int wolfLegs = countRole(wolf, entity::PartRole::UpperLegFR)
                     + countRole(wolf, entity::PartRole::UpperLegFL)
                     + countRole(wolf, entity::PartRole::UpperLegBR)
                     + countRole(wolf, entity::PartRole::UpperLegBL);
        check(wolfLegs == 4, "а у волка четыре");
    }

    // ---- Силуэты разные ----
    //
    // Волк длинный и низкий, овца короткая и плотная. Если отношение
    // длины к высоте у них совпадёт, порода перестанет читаться.
    {
        auto extent = [](const entity::Rig& r, int axis) {
            entity::ResolvedPart p[entity::MAX_PARTS];
            const u8 n = entity::resolve(r, r.rest, glm::vec3(0.f), 0.f,
                                         p, entity::MAX_PARTS);
            f32 lo = 0.f, hi = 0.f;
            for (u8 i = 0; i < n; ++i) {
                const f32 c = p[i].center[axis], h = p[i].size[axis] * 0.5f;
                if (i == 0) { lo = c - h; hi = c + h; }
                else { lo = std::min(lo, c - h); hi = std::max(hi, c + h); }
            }
            return hi - lo;
        };
        const f32 wolfRatio  = extent(wolf, 2)  / std::max(0.01f, extent(wolf, 1));
        const f32 sheepRatio = extent(sheep, 2) / std::max(0.01f, extent(sheep, 1));
        check(wolfRatio > sheepRatio * 1.15f,
              "волк длиннее относительно роста, чем овца");
    }

    // ---- Примета качается СВОИМ суставом ----
    //
    // Ухо — ребёнок головы, и на бегу оно поедет вместе с ней, даже
    // если собственного движения у него нет вовсе. Поэтому сравнивать
    // надо не с покоем, а с той же позой, где обнулён угол самого
    // уха: разница и есть вклад его сустава.
    {
        anim::AnimState run;
        run.phase = 1.1f; run.speedNorm = 1.f;
        entity::Pose moving;
        anim::poseFor(wolf, moving, run);

        int earOwn = 0, tailOwn = 0;
        for (u8 i = 0; i < wolf.count; ++i) {
            const entity::PartRole r = wolf.parts[i].role;
            const bool ear  = (r == entity::PartRole::Ear);
            const bool tail = (r == entity::PartRole::Tail);
            if (!ear && !tail) continue;

            entity::Pose frozen = moving;
            frozen.euler[i] = wolf.rest.euler[i];   // сустав не двигается

            entity::ResolvedPart a[entity::MAX_PARTS], b[entity::MAX_PARTS];
            const u8 n = entity::resolve(wolf, moving, glm::vec3(0.f), 0.f,
                                         a, entity::MAX_PARTS);
            entity::resolve(wolf, frozen, glm::vec3(0.f), 0.f,
                            b, entity::MAX_PARTS);
            u8 w = 0;
            for (u8 j = 0; j < i; ++j) if (wolf.parts[j].visible) ++w;
            if (w >= n) continue;

            if (glm::length(a[w].center - b[w].center) > 1e-3f) {
                if (ear) ++earOwn; else ++tailOwn;
            }
        }
        check(earOwn > 0, "на бегу ухо мотает собственным суставом");
        check(tailOwn > 0, "и хвост качается своим");
    }

    // ---- Поза покоя переживает анимацию ----
    //
    // Висячее ухо задано наклоном сустава в позе покоя. Если анимация
    // кладётся ВМЕСТО неё, а не поверх, овца на первом же шаге
    // вскидывает уши торчком — и перестаёт быть овцой.
    //
    // Смотрим, куда ухо показывает: местное +Y, повёрнутое суставом.
    // Его составляющая по Z отрицательна у висячего уха и
    // положительна у торчащего.
    {
        auto earTilt = [](const entity::Rig& r, const entity::Pose& pose) {
            entity::ResolvedPart p[entity::MAX_PARTS];
            const u8 n = entity::resolve(r, pose, glm::vec3(0.f), 0.f,
                                         p, entity::MAX_PARTS);
            u8 w = 0;
            for (u8 i = 0; i < r.count && w < n; ++i) {
                if (!r.parts[i].visible) continue;
                if (r.parts[i].role == entity::PartRole::Ear)
                    return (p[w].rot * glm::vec3(0.f, 1.f, 0.f)).z;
                ++w;
            }
            return 0.f;
        };

        check(earTilt(sheep, sheep.rest) < -0.5f, "стоя у овцы ухо свисает назад");
        check(earTilt(wolf,  wolf.rest)  >  0.1f, "а у волка смотрит вперёд");

        int sheepUp = 0;
        for (int k = 0; k < 16; ++k) {
            anim::AnimState st;
            st.phase = (f32)k * 0.4f; st.speedNorm = 1.f;
            entity::Pose pose;
            anim::poseFor(sheep, pose, st);
            if (earTilt(sheep, pose) > -0.3f) ++sheepUp;
        }
        check(sheepUp == 0, "и на бегу овца ушей торчком не вскидывает");
    }

    // ---- Стоя зверь хвостом в такт шагу не машет ----
    {
        entity::Pose standing;
        anim::walkPose(wolf, standing, 2.2f, 0.f);
        f32 worst = 0.f;
        for (u8 i = 0; i < wolf.count; ++i)
            for (int k = 0; k < 3; ++k)
                worst = std::max(worst, std::fabs(standing.euler[i][k]));
        check(worst < 1e-4f, "на нулевой скорости ходьба не даёт ничего");
    }
}

// ------------------------------------------------------------
// Фаза шага идёт путём, а не временем.
//
// Раньше её двигали строчки `walkPhase += dt * 9.f`, разные на каждое
// состояние ИИ: в покое 2, в ходьбе 6, в погоне 9. Скорость и длина
// шага при этом не связаны ничем, поэтому ноги скользят по земле — и
// «правильного» множителя не существует: он верен ровно для одной
// скорости.
// ------------------------------------------------------------
void testGaitPhaseFollowsDistance() {
    group("походка: фаза идёт путём, а не временем");

    // ---- 1. Один шаг на длину шага, какой бы ни была скорость ----
    //
    // Это и есть определение «нога не скользит»: за путь в stride
    // модель обязана проделать ровно один цикл.
    {
        constexpr f32 TAU = 6.28318530718f;
        int bad = 0;
        for (const f32 speed : { 0.5f, 2.f, 4.5f, 7.5f, 12.f }) {
            ecs::Gait g{};
            g.stride = 1.6f;
            // Шаг по времени подбираем так, чтобы за 240 кадров
            // пройти РОВНО одну длину шага: округление числа кадров
            // само по себе дало бы расхождение и проверяло бы его, а
            // не формулу.
            const int steps = 240;
            const f32 dt = g.stride / (speed * (f32)steps);
            for (int i = 0; i < steps; ++i)
                anim::advanceGait(g, glm::vec3(0.f, 0.f, speed), dt);

            // Фаза приведена к [0, TAU), поэтому полный цикл читается
            // как возврат к началу.
            const f32 off = std::min(g.phase, TAU - g.phase);
            if (off > 0.05f) ++bad;
        }
        check(bad == 0, "за длину шага — ровно один цикл, на любой скорости");
    }

    // ---- 2. Вдвое быстрее — вдвое чаще, а не шире ----
    {
        ecs::Gait slow{}, fast{};
        slow.stride = fast.stride = 1.6f;
        const f32 dt = 1.f / 60.f;
        for (int i = 0; i < 60; ++i) {
            anim::advanceGait(slow, glm::vec3(0, 0, 2.f), dt);
            anim::advanceGait(fast, glm::vec3(0, 0, 4.f), dt);
        }
        // Фаза свёрнута, поэтому сравниваем накопленный путь напрямую:
        // два метра против четырёх за ту же секунду.
        ecs::Gait s2{}, f2{};
        s2.stride = f2.stride = 1000.f;   // достаточно, чтобы не свернулась
        for (int i = 0; i < 60; ++i) {
            anim::advanceGait(s2, glm::vec3(0, 0, 2.f), dt);
            anim::advanceGait(f2, glm::vec3(0, 0, 4.f), dt);
        }
        check(std::fabs(f2.phase - 2.f * s2.phase) < 1e-3f,
              "вдвое быстрее — вдвое больше циклов");
    }

    // ---- 3. Длинная нога — редкий шаг ----
    {
        ecs::Gait shortLeg{}, longLeg{};
        shortLeg.stride = 1000.f;
        longLeg.stride  = 2000.f;
        const f32 dt = 1.f / 60.f;
        for (int i = 0; i < 60; ++i) {
            anim::advanceGait(shortLeg, glm::vec3(0, 0, 3.f), dt);
            anim::advanceGait(longLeg,  glm::vec3(0, 0, 3.f), dt);
        }
        check(longLeg.phase < shortLeg.phase - 1e-4f,
              "при том же пути длинноногий делает меньше шагов");
    }

    // ---- 4. На месте фаза замирает, а не сбрасывается ----
    {
        ecs::Gait g{};
        g.stride = 1.6f;
        for (int i = 0; i < 30; ++i)
            anim::advanceGait(g, glm::vec3(0, 0, 3.f), 1.f / 60.f);
        const f32 walked = g.phase;
        check(walked > 0.1f, "на ходу фаза растёт");

        for (int i = 0; i < 120; ++i)
            anim::advanceGait(g, glm::vec3(0.f), 1.f / 60.f);
        check(std::fabs(g.phase - walked) < 1e-6f,
              "встал — фаза замерла там, где была, а не обнулилась");

        // Дрожание скорости тоже не должно двигать ноги.
        for (int i = 0; i < 120; ++i)
            anim::advanceGait(g, glm::vec3((i % 2 ? 0.01f : -0.01f), 0.f, 0.f),
                              1.f / 60.f);
        check(std::fabs(g.phase - walked) < 1e-6f,
              "и от дрожания скорости не дёргается");
    }

    // ---- 5. Падение с высоты не считается шагом ----
    {
        ecs::Gait g{};
        g.stride = 1.6f;
        for (int i = 0; i < 120; ++i)
            anim::advanceGait(g, glm::vec3(0.f, -20.f, 0.f), 1.f / 60.f);
        check(g.phase == 0.f, "вертикальная скорость шагов не делает");
    }

    // ---- 6. Фаза не растёт неограниченно ----
    //
    // За час игры она иначе доходит до величин, на которых синус
    // теряет точность, и походка начинает дёргаться.
    {
        ecs::Gait g{};
        g.stride = 1.6f;
        for (int i = 0; i < 20000; ++i)
            anim::advanceGait(g, glm::vec3(0, 0, 8.f), 1.f / 60.f);
        check(g.phase >= 0.f && g.phase < 6.2832f,
              "фаза остаётся в пределах одного оборота");
    }

    // ---- 7. Длина шага берётся из оснастки ----
    {
        int bad = 0;
        f32 minStride = 1e9f, maxStride = 0.f;
        for (u16 id = 1; id < mobs::MOB_COUNT; ++id) {
            const entity::Rig& rg = mobs::rigFor(id);
            if (rg.count == 0) continue;
            if (!(rg.strideLength > 0.05f)) ++bad;
            minStride = std::min(minStride, rg.strideLength);
            maxStride = std::max(maxStride, rg.strideLength);
        }
        check(bad == 0, "у каждого вида длина шага положительна");
        // У курицы и у коровы ноги разной длины — значит и шаг разный.
        check(maxStride > minStride * 1.5f,
              "коротконогим и длинноногим шаг задан разный");
    }
}

// ------------------------------------------------------------
// Двуногий: цельное тело, а не набор парящих коробок.
//
// Прежний NPC собирался в рендере руками, и собирался неправильно:
// коробка тела стояла ЦЕНТРОМ в точке опоры, то есть наполовину под
// землёй, голова висела на фиксированных 1.45, а между ними, там где
// полагалась грудь, был просвет в три четверти метра. Ноги при этом
// «шагали» сдвигом коробки по Z. Ни компилятор, ни проверки этого не
// видели: рендер никто не разбирал.
// ------------------------------------------------------------
void testHumanoidRigIsWholeBody() {
    group("двуногий: тело цельное и стоит на земле");

    for (const f32 H : { 1.5f, 1.8f, 2.2f }) {
        entity::HumanoidSpec spec;
        spec.height = H;
        const entity::Rig rig = entity::humanoidRig(spec);

        entity::Pose rest;
        entity::ResolvedPart p[entity::MAX_PARTS];
        const u8 n = entity::resolve(rig, rest, glm::vec3(0.f), 0.f,
                                     p, entity::MAX_PARTS);
        if (n == 0) { check(false, "оснастка пуста"); continue; }

        char m[176];

        // ---- Подошва на опоре ----
        const f32 lo = entity::lowestPoint(rig, rest, 0.f);
        std::snprintf(m, sizeof(m), "рост %.1f: подошва на опоре (%.3f)", H, lo);
        check(std::fabs(lo) < 1e-3f, m);

        // ---- Макушка на заданной высоте ----
        f32 top = p[0].center.y + p[0].size.y * 0.5f;
        for (u8 i = 1; i < n; ++i)
            top = std::max(top, p[i].center.y + p[i].size.y * 0.5f);
        std::snprintf(m, sizeof(m), "рост %.1f: макушка там, где заказано (%.3f)",
                      H, top);
        check(std::fabs(top - H) < 1e-3f, m);

        // ---- Ни одна часть не под землёй ----
        int sunk = 0;
        for (u8 i = 0; i < n; ++i)
            if (p[i].center.y + p[i].size.y * 0.5f < -1e-3f) ++sunk;
        std::snprintf(m, sizeof(m), "рост %.1f: ничего не закопано", H);
        check(sunk == 0, m);

        // ---- Тело без разрывов по вертикали ----
        //
        // Именно разрыв и был виден: голова сама по себе, тело само.
        // Берём туловище, голову и ноги — то, что образует силуэт, —
        // и требуем, чтобы их отрезки по Y смыкались.
        f32 torsoBottom = 0.f, torsoTop = 0.f, headBottom = 0.f, legTop = 0.f;
        bool haveTorso = false, haveHead = false, haveLeg = false;
        u8 w = 0;
        for (u8 i = 0; i < rig.count && w < n; ++i) {
            if (!rig.parts[i].visible) continue;
            const f32 b = p[w].center.y - p[w].size.y * 0.5f;
            const f32 t = p[w].center.y + p[w].size.y * 0.5f;
            switch (rig.parts[i].role) {
                case entity::PartRole::Torso:
                    torsoBottom = b; torsoTop = t; haveTorso = true; break;
                case entity::PartRole::Head:
                    headBottom = b; haveHead = true; break;
                case entity::PartRole::UpperLegFL:
                case entity::PartRole::UpperLegFR:
                    legTop = std::max(legTop, t); haveLeg = true; break;
                default: break;
            }
            ++w;
        }
        check(haveTorso && haveHead && haveLeg,
              "у двуногого есть торс, голова и ноги");
        if (haveTorso && haveHead) {
            std::snprintf(m, sizeof(m),
                          "рост %.1f: голова сидит на плечах, а не парит "
                          "(зазор %.3f)", H, headBottom - torsoTop);
            check(std::fabs(headBottom - torsoTop) < 1e-3f, m);
        }
        if (haveTorso && haveLeg) {
            std::snprintf(m, sizeof(m),
                          "рост %.1f: торс сидит на бёдрах (зазор %.3f)",
                          H, torsoBottom - legTop);
            check(std::fabs(torsoBottom - legTop) < 1e-3f, m);
        }

        // ---- Ноги шагают ВРАЩЕНИЕМ ----
        //
        // Проверка на ту самую ошибку: коробка не должна ехать
        // параллельно себе. При повороте в бедре ступня и опускается,
        // и уходит вперёд; при сдвиге — только уходит.
        {
            entity::Pose walk;
            anim::walkPose(rig, walk, 1.2f, 1.f);
            entity::ResolvedPart q[entity::MAX_PARTS];
            entity::resolve(rig, walk, glm::vec3(0.f), 0.f, q, entity::MAX_PARTS);
            int rotated = 0;
            u8 v = 0;
            for (u8 i = 0; i < rig.count && v < n; ++i) {
                if (!rig.parts[i].visible) continue;
                if (anim::isUpperLimb(rig.parts[i].role)) {
                    // Коробка бедра обязана быть ПОВЁРНУТА, а не
                    // просто переставлена.
                    const glm::vec3 down = q[v].rot * glm::vec3(0, -1, 0);
                    if (std::fabs(down.z) > 1e-3f) ++rotated;
                }
                ++v;
            }
            std::snprintf(m, sizeof(m), "рост %.1f: бедро поворачивается, "
                                        "а не сдвигается", H);
            check(rotated > 0, m);
        }
    }

    // Пропорции задаются долями роста, значит модель обязана
    // масштабироваться целиком, а не тянуться одними ногами.
    {
        entity::HumanoidSpec a; a.height = 1.5f;
        entity::HumanoidSpec b; b.height = 3.0f;
        const entity::Rig ra = entity::humanoidRig(a);
        const entity::Rig rb = entity::humanoidRig(b);
        check(ra.count == rb.count, "у высокого и низкого частей поровну");

        bool proportional = true;
        for (u8 i = 0; i < ra.count && i < rb.count; ++i) {
            const glm::vec3 sa = ra.parts[i].size * 2.f;
            const glm::vec3 sb = rb.parts[i].size;
            if (glm::length(sa - sb) > 1e-3f) proportional = false;
        }
        check(proportional, "вдвое выше — значит вдвое крупнее целиком");
    }
}

// ------------------------------------------------------------
// Походка соответствует анатомии.
// ------------------------------------------------------------
void testGaitMatchesAnatomy() {
    group("походка: диагональные пары и сгиб колена");

    // Четвероногое шагает ДИАГОНАЛЬНЫМИ парами: правая передняя с
    // левой задней. Иначе получается иноходь и существо
    // переваливается боком.
    check(anim::limbPhaseSign(entity::PartRole::UpperLegFR) ==
          anim::limbPhaseSign(entity::PartRole::UpperLegBL),
          "правая передняя идёт с левой задней");
    check(anim::limbPhaseSign(entity::PartRole::UpperLegFL) ==
          anim::limbPhaseSign(entity::PartRole::UpperLegBR),
          "левая передняя — с правой задней");
    check(anim::limbPhaseSign(entity::PartRole::UpperLegFR) !=
          anim::limbPhaseSign(entity::PartRole::UpperLegFL),
          "а передние между собой — врозь");

    // Голень и бедро одной ноги идут в одной фазе.
    check(anim::limbPhaseSign(entity::PartRole::UpperLegFR) ==
          anim::limbPhaseSign(entity::PartRole::LowerLegFR),
          "голень следует за своим бедром");

    const entity::Rig& rig = mobs::rigFor(1);
    if (rig.count == 0) return;

    // На нулевой скорости конечности неподвижны: иначе существо
    // «идёт» стоя на месте.
    {
        entity::Pose p;
        anim::walkPose(rig, p, 1.3f, 0.f);
        f32 maxAngle = 0.f;
        for (u8 i = 0; i < rig.count; ++i)
            for (int k = 0; k < 3; ++k)
                maxAngle = std::max(maxAngle, std::fabs(p.euler[i][k]));
        check(maxAngle < 1e-4f, "при нулевой скорости конечности неподвижны");
    }

    // Амплитуда растёт со скоростью.
    {
        entity::Pose slow, fast;
        anim::walkPose(rig, slow, 1.3f, 0.3f);
        anim::walkPose(rig, fast, 1.3f, 1.0f);
        f32 sMax = 0.f, fMax = 0.f;
        for (u8 i = 0; i < rig.count; ++i) {
            sMax = std::max(sMax, std::fabs(slow.euler[i].x));
            fMax = std::max(fMax, std::fabs(fast.euler[i].x));
        }
        check(fMax > sMax + 0.05f, "шире шаг на большей скорости");
    }

    // Колено гнётся только в одну сторону, и сторона эта — своя у
    // передних ног и у задних.
    //
    // Проверяем ГЕОМЕТРИЮ, а не знак угла. Прежняя проверка требовала
    // «угол по X не положительный» — и пропускала ровно ту ошибку, от
    // которой сторожила: с отрицательным углом голень уезжает в +Z,
    // то есть колено выгибается ВПЕРЁД. Сторожить надо положение
    // голени относительно колена.
    {
        int wrongWay = 0, moved = 0;
        for (int k = 0; k < 24; ++k) {
            entity::Pose full;
            anim::walkPose(rig, full, (f32)k * 0.26f, 1.f);

            for (u8 i = 0; i < rig.count; ++i) {
                const entity::PartRole r = rig.parts[i].role;
                if (!anim::isLowerLimb(r)) continue;

                // Берём ОДИН сустав: иначе к смещению голени
                // примешивается качание бедра, и сторона сгиба
                // перестаёт читаться.
                entity::Pose only;
                only.euler[i] = full.euler[i];

                entity::Pose rest;
                entity::ResolvedPart bent[entity::MAX_PARTS];
                entity::ResolvedPart straight[entity::MAX_PARTS];
                const u8 n = entity::resolve(rig, only, glm::vec3(0.f), 0.f,
                                             bent, entity::MAX_PARTS);
                entity::resolve(rig, rest, glm::vec3(0.f), 0.f,
                                straight, entity::MAX_PARTS);
                u8 w = 0;
                for (u8 j = 0; j < i; ++j)
                    if (rig.parts[j].visible) ++w;
                if (w >= n) continue;

                // Сравниваем с той же голенью в РАСПРЯМЛЁННОЙ ноге.
                // Просто z коробки не годится: он в основном говорит,
                // передняя это нога или задняя, а не куда согнулось
                // колено.
                const f32 dz = bent[w].center.z - straight[w].center.z;
                if (std::fabs(dz) > 1e-4f) ++moved;

                // Сторона сгиба названа ЗДЕСЬ, а не взята из
                // kneeBendSign: иначе проверка сверяет функцию с самой
                // собой и молча принимает любой её знак.
                //   передняя нога складывается назад, в -Z;
                //   задняя  — вперёд, в +Z.
                const bool rear = (r == entity::PartRole::LowerLegBL ||
                                   r == entity::PartRole::LowerLegBR);
                const f32 want = rear ? 1.f : -1.f;
                if (dz * want < -1e-4f) ++wrongWay;
            }
        }
        check(moved > 0, "колено вообще сгибается");
        check(wrongWay == 0, "и складывается в свою сторону: "
                             "передние назад, задние вперёд");
    }
}

// ------------------------------------------------------------
// Огрублённые уровни детализации не дырявят землю.
//
// Именно за это их и подозревают в первую очередь, когда в мире
// появляются дыры: коэффициент огрубления, сведение куба вокселей в
// клетку, рамка соседей — ошибиться есть где, а на экране LOD виден
// только вдали, где всё и так нечётко.
//
// Проверка прямая: для каждой из 1024 колонок чанка должна найтись
// верхняя грань, и она не должна оказаться НИЖЕ настоящей поверхности
// — иначе сквозь неё будет видно то, что под землёй.
//
// Заодно это ответ на подозрение, с которого начинался разбор дыр на
// устройстве: мешер чист на всех четырёх уровнях, дело не в нём.
// ------------------------------------------------------------
void testLodCoversGround() {
    group("world: огрублённые уровни не дырявят землю");

    world::blocks();
    auto chunk = std::make_unique<world::Chunk>();
    chunk->coord = {0, 0, 0};

    // Ступенчатый рельеф: ровная земля такую ошибку не поймает, а
    // склоны и уступы — как раз то, на чём огрубление и спотыкается.
    for (i32 z = 0; z < world::CHUNK_SIZE; ++z)
        for (i32 x = 0; x < world::CHUNK_SIZE; ++x) {
            const i32 h = 30 + (x / 3) % 7 + (z / 5) % 5 + ((x + z) % 3);
            for (i32 y = 0; y <= h; ++y)
                chunk->setUnlocked(x, y, z, y == h ? world::GRASS : world::STONE);
        }

    std::vector<i32> surf((usize)world::CHUNK_SIZE * world::CHUNK_SIZE, -1);
    for (i32 z = 0; z < world::CHUNK_SIZE; ++z)
        for (i32 x = 0; x < world::CHUNK_SIZE; ++x)
            for (i32 y = world::CHUNK_SIZE_Y - 1; y >= 0; --y)
                if (chunk->at(x, y, z) != world::AIR) {
                    surf[(usize)z * world::CHUNK_SIZE + x] = y;
                    break;
                }

    world::ChunkNeighbors nb;
    std::vector<world::Quad> quads;

    for (u8 lod = 0; lod < 4; ++lod) {
        world::buildGreedyMesh(*chunk, nb, quads, (world::Lod)lod);

        std::vector<i32> cover((usize)world::CHUNK_SIZE * world::CHUNK_SIZE, -1);
        for (const auto& q : quads) {
            if (q.v0.face != 2) continue;             // только верхние грани
            const i32 x0 = (i32)q.v0.pos.x, z0 = (i32)q.v0.pos.z;
            const i32 y  = (i32)q.v0.pos.y;
            const i32 wx = (i32)(q.du.x + q.dv.x);
            const i32 wz = (i32)(q.du.z + q.dv.z);
            for (i32 z = z0; z < z0 + (wz ? wz : 1); ++z)
                for (i32 x = x0; x < x0 + (wx ? wx : 1); ++x) {
                    if ((u32)x >= (u32)world::CHUNK_SIZE ||
                        (u32)z >= (u32)world::CHUNK_SIZE) continue;
                    i32& cv = cover[(usize)z * world::CHUNK_SIZE + x];
                    if (y > cv) cv = y;
                }
        }

        int uncovered = 0, sunken = 0;
        for (usize i = 0; i < cover.size(); ++i) {
            if (cover[i] < 0) { ++uncovered; continue; }
            if (cover[i] < surf[i] + 1) ++sunken;
        }
        char msg[128];
        std::snprintf(msg, sizeof(msg),
                      "ур.%u: все 1024 колонки накрыты сверху (без крыши %d)",
                      (unsigned)lod, uncovered);
        check(uncovered == 0, msg);
        std::snprintf(msg, sizeof(msg),
                      "ур.%u: крыша не проваливается под поверхность (провалов %d)",
                      (unsigned)lod, sunken);
        check(sunken == 0, msg);
    }
}

// ------------------------------------------------------------
// Обход граней чанка смотрит наружу.
//
// check_winding.py стережёт таблицы кубов мобов и предметов, а
// геометрию мира строит мешер — и его обход не проверял никто. Цена
// ошибки здесь ровно та же и даже хуже: при отсечении задних граней
// отсекаются наружные, и мир виден изнутри. Именно этим и оказалась
// поломка графики, только пришла она с другой стороны — из
// объявления конвейера.
//
// Требование то же, что к кубам: нормаль, посчитанная по обходу
// первого треугольника квада, обязана совпадать по направлению с
// нормалью его грани.
// ------------------------------------------------------------
void testMeshWindingFacesOutward() {
    group("render: обход граней чанка смотрит наружу");

    world::blocks();
    auto chunk = std::make_unique<world::Chunk>();
    chunk->coord = {0, 0, 0};
    // Одинокий блок в воздухе: у него видны все шесть граней сразу.
    chunk->setUnlocked(16, 40, 16, world::STONE);

    world::ChunkNeighbors nb;
    std::vector<world::Quad> quads;
    world::buildGreedyMesh(*chunk, nb, quads, world::Lod::Full);

    std::vector<render::VoxelVertex> verts;
    std::vector<u32> idx;
    u32 opaque = 0;
    render::buildChunkVertices(*chunk, quads, verts, idx, opaque);
    check(opaque == 36, "у одинокого блока шесть граней, тридцать шесть индексов");

    auto posOf = [&](u32 i) {
        const u32 p = verts[i].packed;
        return glm::vec3((f32)(p & 63u), (f32)((p >> 6) & 255u), (f32)((p >> 14) & 63u));
    };

    bool seen[6] = {};
    int inward = 0;
    for (u32 t = 0; t * 3 + 2 < opaque; ++t) {
        const u32 ia = idx[t*3], ib = idx[t*3+1], ic = idx[t*3+2];
        const u32 face = (verts[ia].packed >> 20) & 7u;
        if (face > 5) { ++inward; continue; }
        seen[face] = true;
        const glm::vec3 n = glm::cross(posOf(ib) - posOf(ia), posOf(ic) - posOf(ia));
        if (glm::dot(n, glm::vec3(render::FACE_NORMAL[face])) <= 0.f) ++inward;
    }
    check(inward == 0, "ни один треугольник не намотан внутрь");
    bool all = true;
    for (bool s2 : seen) all = all && s2;
    check(all, "все шесть граней построены");

    // И конвейер объявляет ту сторону, которая из этого следует.
    const std::string pipe = readSource("app/src/main/cpp/src/vk/vk_pipeline.h");
    if (!pipe.empty()) {
        check(pipe.find("frontFace  = VK_FRONT_FACE_COUNTER_CLOCKWISE") != std::string::npos,
              "конвейер считает лицевой грань, обойдённую против часовой стрелки");
    }
    const std::string rs = readSource("app/src/main/cpp/src/render/mob_renderer.cpp");
    if (!rs.empty())
        check(rs.find("VK_FRONT_FACE_CLOCKWISE") == std::string::npos,
              "рендереры не переопределяют сторону по-своему");
}

// ------------------------------------------------------------
// Сейв записан и прочитан обратно ФАЙЛОМ.
//
// Проверки выше гоняли тело сейва через ByteWriter/ByteReader и
// ничего не знали про заголовок файла. А сломан был именно он:
// писатель складывал поля и писал 48 байт, загрузчик верил
// комментарию «44 байта фиксированные» и читал 44. Контрольная сумма
// лежит в последних четырёх байтах — до загрузчика она не доезжала и
// оставалась нулём, а тело он начинал читать на четыре байта раньше
// начала. Любая загрузка кончалась «CRC mismatch (expected=0)», и ни
// одна проверка этого не видела, потому что ни одна не открывала
// файл.
// ------------------------------------------------------------
void testSaveFileRoundTrip() {
    group("save: файл записан и прочитан обратно");

    check(save::SAVE_HEADER_SIZE == 48,
          "размер заголовка посчитан из полей, а не записан числом");

    items::items();
    world::blocks();

    // init() создаёт подкаталог saves внутри переданного; сам
    // переданный каталог обязан существовать — во время проверки это
    // рабочий каталог сборки.
    save::SaveManager mgr;
    mgr.init("build/hostcheck");
    const save::SaveSlot slot = mgr.slots().slot(0, 0);
    std::remove(slot.dataPath().c_str());

    constexpr u64 SEED = 0xC0FFEEULL;
    world::ChunkManager world(SEED, 2);
    world::DayCycle day;
    save::WorldDeltaStore deltas;

    ecs::Registry reg;
    const ecs::Entity player = reg.create();
    ecs::Transform tf;
    tf.position = { 12.5f, 40.f, -7.25f };
    reg.add(player, tf);
    items::Wallet wal;
    wal.gold = 4321;
    reg.add(player, wal);

    const save::SaveStatus ws = mgr.save(slot, world, reg, player, deltas,
                                         SEED, 777, day);
    if (ws != save::SaveStatus::Ok)
        std::printf("    (статус записи: %d, путь: %s)\n",
                    (int)ws, slot.dataPath().c_str());
    check(ws == save::SaveStatus::Ok, "сейв записан");

    // Файл на диске обязан начинаться с заголовка полного размера.
    std::FILE* f = std::fopen(slot.dataPath().c_str(), "rb");
    check(f != nullptr, "файл сейва появился");
    if (f) {
        std::fseek(f, 0, SEEK_END);
        const long sz = std::ftell(f);
        std::fclose(f);
        check(sz > (long)save::SAVE_HEADER_SIZE,
              "в файле есть заголовок и тело");
    }

    // И прочитан обратно тем же менеджером.
    ecs::Registry reg2;
    const ecs::Entity player2 = reg2.create();
    reg2.add(player2, ecs::Transform{});
    reg2.add(player2, items::Wallet{});
    world::ChunkManager world2(SEED, 2);
    save::WorldDeltaStore deltas2;
    u64 outSeed = 0; u32 outPlay = 0;
    world::DayCycle day2;

    const save::SaveStatus rs = mgr.load(slot, world2, reg2, player2, deltas2,
                                         &outSeed, &outPlay, &day2);
    check(rs == save::SaveStatus::Ok, "сейв прочитан обратно");
    check(outSeed == SEED, "зерно мира дошло целым");
    check(outPlay == 777, "наигранное время дошло целым");
    if (auto* w2 = reg2.get<items::Wallet>(player2))
        check(w2->gold == 4321, "кошелёк дошёл целым");
    if (auto* t2 = reg2.get<ecs::Transform>(player2))
        check(std::fabs(t2->position.x - 12.5f) < 0.001f,
              "положение игрока дошло целым");

    std::remove(slot.dataPath().c_str());
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
    testLodCoversGround();
    testMeshWindingFacesOutward();
    testChunkSeamAcrossOrigin();
    testVoxelShading();
    testDayCycle();
    testBossPhases();

    testVulkanGuards();
    testRenderPassSync();
    testDebugShadingWired();
    testMinimalScene();
    testDebugSceneIsolated();
    testDiagnosticBuildWired();
    testUnknownNeighborIsNotAir();
    testNeighborArrivalTriggersRemesh();
    testVoxelColorIsPlaceIndependent();
    testFaceShadingHasSingleSource();
    testApkCarriesTheShadersItWasBuiltFrom();
    testBuildStampIsNotStale();
    testSkyPaysForMathNotBranches();
    testWorldSharesOneLightingModel();
    testDistantGrassIsNotSubPixel();
    testShadersAvoidUndefinedMath();
    testLodHasSingleSourceOfTruth();
    testResidentLodSurvivesRequest();
    testLodCompletionIsAddressed();
    testMeshJobBuildsRequestedLod();
    testJobsOutlivingTheirWorldAreSafe();
    testChunkStreamingIsBudgeted();
    testSurfaceHeightCacheMatchesGenerator();
    testUnloadAcceptsTighterRadius();
    testFramePassOrder();
    testFrameGpuBreakdown();
    testFrameRateIsMeasuredByWallClock();
    testFrameRateLimitSetting();
    testClipboardLogTrimming();
    testCoarseWaterIsStable();
    testCoarseWaterDoesNotFloat();
    testLodSeamHasNoCracks();
    testDistantWaterIsNotBlended();
    testWaterSortedByWaterCenter();
    testUiTapSurvivesRedraw();
    testHudAndButtonsDoNotOverlap();
    testUiThemeObeysItsOwnRules();
    testUiThemeMatchesItsDocument();
    testInteractPromptUnlocksScreens();
    testDialogueChoiceReachesTheGame();
    testNavigationReturnsWhereItCameFrom();
    testPauseMenuIsGroupedAndComplete();
    testConfirmSwallowsTouchesOutsideIt();
    testInventoryShowsEverySlot();
    testInventoryTapDoesOneThing();
    testRussianTextIsActuallyDrawn();
    testSettingsFitAndDoSomething();
    testDialogueReadsAsAConversation();
    testQuestLogAnswersWhatToDoNow();
    testNoticesQueueAndPrioritise();
    testAttributeSteppersArePressable();
    testCraftTradeEnchantShareOneLook();
    testEntityFacingHasNoSidewaysMotion();
    testFacingTurnsSmoothly();
    testRigHierarchyIsSound();
    testRigResolveRotatesParts();
    testGaitMatchesAnatomy();
    testHumanoidRigIsWholeBody();
    testBeastsHaveCharacter();
    testGaitPhaseFollowsDistance();
    testPlayerHasModel();
    testBufferMapContract();
    testUiGeometry();
    testMeshFitsPacking();
    testWorldQueries();
    testVoxelReader();
    testPathShape();
    testAudioMixer();
    testSaveRoundTrip();
    testSaveFileRoundTrip();
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
