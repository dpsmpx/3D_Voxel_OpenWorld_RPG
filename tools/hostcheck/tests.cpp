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
#include "items/item_pickup.h"
#include "items/throwable.h"
#include "combat/components.h"
#include "combat/projectile.h"
#include "trade/trade.h"
#include "ecs/components.h"
#include "npc/dialogue.h"
#include "npc/npc_ai.h"
#include "save/save_player.h"
#include "save/save_npc.h"
#include "progression/skill_tree.h"
#include "progression/progression.h"
#include "crafting/crafting.h"
#include "crafting/recipe.h"
#include "items/item_use.h"
#include "combat/resonance.h"
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
#include "render/mob_renderer.h"
#include "physics/character_controller.h"
#include "input/touch.h"
#include <android/input.h>
#include "vk/vk_buffer.h"
#include "vk/vk_texture.h"
#include "world/noise.h"
#include "world/terrain.h"
#include "world/day_cycle.h"
#include "mobs/mob_ai.h"
#include "mobs/mob_def.h"
#include "save/save_manager.h"
#include "ui/ui_context.h"
#include "ui/slider.h"
#include "world/features.h"
#include "world/ai/pathfinding.h"
#include "core/job_system.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cctype>
#include <set>
#include <dirent.h>
#include "ui/hud_layout.h"
#include "config/localization.h"
#include "core/orientation.h"
#include "entity/locomotion.h"
#include "entity/mob_rigs.h"
#include "entity/humanoid_rig.h"
#include "entity/showcase.h"
#include "npc/npc_rig.h"
#include "npc/npc_spawner.h"
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

/// Читает файл проекта целиком. Определение ниже по тексту:
/// часть проверок пользуется им раньше.
static std::string readSource(const char* path);

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
    u32 n = world::buildGreedyMesh(*chunk, nb, quads);
    check(n == 0 && quads.empty(), "пустой чанк не даёт геометрии");

    // Плоский слой камня: верх и низ должны слиться в один квад каждый,
    // плюс четыре боковых стенки.
    fillFlat(*chunk, 1, world::STONE);
    n = world::buildGreedyMesh(*chunk, nb, quads);
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
    n = world::buildGreedyMesh(*single, nb, quads);
    check(n == 6, "у одиночного блока ровно 6 граней");

    // Два соседних блока вдоль X. Внутренние грани отсекаются (10 из 12),
    // а оставшиеся четыре боковые сливаются в квады шириной 2 — итого 6.
    auto pair = std::make_unique<world::Chunk>();
    pair->voxels[world::chunkIndex(5, 5, 5)] = world::STONE;
    pair->voxels[world::chunkIndex(6, 5, 5)] = world::STONE;
    n = world::buildGreedyMesh(*pair, nb, quads);
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

    // Детерминированность: тот же чанк — тот же меш.
    fillFlat(*chunk, 8, world::STONE);
    std::vector<world::Quad> first, again;
    world::buildGreedyMesh(*chunk, nb, first);
    world::buildGreedyMesh(*chunk, nb, again);
    check(!first.empty(), "на сплошном слое геометрия есть");
    check(again.size() == first.size(), "меширование детерминировано");

    // Меш не рождает геометрию в пустоте: ни один квад не уходит
    // выше поверхности, ни один не висит ниже дна, а верх остаётся
    // сплошным.
    auto slab = std::make_unique<world::Chunk>();
    constexpr i32 TOP = 40;
    for (i32 x = 0; x < world::CHUNK_SIZE; ++x)
        for (i32 z = 0; z < world::CHUNK_SIZE; ++z)
            for (i32 y = 0; y < TOP; ++y)
                slab->voxels[world::chunkIndex(x, y, z)] = world::STONE;

    {
        std::vector<world::Quad> lq;
        world::buildGreedyMesh(*slab, nb, lq);

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
        check(inside, "геометрия не выходит за чанк");
        check(maxY <= (f32)TOP, "ничего не висит выше поверхности");
        check(topArea >= (f32)(world::CHUNK_SIZE * world::CHUNK_SIZE),
              "верхняя поверхность сплошная");
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
        world::buildGreedyMesh(*cell.c, nb, quads);

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
        world::buildGreedyMesh(*cells[0].c, lone, quads);
        usize wall = 0;
        for (const auto& q : quads)
            if (q.v0.face == 1 && q.v0.pos.x == 0.f && q.v0.pos.y < 20.f) ++wall;
        check(wall == 0, "по неизвестному соседу стена не строится");
    }

    // Свет на стыке считается по СОСЕДУ, а не по краю своего чанка.
    //
    // Открытость неба берётся из карты верхних непрозрачных клеток, и
    // у этой карты есть рамка в одну клетку шириной — ровно затем,
    // чтобы угол грани на самой границе чанка видел, что стоит по ту
    // сторону. Без рамки стена соседа для нас не существует, её тень
    // не ложится, и по краю каждого чанка идёт светлая кайма в один
    // блок — сетка из швов по всему миру.
    //
    // Проверяется сравнением: одна и та же геометрия, собранная
    // внутри чанка и через границу, обязана дать один и тот же свет.
    {
        constexpr i32 N   = world::CHUNK_SIZE;
        constexpr i32 GND = 20;
        constexpr i32 TOP = 40;

        // Наименьшая открытость неба среди верхних граней, накрывающих
        // колонку: именно её и съедает тень стены.
        auto skyAtColumn = [](const std::vector<world::Quad>& qs,
                              i32 wantX, i32 wantZ) -> int {
            int lowest = 8;
            for (const auto& q : qs) {
                if (q.v0.face != 2) continue;
                const i32 x0 = (i32)q.v0.pos.x, z0 = (i32)q.v0.pos.z;
                const i32 wx = (i32)(q.du.x + q.dv.x);
                const i32 wz = (i32)(q.du.z + q.dv.z);
                if (wantX < x0 || wantX >= x0 + (wx ? wx : 1)) continue;
                if (wantZ < z0 || wantZ >= z0 + (wz ? wz : 1)) continue;
                for (u8 v : q.sky) if ((int)(v & 7) < lowest) lowest = (int)(v & 7);
            }
            return lowest;
        };

        // Внутри одного чанка: стена на x = 0, смотрим колонку x = 1.
        auto inner = std::make_unique<world::Chunk>();
        inner->coord = { 0, 0, 0 };
        fillFlat(*inner, GND, world::STONE);
        for (i32 z = 0; z < N; ++z)
            for (i32 y = GND; y < TOP; ++y)
                inner->setUnlocked(0, y, z, world::STONE);
        world::ChunkNeighbors none;
        world::buildGreedyMesh(*inner, none, quads);
        const int innerSky = skyAtColumn(quads, 1, N / 2);

        // Через границу: стена — последняя колонка соседа слева,
        // смотрим колонку x = 0 своего чанка. Геометрия та же.
        auto west = std::make_unique<world::Chunk>();
        west->coord = { -1, 0, 0 };
        fillFlat(*west, GND, world::STONE);
        for (i32 z = 0; z < N; ++z)
            for (i32 y = GND; y < TOP; ++y)
                west->setUnlocked(N - 1, y, z, world::STONE);
        auto east = std::make_unique<world::Chunk>();
        east->coord = { 0, 0, 0 };
        fillFlat(*east, GND, world::STONE);
        world::ChunkNeighbors seamNb;
        seamNb.nx = west.get();
        world::buildGreedyMesh(*east, seamNb, quads);
        const int seamSky = skyAtColumn(quads, 0, N / 2);

        char msg[160];
        std::snprintf(msg, sizeof(msg),
                      "стена внутри чанка и правда затеняет землю (небо %d из 7)",
                      innerSky);
        check(innerSky < 7, msg);
        std::snprintf(msg, sizeof(msg),
                      "стена соседа затеняет так же, как своя (%d против %d)",
                      seamSky, innerSky);
        check(seamSky == innerSky, msg);
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
    world::buildGreedyMesh(*flat, nb, quads);
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
    world::buildGreedyMesh(*step, nb, quads);
    bool anyShaded = false;
    for (const auto& q : quads)
        if (q.v0.face == 2)
            for (u8 a : q.ao) if (a < 3) anyShaded = true;
    check(anyShaded, "у стены верхняя грань темнеет в углах");

    // Разное затенение обязано разрывать слияние: иначе тень от стены
    // растеклась бы по всей плоскости одним квадом.
    usize flatTop = 0, stepTop = 0;
    world::buildGreedyMesh(*flat, nb, quads);
    for (const auto& q : quads) if (q.v0.face == 2) ++flatTop;
    world::buildGreedyMesh(*step, nb, quads);
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
    world::buildGreedyMesh(*step, nb, quads);
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
    world::buildGreedyMesh(*cave, nb, quads);
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
    world::buildGreedyMesh(*lake, nb, quads);
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

    // Босса ставит только updateBosses. Раньше это проверялось по полю
    // spawnWeight == 0 — но вес спавна не читал никто, а отсекал
    // боссов от обычного спавна отдельный `if (def.isBoss) continue;`.
    // Проверка сверяла поле, к которому спавнер не обращается.
    {
        const std::string src =
            readSource("app/src/main/cpp/src/mobs/spawner.cpp");
        check(!src.empty() && src.find("if (def.isBoss) continue;")
                              != std::string::npos,
              "обычный спавн пропускает боссов");
    }

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

/// Все .cpp и .h дерева исходников, кроме перечисленных.
///
/// Нужно там, где вопрос звучит «а ЗОВЁТ ли это хоть кто-нибудь».
/// Список файлов вручную для такого вопроса не годится: дыру как раз
/// и создаёт файл, который забыли внести в список.
static void collectSources(const std::string& dir,
                           const std::set<std::string>& skipNames,
                           std::string& out)
{
    DIR* d = ::opendir(dir.c_str());
    if (!d) return;
    while (struct dirent* e = ::readdir(d)) {
        const std::string name = e->d_name;
        if (name == "." || name == "..") continue;
        const std::string full = dir + "/" + name;
        DIR* sub = ::opendir(full.c_str());
        if (sub) { ::closedir(sub); collectSources(full, skipNames, out); continue; }
        const usize dot = name.rfind('.');
        if (dot == std::string::npos) continue;
        const std::string ext = name.substr(dot);
        if (ext != ".cpp" && ext != ".h") continue;
        if (skipNames.count(name)) continue;
        out += readSource(full.c_str());
        out += "\n";
    }
    ::closedir(d);
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
        world::buildGreedyMesh(*c, nb, quads);
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
        world::buildGreedyMesh(*c, nb, quads);
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
        world::buildGreedyMesh(*c, nb, quads);
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
        world::buildGreedyMesh(*c, nb, quads);
        check(facesTowardPlusX(quads) == 0,
              "стык без соседа пуст");
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
    check(after.find("enqueueMesh(ctx->coord)") != std::string::npos,
          "свой чанк ставится на меширование");
    usize n = 0;
    for (const char* d : { "coord.x - 1", "coord.x + 1", "coord.z - 1", "coord.z + 1" })
        if (after.find(d) != std::string::npos) ++n;
    check(n == 4, "и все четыре соседа — тоже");

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
    world::buildGreedyMesh(*c, none, quads);
    const usize without = quads.size();

    auto air = std::make_unique<world::Chunk>();
    air->coord = { 1, 0, 0 };
    world::ChunkNeighbors with;
    with.px = air.get();
    world::buildGreedyMesh(*c, with, quads);
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
// генератор. Иначе трава разъедется с рельефом.
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
// Очередь готовых мешей не теряет чанков
//
// Пока уровней детализации было четыре, рендер сам заказывал меш,
// которого у него нет: каждый кадр отбор смотрел, какой уровень нужен
// чанку, какой лежит в видеопамяти, и разницу заказывал заново. Это
// заодно было и страховкой. Если меш забрали из очереди, но выгрузить
// не смогли — не открылся пакет передачи, кончился staging-пул на
// пике загрузки мира, не создался буфер, — потеря исправлялась сама
// на следующем кадре, и никто про неё не знал.
//
// Уровней больше нет, и заказывать «недостающий уровень» рендеру
// нечего: о чанке он узнаёт ТОЛЬКО из очереди готовых мешей, и ровно
// один раз. Значит, забрать меш и не выгрузить — теперь навсегда: на
// его месте останется дыра в ландшафте до следующей правки блока по
// соседству, то есть, скорее всего, до конца сессии.
//
// Проверяется путь возврата: неудавшуюся выгрузку можно отдать
// обратно в очередь, и она приедет снова.
// ------------------------------------------------------------
void testMeshQueueLosesNothing() {
    group("мир: очередь мешей не теряет чанков");

    world::blocks();
    if (!jobs::gJobs.running()) jobs::gJobs.start(2);
    world::ChunkManager mgr(0x9E3779B9ULL, 2);

    // Забираем всё, что успевает построиться. Меширование идёт в
    // фоне, поэтому пустая очередь означает не «всё приехало», а
    // «ещё считают»: между пустыми выборками ждём.
    std::vector<world::MeshReady> first;
    int idle = 0;
    for (int spin = 0; spin < 4000 && idle < 50; ++spin) {
        mgr.update({ 0.f, 70.f, 0.f });
        auto batch = mgr.pollMeshesReady();
        if (batch.empty()) {
            ++idle;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            continue;
        }
        idle = 0;
        for (auto& m : batch) first.push_back(std::move(m));
    }
    check(!first.empty(), "меши доезжают до очереди");
    check(mgr.pollMeshesReady().empty(), "очередь исчерпана");
    if (first.empty()) return;

    // Один и тот же чанк мог приехать несколько раз: соседи
    // достраиваются и заказывают перестройку. Считаем чанки, а не
    // записи.
    std::vector<std::shared_ptr<world::Chunk>> uniq;
    for (auto& m : first) {
        bool seen = false;
        for (auto& u : uniq) if (u == m.chunk) { seen = true; break; }
        if (!seen) uniq.push_back(m.chunk);
    }

    usize withQuads = 0;
    for (auto& c : uniq) {
        std::lock_guard lk(c->meshMutex);
        if (c->mesh.built) ++withQuads;
    }
    check(withQuads > 0, "у забранных мешей есть квады");

    // Главное: меш, который не удалось выгрузить, возвращается в
    // очередь. Иначе на его месте останется дыра до конца сессии.
    for (auto& c : uniq) mgr.requeueMesh(c);
    auto again = mgr.pollMeshesReady();
    check(again.size() == withQuads,
          "возвращённые меши приезжают снова, все до одного");
    if (again.empty()) return;

    // Дважды вернуть один и тот же меш — это одна запись в очереди, а
    // не две: иначе рендер выгрузит его вторично и потратит на это
    // целый слот кадрового бюджета.
    for (auto& m : again) mgr.requeueMesh(m.chunk);
    for (auto& m : again) mgr.requeueMesh(m.chunk);
    auto third = mgr.pollMeshesReady();
    check(third.size() == again.size(), "повторный возврат не двоит очередь");
    if (third.empty()) return;

    // Чанк без квадов возвращать нечего: если бы возврат этого не
    // проверял, рендер получал бы его каждый кадр, не мог бы выгрузить
    // (выгружать нечего) и возвращал бы обратно — бесконечный круг на
    // ровном месте.
    auto probe = third.front().chunk;
    {
        std::lock_guard lk(probe->meshMutex);
        probe->mesh.built = false;
    }
    mgr.requeueMesh(probe);
    auto afterEmpty = mgr.pollMeshesReady();
    usize probeSeen = 0;
    for (auto& m : afterEmpty) if (m.chunk == probe) ++probeSeen;
    check(probeSeen == 0, "чанк без квадов в очередь не возвращается");

    // Выгруженный чанк — тем более: его буферы рендер уже отпустил, а
    // сам чанк вот-вот исчезнет.
    auto gone = third.back().chunk;
    {
        std::lock_guard lk(gone->meshMutex);
        gone->mesh.built = true;
    }
    mgr.removeChunks({ world::ChunkCoord{ gone->coord.x, gone->coord.z } });
    mgr.requeueMesh(gone);
    auto afterRemoved = mgr.pollMeshesReady();
    usize goneSeen = 0;
    for (auto& m : afterRemoved) if (m.chunk == gone) ++goneSeen;
    check(goneSeen == 0, "выгруженный чанк в очередь не возвращается");

    // И пустой указатель не роняет процесс: очередь чистят и из
    // обработчика нехватки памяти, где чанк мог уже уйти.
    mgr.requeueMesh(nullptr);
    check(true, "пустой чанк возврат переживает");
}

// ------------------------------------------------------------
// Ни один выход из выгрузки не теряет меш
//
// ChunkManager::requeueMesh умеет вернуть меш в очередь — но толку от
// этого ровно столько, сколько путей выхода из uploadChunks им
// пользуются. Забранный меш из очереди уже вычеркнут, и второго
// шанса не будет: пропущенный путь — это дыра в ландшафте, которая
// не зарастёт.
//
// Путей ровно три: пакет передачи не открылся (тогда потеряны ВСЕ
// меши кадра), выгрузка не удалась (потерян один), выгрузка удалась
// (меш на месте, квады можно отпускать). Проверяем, что первые два
// возвращают, а третий — отпускает.
// ------------------------------------------------------------
void testFailedUploadGoesBackToTheQueue() {
    group("рендер: неудавшаяся выгрузка возвращается в очередь");

    const std::string cr =
        readSource("app/src/main/cpp/src/render/chunk_renderer.cpp");
    if (cr.empty()) {
        check(true, "исходник не найден, проверка пропущена");
        return;
    }

    const usize body = cr.find("void ChunkRenderer::uploadChunks(");
    check(body != std::string::npos, "ChunkRenderer::uploadChunks на месте");
    if (body == std::string::npos) return;
    const usize bodyEnd = cr.find("\n}\n", body);
    check(bodyEnd != std::string::npos, "конец uploadChunks найден");
    if (bodyEnd == std::string::npos) return;
    const std::string win = cr.substr(body, bodyEnd - body);

    // Пакет не открылся — возвращаем весь забранный кадр.
    const usize batch = win.find("beginTransferBatch");
    check(batch != std::string::npos, "пакет передачи открывается здесь же");
    if (batch == std::string::npos) return;
    const usize batchFail = win.find("VK_NULL_HANDLE", batch);
    const usize batchLoop = win.find("for (const auto& rm : ready)", batch);
    check(batchFail != std::string::npos, "неоткрывшийся пакет обрабатывается");
    check(batchLoop != std::string::npos && batchLoop > batchFail,
          "и обрабатывается обходом всего забранного");
    if (batchFail == std::string::npos || batchLoop == std::string::npos) return;
    const usize batchRequeue = win.find("requeueMesh", batchFail);
    check(batchRequeue != std::string::npos && batchRequeue < win.find("staging_.collect"),
          "не открывшийся пакет возвращает в очередь ВСЕ меши кадра");

    // Каждый из трёх исходов разобран, и разобран по-своему.
    const usize done    = win.find("Upload::Done");
    const usize nothing = win.find("Upload::Nothing");
    const usize failed  = win.find("Upload::Failed");
    check(done    != std::string::npos, "исход «выгружено» разобран");
    check(nothing != std::string::npos, "исход «нечего выгружать» разобран");
    check(failed  != std::string::npos, "исход «не смогли» разобран");
    if (done == std::string::npos || nothing == std::string::npos ||
        failed == std::string::npos) return;

    auto tail = [&](usize from) {
        const usize nl = win.find('\n', from);
        return win.substr(from, nl == std::string::npos ? std::string::npos
                                                        : nl - from);
    };
    check(tail(done).find("releaseQuads") != std::string::npos,
          "выгруженный меш отпускает свои квады");
    check(tail(nothing).find("requeueMesh") == std::string::npos,
          "чанк без квадов в очередь не возвращается");
    check(tail(failed).find("requeueMesh") != std::string::npos,
          "неудавшаяся выгрузка возвращается в очередь");
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
// Мир не выходит за потолок памяти
//
// Сколько мегабайт занимает загруженный мир, раньше не спрашивал
// никто: объём определялся ТОЛЬКО дальностью прорисовки. При
// дальности 12 круг держит около 450 чанков — 115 МБ одних вокселей;
// на планшете дальность больше, и предела не было вовсе. Система
// узнаёт об этом раньше игры и сообщает единственным доступным ей
// способом — APP_CMD_LOW_MEMORY, а следом снятием процесса.
//
// Теперь потолок есть, и мир держится под ним сам.
// ------------------------------------------------------------
void testWorldStaysWithinMemoryBudget() {
    group("мир: потолок памяти соблюдается");

    world::blocks();
    if (!jobs::gJobs.running()) jobs::gJobs.start(2);

    // Размер чанка считается от типа, а не записан числом.
    const usize per = world::ChunkManager::CHUNK_BYTES;
    char m[220];
    std::snprintf(m, sizeof(m), "резидентный размер чанка: %zu КБ", per >> 10);
    check(per > 200u * 1024u && per < 400u * 1024u, m);

    std::snprintf(m, sizeof(m), "потолок по умолчанию: %zu МБ",
                  world::ChunkManager::DEFAULT_MEMORY_BUDGET >> 20);
    check(world::ChunkManager::DEFAULT_MEMORY_BUDGET == 512ull * 1024 * 1024, m);

    {
        world::ChunkManager mgr(0xB0DEULL, 8);
        check(mgr.memoryBudget() == world::ChunkManager::DEFAULT_MEMORY_BUDGET,
              "менеджер берёт потолок по умолчанию");
        std::snprintf(m, sizeof(m), "в 512 МБ помещается чанков: %zu",
                      mgr.chunkCapacity());
        check(mgr.chunkCapacity() > 1500, m);
    }

    // Тесный потолок: круг заведомо больше того, что в него влезает.
    {
        const i32 VD = 6;
        world::ChunkManager mgr(0xB0DEULL, VD);
        const usize cap = 40;
        mgr.setMemoryBudget(cap * per);
        check(mgr.chunkCapacity() == cap, "потолок пересчитан в число чанков");

        const glm::vec3 pos{ 0.f, 70.f, 0.f };
        usize peak = 0;
        for (int i = 0; i < 400; ++i) {
            mgr.update(pos);
            auto over = mgr.collectOverBudget(pos);
            if (!over.empty()) mgr.removeChunks(over);
            peak = std::max(peak, mgr.loadedChunks());
        }

        std::snprintf(m, sizeof(m),
                      "круг без потолка держал бы больше: загружено %zu при вместимости %zu",
                      mgr.loadedChunks(), cap);
        check(mgr.loadedChunks() <= cap, m);
        std::snprintf(m, sizeof(m), "за весь прогон не превысили потолок (пик %zu)", peak);
        check(peak <= cap, m);
        std::snprintf(m, sizeof(m), "занято %zu КБ при потолке %zu КБ",
                      mgr.residentBytes() >> 10, mgr.memoryBudget() >> 10);
        check(mgr.residentBytes() <= mgr.memoryBudget(), m);

        // Выгоняются ДАЛЬНИЕ, а не случайные: под игроком мир обязан
        // остаться, иначе он провалится сквозь него.
        auto under = mgr.findChunk(0, 0);
        check(under != nullptr, "чанк под игроком не выгружен");

        // И то, что осталось, ближе того, что выгнали.
        i32 worstKept = 0;
        {
            for (i32 dz = -VD - 2; dz <= VD + 2; ++dz)
                for (i32 dx = -VD - 2; dx <= VD + 2; ++dx)
                    if (mgr.findChunk(dx, dz))
                        worstKept = std::max(worstKept, dx * dx + dz * dz);
        }
        // Всё оставшееся лежит в круге радиуса, который вмещает `cap`
        // клеток: площадь круга радиуса r — pi*r^2.
        const i32 limit = (i32)((f32)cap / 3.14159f) + 3;
        std::snprintf(m, sizeof(m),
                      "оставлены ближние: дальний из оставшихся на %d (предел %d)",
                      worstKept, limit);
        check(worstKept <= limit, m);
    }

    // Потолок выше круга ничего не выгоняет: он потолок, а не квота.
    {
        world::ChunkManager mgr(0xB0DEULL, 4);
        for (int i = 0; i < 400; ++i) mgr.update({ 0.f, 70.f, 0.f });
        const usize before = mgr.loadedChunks();
        check(before > 20, "круг наполнен");
        check(mgr.collectOverBudget({ 0.f, 70.f, 0.f }).empty(),
              "при просторном потолке выгонять нечего");
        check(mgr.loadedChunks() == before, "и ничего не выгнано");
    }
}

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
    world::buildGreedyMesh(*lake, nb, quads);

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
    world::buildGreedyMesh(*rock, nb, quads);
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
// Переключатели настроек
// ------------------------------------------------------------
void testSettingsTogglesActuallyToggle() {
    group("настройки: переключатель и правда переключает");

    // Тап разбирается С КОНЦА списка интерактивных областей: побеждает
    // зарегистрированный ПОСЛЕДНИМ. Место вызова заводит область со
    // своим обработчиком, а рисующий виджет заводил поверх неё вторую,
    // с nullptr, — и обработчик не срабатывал никогда. Слайдеры
    // устроены иначе и работали: там область одна, и значение двигает
    // сам виджет.
    ui::UiContext ctx;
    ctx.init(nullptr, 1000, 500);

    bool value  = false;
    int  taps   = 0;
    const ui::Rect r{ 100.f, 100.f, 300.f, 60.f };

    auto frame = [&]() {
        ctx.beginFrame();
        const int idx = ctx.pushInteractiveRect(r, [&]() { value = !value; ++taps; });
        ui::toggleWidget(ctx, r, idx, &value, "Инверсия X");
        ctx.endFrame();
        return idx;
    };

    frame();
    check(ctx.handleTouch(1, 200.f, 130.f, 0), "нажатие попало в переключатель");
    frame();
    check(ctx.handleTouch(1, 200.f, 130.f, 1), "отпускание принято");
    check(taps == 1, "обработчик места вызова сработал");
    check(value, "значение переключилось");

    // И обратно.
    frame();
    ctx.handleTouch(2, 200.f, 130.f, 0);
    frame();
    ctx.handleTouch(2, 200.f, 130.f, 1);
    check(taps == 2 && !value, "второй тап вернул значение обратно");

    // Нажатое состояние виджет берёт у места вызова, а не заводит своё.
    frame();
    ctx.handleTouch(3, 200.f, 130.f, 0);
    const int idx = frame();
    check(ctx.isInteractivePressed(idx), "подсветка нажатия видна виджету");
    ctx.handleTouch(3, 200.f, 130.f, 1);

    // Выбор из списка — то же самое.
    u32 lang = 0;
    int cycles = 0;
    static const char* opts[3] = { "English", "Русский", "Deutsch" };
    auto cycleFrame = [&]() {
        ctx.beginFrame();
        const int i = ctx.pushInteractiveRect(r, [&]() {
            lang = (lang + 1) % 3; ++cycles;
        });
        ui::cycleWidget(ctx, r, i, "Язык", opts, 3, &lang);
        ctx.endFrame();
    };
    cycleFrame();
    ctx.handleTouch(4, 200.f, 130.f, 0);
    cycleFrame();
    ctx.handleTouch(4, 200.f, 130.f, 1);
    check(cycles == 1 && lang == 1, "выбор языка переключился");

    // ---- Исходники ----
    //
    // Виджет рисует. Заводить интерактивную область — дело места
    // вызова: оно одно знает, что делать по тапу.
    const std::string s = readSource("app/src/main/cpp/src/ui/slider.cpp");
    if (!s.empty()) {
        const usize t = s.find("void toggleWidget");
        const usize c = s.find("void cycleWidget");
        check(t != std::string::npos && c != std::string::npos,
              "оба виджета на месте");
        if (t != std::string::npos && c != std::string::npos) {
            const std::string tb = s.substr(t, c - t);
            check(tb.find("pushInteractiveRect") == std::string::npos,
                  "переключатель своей области не заводит");
            const std::string cb = s.substr(c);
            check(cb.find("pushInteractiveRect") == std::string::npos,
                  "выбор из списка — тоже");
        }
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
// Подгрузка мира не гасит экран
//
// На время генерации мира поверх всего лежала заливка
// rgba(8,12,18,190) во весь экран — три четверти непрозрачности на
// каждом пикселе. А мир к этому моменту уже нарисован и уже играбелен:
// чанки грузятся вокруг игрока, ближние готовы первыми. Занавес прятал
// ровно то, ради чего игрок ждёт, и создавал впечатление, что игра
// ещё не началась.
//
// Проверяется и размер плашки, и то, что во весь экран больше ничего
// не заливается.
// ------------------------------------------------------------
void testLoadingIsAPanelNotACurtain() {
    group("подгрузка: плашка, а не занавес");

    struct Size { f32 w, h; i32 dpi; const char* name; };
    const Size sizes[] = {
        { 2306.f, 1080.f, 400, "2306x1080 @400" },
        { 1280.f,  720.f, 320, "1280x720 @320"  },
        { 2560.f, 1600.f, 280, "2560x1600 @280" },
        {  960.f,  540.f, 240, "960x540 @240"   },
    };

    for (const auto& sz : sizes) {
        const ui::HudLayout L(sz.w, sz.h,
                              ui::theme::Metrics::fromDensityDpi(sz.dpi),
                              ui::SafeInsets{}, false);
        const ui::Rect r = L.loadingPanel();
        const f32 share = (r.w * r.h) / (sz.w * sz.h);

        char m[190];
        std::snprintf(m, sizeof(m), "%s: плашка занимает %.1f%% экрана",
                      sz.name, (double)(share * 100.f));
        // Занавес занимал 100 %. Плашка обязана быть именно плашкой.
        check(share < 0.10f, m);

        std::snprintf(m, sizeof(m), "%s: плашка целиком на экране", sz.name);
        check(r.x >= -0.5f && r.y >= -0.5f &&
              r.x + r.w <= sz.w + 0.5f && r.y + r.h <= sz.h + 0.5f, m);

        std::snprintf(m, sizeof(m), "%s: плашка не прилипла к краю", sz.name);
        check(r.x > 1.f && r.y > 1.f, m);
    }

    // И в самом коде отрисовки нет заливки во весь экран.
    const std::string src = readSource("app/src/main/cpp/src/ui/ui_system.cpp");
    if (src.empty()) {
        check(true, "исходник не найден, проверка пропущена");
        return;
    }
    const usize b = src.find("void UiSystem::drawLoadingOverlay()");
    check(b != std::string::npos, "drawLoadingOverlay на месте");
    if (b == std::string::npos) return;
    const usize e = src.find("\n}\n", b);
    check(e != std::string::npos, "конец drawLoadingOverlay найден");
    if (e == std::string::npos) return;
    const std::string body = src.substr(b, e - b);

    // Заливка от нуля до полной ширины и высоты — это и есть занавес.
    check(body.find("0.f, 0.f, w, h") == std::string::npos &&
          body.find("0.f, 0.f, (float)screenW_") == std::string::npos,
          "заливки во весь экран в отрисовке нет");
    check(body.find("loadingPanel()") != std::string::npos,
          "плашка берёт свой прямоугольник из раскладки");
}

// ------------------------------------------------------------
void testHudAndButtonsDoNotOverlap() {
    group("раскладка: HUD и экранные кнопки не налезают");

    // Проверяется ТА ЖЕ раскладка, по которой рисуется.
    //
    // Раньше здесь сверялись прямоугольники, которые не вызывал
    // никто, кроме этой проверки, а отрисовка считала свои числа
    // без общего масштаба. На 1280x720 расхождение
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
        // Плашка подгрузки живёт поверх обычного HUD, а не вместо
        // него: пока мир достраивается, играть уже можно.
        rects.push_back({ "плашка подгрузки", L.loadingPanel() });
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

    // И рисуется оно последним, поверх всего. Кадр собирает
    // buildFrame: render() только отправляет собранное на видеокарту.
    const usize build = src.find("void UiSystem::buildFrame(");
    check(build != NONE, "кадр собирается отдельно от отправки");
    if (build == NONE) return;
    const usize bend = src.find("\n}\n", build);
    const std::string rb = src.substr(build, bend - build);
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
// Блоки не берутся из ниоткуда и не пропадают в никуда.
//
// Установка читала активную ячейку пояса и ничего из неё не списывала:
// стопка не таяла никогда. Разрушение обращало воксель в воздух и не
// выдавало ничего — таблица ItemRegistry::blockToItem была построена
// при старте и ни разу не спрошена. В игре, целиком состоящей из
// блоков, добыча не приносила ничего, а строительство не стоило ничего.
// ------------------------------------------------------------
void testBlockEconomy() {
    group("блоки: установка списывает, разрушение выдаёт");

    world::blocks();
    items::items();

    // Таблица «блок → предмет» заполняется при старте реестра. Если
    // она пуста, всё остальное ниже проверяет пустоту.
    check(items::items().blockToItem(world::STONE) == items::ITEM_STONE,
          "таблица «блок → предмет» заполнена");

    jobs::gJobs.start(2);
    {
        world::ChunkManager mgr(0xB10CULL, 2);
        bool ready = false;
        for (int i = 0; i < 500 && !ready; ++i) {
            mgr.update({ 8.f, 70.f, 8.f });
            ready = mgr.isReadyAt(8, 8);
            if (!ready) std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        check(ready, "чанк сгенерирован");

        if (ready) {
            ecs::Registry reg;
            player::Player pl;
            const i32 surf = mgr.generator().surfaceHeight(8, 8);
            pl.init(reg, glm::vec3(8.5f, (f32)surf + 4.f, 8.5f));

            // Прицел по умолчанию смотрит на север (0,0,-1). Ставим
            // цель в двух блоках перед глазами и расчищаем путь,
            // чтобы луч дошёл именно до неё.
            const glm::vec3 eye = pl.eyePosition();
            const i32 bx = (i32)std::floor(eye.x);
            const i32 by = (i32)std::floor(eye.y);
            const i32 bz = (i32)std::floor(eye.z);
            mgr.setVoxel(bx, by, bz - 1, world::AIR);
            mgr.setVoxel(bx, by, bz - 2, world::STONE);

            // ---- Разрушение выдаёт предмет ----
            const usize before = reg.pool<items::ItemPickup>().size();
            check(pl.tryBreakBlock(mgr), "блок сломан");
            check(mgr.getVoxel(bx, by, bz - 2) == world::AIR,
                  "и воксель стал воздухом");

            auto& pool = reg.pool<items::ItemPickup>();
            check(pool.size() == before + 1, "из него выпал ровно один предмет");
            if (pool.size() == before + 1) {
                auto* pick = pool.get(pool.entityAt((u32)before));
                check(pick != nullptr, "выпавшее — настоящий предмет");
                if (pick) {
                    check(pick->stack.itemId == items::ITEM_STONE,
                          "и это камень, а не что попало");
                    check(pick->stack.count == 1, "в количестве одной штуки");
                }
            }

            // ---- Установка списывает из пояса ----
            auto* inv = pl.inventory();
            check(inv != nullptr, "инвентарь у игрока есть");
            if (inv) {
                auto& slot = inv->activeSlot();
                slot.itemId = items::ITEM_STONE;
                slot.count  = 5;

                // Ставить нужно во что-то: возвращаем опору.
                mgr.setVoxel(bx, by, bz - 2, world::STONE);
                check(pl.tryPlaceBlock(mgr, world::STONE), "блок поставлен");
                check(inv->activeSlot().count == 4,
                      "и одна штука списана из активной ячейки");

                // Последняя штука очищает ячейку, а не уходит в минус.
                inv->activeSlot().count = 1;
                mgr.setVoxel(bx, by, bz - 1, world::AIR);
                check(pl.tryPlaceBlock(mgr, world::STONE),
                      "последний блок из стопки ставится");
                check(inv->activeSlot().empty(),
                      "и ячейка после него пуста");

                // Пустая ячейка ничего не списывает и не уходит в минус.
                const u16 cnt = inv->activeSlot().count;
                check(cnt == 0, "счётчик не ушёл ниже нуля");
            }
        }
    }
    jobs::gJobs.stop();
}

// ------------------------------------------------------------
// У каждой экранной кнопки есть обработчик.
//
// Кнопка прыжка была зарегистрирована с `nullptr` вместо действия:
// `evJump_` не выставлялся НИКОГДА — ни с экрана, ни с геймпада, —
// и прыжок по кнопке не происходил вовсе. Компилятор такое не видит:
// nullptr — законное значение std::function. Кнопка при этом
// рисуется, нажимается и подсвечивается, просто ничего не делает.
// ------------------------------------------------------------
void testEveryButtonHasAnAction() {
    group("ввод: у каждой кнопки есть действие");

    const std::string src = readSource("app/src/main/cpp/src/main.cpp");
    if (src.empty()) { check(false, "main.cpp не прочитался"); return; }

    // Строки вида `btnXxx_ = add(cfg::Btn_Xxx, ...)`.
    int total = 0, dead = 0;
    std::string deadNames;
    for (const std::string& line : splitLines(src)) {
        const std::string t = trimmed(line);
        if (t.rfind("//", 0) == 0) continue;
        const usize a = t.find("= add(cfg::Btn_");
        if (a == std::string::npos) continue;
        ++total;
        if (t.find("nullptr") != std::string::npos) {
            ++dead;
            const usize b = t.find("cfg::Btn_");
            deadNames += " " + t.substr(b, t.find(',', b) - b);
        }
    }

    char m[200];
    std::snprintf(m, sizeof(m), "кнопок с действием: %d", total);
    check(total >= 6, m);
    std::snprintf(m, sizeof(m), "без действия зарегистрированы:%s",
                  dead ? deadNames.c_str() : " нет");
    check(dead == 0, m);
}

// ------------------------------------------------------------
// Бег: двойное нажатие по джойстику вместо кнопки.
//
// Кнопка бега занимала 64 dp на левой половине экрана — там же, где
// джойстик, — и требовала второй руки. Теперь бег живёт на том же
// пальце, что и направление: короткий тап, затем второе нажатие,
// которое игрок удерживает и ведёт.
// ------------------------------------------------------------
void testSprintByDoubleTap() {
    group("бег: двойное нажатие по джойстику");

    // Джойстик слева, экран 1920x1080 — палец кладём в левую половину.
    auto makeInput = [] {
        auto t = std::make_unique<input::TouchInput>();
        t->setViewport(1920, 1080);
        t->setJoystickLeftHanded(true);
        return t;
    };
    constexpr f32 JX = 400.f, JY = 800.f;

    auto down = [](input::TouchInput& t, f32 x, f32 y, f32 at) {
        t.onTouch(AMOTION_EVENT_ACTION_DOWN, 0, 1, x, y, at);
    };
    auto up = [](input::TouchInput& t, f32 x, f32 y, f32 at) {
        t.onTouch(AMOTION_EVENT_ACTION_UP, 0, 1, x, y, at);
    };
    auto move = [](input::TouchInput& t, f32 x, f32 y, f32 at) {
        t.onTouch(AMOTION_EVENT_ACTION_MOVE, 0, 1, x, y, at);
    };

    // ---- 1. Одиночное нажатие бег не включает ----
    {
        auto t = makeInput();
        down(*t, JX, JY, 0.0f);
        move(*t, JX, JY - 90.f, 0.05f);
        check(!t->sprintActive(), "одно нажатие — это шаг, а не бег");
        check(glm::length(t->moveAxis()) > 0.1f, "и направление при этом есть");
    }

    // ---- 2. Двойное нажатие включает бег ----
    {
        auto t = makeInput();
        down(*t, JX, JY, 0.00f);
        up  (*t, JX, JY, 0.08f);          // короткий тап
        down(*t, JX, JY, 0.14f);          // второе нажатие — сразу
        check(t->sprintActive(), "после двойного нажатия игрок бежит");

        move(*t, JX, JY - 100.f, 0.20f);  // ведём — бег не теряется
        check(t->sprintActive(), "и продолжает бежать, пока ведёт");
        check(t->moveAxis().y > 0.1f, "направление при этом рабочее");

        up(*t, JX, JY - 100.f, 0.60f);
        check(!t->sprintActive(), "отпустил — перестал бежать");
    }

    // ---- 3. Медленное повторное нажатие бег не включает ----
    //
    // Иначе любой повторный заход пальца на джойстик оказывался бы
    // бегом, и ходить шагом стало бы нельзя.
    {
        auto t = makeInput();
        down(*t, JX, JY, 0.00f);
        up  (*t, JX, JY, 0.08f);
        down(*t, JX, JY, 0.90f);          // спустя почти секунду
        check(!t->sprintActive(), "нажатие через паузу — обычный шаг");
    }

    // ---- 4. Первое нажатие должно быть ТАПОМ ----
    //
    // Если игрок вёл джойстик и отпустил, это не половина двойного
    // нажатия: иначе, отпустив после ходьбы и взявшись снова, он
    // каждый раз срывался бы на бег.
    {
        auto t = makeInput();
        down(*t, JX, JY, 0.00f);
        move(*t, JX, JY - 120.f, 0.10f);  // вёл
        up  (*t, JX, JY - 120.f, 0.30f);
        down(*t, JX, JY - 120.f, 0.36f);
        check(!t->sprintActive(), "после ведения повторное нажатие — шаг");
    }

    // ---- 5. Второе нажатие должно быть РЯДОМ ----
    {
        auto t = makeInput();
        down(*t, JX, JY, 0.00f);
        up  (*t, JX, JY, 0.08f);
        down(*t, JX + 500.f, JY, 0.14f);  // другой угол экрана
        check(!t->sprintActive(), "нажатие в стороне — не двойное");
    }

    // ---- 6. Геймпад бежит своей кнопкой ----
    //
    // Экранной кнопки, через которую бег раньше пропускали, больше
    // нет, и геймпаду нужен свой вход.
    {
        auto t = makeInput();
        check(!t->sprintActive(), "по умолчанию не бежит");
        t->injectGamepadSprint(true);
        check(t->sprintActive(), "кнопка геймпада включает бег");
        t->injectGamepadSprint(false);
        check(!t->sprintActive(), "и отпускается");
    }
}

// ------------------------------------------------------------
// Управление игроком: прыжок, подъём на ступень, бег.
//
// У контроллера персонажа не было НИ ОДНОЙ проверки — поэтому в нём
// и дожило до экрана то, что кнопка прыжка зарегистрирована с
// обработчиком nullptr: `evJump_` не выставлялся никогда, ни с
// экрана, ни с геймпада, и прыжок по кнопке не происходил вовсе.
// ------------------------------------------------------------
namespace {

/// Полигон: ровный каменный пол на высоте FLOOR и, по желанию,
/// ступень высотой stepH поперёк пути.
///
/// Мир строится НАСТОЯЩИМ ChunkManager: контроллер читает воксели
/// через VoxelReader, и подменять их нечем. Поэтому ждём генерацию,
/// а потом переписываем нужные чанки под себя.
struct Arena {
    static constexpr i32 FLOOR = 40;   // верх пола: стоять на y = 41

    std::unique_ptr<world::ChunkManager> mgr;

    bool build(i32 stepH, i32 stepAtX) {
        world::blocks();
        if (!jobs::gJobs.running()) jobs::gJobs.start(2);
        mgr = std::make_unique<world::ChunkManager>(777, 2);

        std::vector<std::shared_ptr<world::Chunk>> got;
        for (i32 cx = -1; cx <= 1; ++cx)
            for (i32 cz = -1; cz <= 1; ++cz)
                got.push_back(mgr->getChunk(cx, cz));

        // Ждём генерацию: писать в чанк до неё бессмысленно — задача
        // перезапишет всё своим рельефом.
        for (int spin = 0; spin < 20000; ++spin) {
            bool all = true;
            for (auto& c : got)
                if (!c->generated.load(std::memory_order_acquire)) all = false;
            if (all) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        for (auto& c : got)
            if (!c->generated.load(std::memory_order_acquire)) return false;

        for (auto& c : got) {
            for (i32 x = 0; x < world::CHUNK_SIZE; ++x)
                for (i32 z = 0; z < world::CHUNK_SIZE; ++z) {
                    const i32 wx = c->coord.x * world::CHUNK_SIZE + x;
                    for (i32 y = 0; y < world::CHUNK_SIZE_Y; ++y) {
                        u16 b = world::AIR;
                        if (y <= FLOOR) b = world::STONE;
                        // Ступень — стена высотой stepH, начиная с
                        // мировой координаты stepAtX и дальше.
                        if (stepH > 0 && wx >= stepAtX &&
                            y > FLOOR && y <= FLOOR + stepH) b = world::STONE;
                        c->setUnlocked(x, y, z, b);
                    }
                }
            c->version.fetch_add(1, std::memory_order_release);
        }
        return true;
    }
};

} // namespace

void testPlayerControls() {
    group("управление: прыжок, ступень, бег");

    Arena arena;
    if (!arena.build(0, 9999)) {
        check(false, "полигон не построился — генерация чанков не дождалась");
        return;
    }

    const f32 dt = 1.f / 60.f;
    const f32 standY = (f32)(Arena::FLOOR + 1);

    // ---- 1. Прыжок по нажатию ----
    {
        physics::CharacterController cc;
        cc.setPosition({ 4.5f, standY, 4.5f });

        // Дать встать на землю.
        physics::MoveInput idle{};
        for (int i = 0; i < 10; ++i) cc.update(*arena.mgr, idle, dt);
        check(cc.state().onGround, "игрок стоит на земле");
        const f32 y0 = cc.state().position.y;

        physics::MoveInput jump{};
        jump.jumpPressed = true;
        cc.update(*arena.mgr, jump, dt);
        check(cc.state().velocity.y > 1.f,
              "нажатие прыжка даёт скорость вверх");

        f32 peak = cc.state().position.y;
        physics::MoveInput hold{};
        for (int i = 0; i < 40; ++i) {
            cc.update(*arena.mgr, hold, dt);
            peak = std::max(peak, cc.state().position.y);
        }
        char m[140];
        std::snprintf(m, sizeof(m), "игрок поднялся на %.2f блока", peak - y0);
        check(peak > y0 + 1.f, m);

        for (int i = 0; i < 120; ++i) cc.update(*arena.mgr, hold, dt);
        check(cc.state().onGround, "и приземлился обратно");
    }

    // ---- 2. Без нажатия прыжка нет ----
    {
        physics::CharacterController cc;
        cc.setPosition({ 4.5f, standY, 4.5f });
        physics::MoveInput idle{};
        f32 peak = 0.f;
        for (int i = 0; i < 60; ++i) {
            cc.update(*arena.mgr, idle, dt);
            peak = std::max(peak, cc.state().position.y);
        }
        check(peak < standY + 0.2f, "без нажатия игрок стоит на месте");
    }

    // ---- 3. Подъём на ступень: плавно, а не телепортом ----
    //
    // Раньше контроллер убеждался, что наверху свободно, и ОДНОЙ
    // СТРОКОЙ переставлял игрока туда: `position.y += h`. Физически
    // верно, на глаз — скачок камеры на полметра.
    {
        Arena stepArena;
        if (!stepArena.build(1, 8)) {
            check(false, "полигон со ступенью не построился");
        } else {
            physics::CharacterController cc;
            cc.setPosition({ 4.5f, standY, 4.5f });

            physics::MoveInput mi{};
            mi.wishDir = { 1.f, 0.f };          // идём в +X, на ступень

            f32 prevY = cc.state().position.y;
            f32 biggestJump = 0.f;
            f32 topY = prevY;
            int framesRising = 0;
            for (int i = 0; i < 180; ++i) {
                cc.update(*stepArena.mgr, mi, dt);
                const f32 y = cc.state().position.y;
                const f32 d = y - prevY;
                if (d > 1e-4f) ++framesRising;
                biggestJump = std::max(biggestJump, std::fabs(d));
                topY = std::max(topY, y);
                prevY = y;
            }

            char m[180];
            std::snprintf(m, sizeof(m), "поднялся на %.2f (ступень 1.0)",
                          topY - standY);
            check(topY > standY + 0.9f, m);

            // Вот это и есть «плавно». За кадр при скорости подъёма
            // 4.5 блока в секунду выходит 0.075 блока; телепорт давал
            // бы всю высоту разом.
            std::snprintf(m, sizeof(m),
                          "самый большой шаг вверх за кадр — %.3f блока",
                          (double)biggestJump);
            check(biggestJump < 0.2f, m);

            std::snprintf(m, sizeof(m), "подъём занял кадров: %d", framesRising);
            check(framesRising >= 5, m);
        }
    }

    // ---- 3б. Стена в два блока остаётся стеной ----
    //
    // Подъём на ступень обязан брать один блок и ровно один: иначе
    // игрок въезжает на отвесные стены, и мир перестаёт держать.
    {
        Arena wall;
        if (!wall.build(2, 8)) {
            check(false, "полигон со стеной не построился");
        } else {
            physics::CharacterController cc;
            cc.setPosition({ 4.5f, standY, 4.5f });
            physics::MoveInput mi{};
            mi.wishDir = { 1.f, 0.f };
            f32 topY = standY;
            int inside = 0;
            for (int i = 0; i < 240; ++i) {
                cc.update(*wall.mgr, mi, dt);
                topY = std::max(topY, cc.state().position.y);
                // Упереться в стену — законно. ВЪЕХАТЬ в неё — нет:
                // изнутри геометрии игрока выталкивает наверх, и так
                // он и оказывался на крыше.
                if (physics::overlapsSolid(*wall.mgr,
                                           cc.box.min(cc.state().position),
                                           cc.box.max(cc.state().position)))
                    ++inside;
            }
            char m[160];
            std::snprintf(m, sizeof(m), "у стены в два блока игрок поднялся на %.2f",
                          topY - standY);
            check(topY < standY + 1.2f, m);
            check(cc.state().position.x < 8.f, "и не прошёл сквозь неё");
            std::snprintf(m, sizeof(m),
                          "и ни одного кадра не был внутри стены (было %d из 240)",
                          inside);
            check(inside == 0, m);
        }
    }

    // ---- 4. Бег быстрее шага ----
    {
        auto travel = [&](bool sprint) {
            physics::CharacterController cc;
            cc.setPosition({ 4.5f, standY, 4.5f });
            physics::MoveInput mi{};
            mi.wishDir = { 0.f, 1.f };
            mi.sprint = sprint;
            for (int i = 0; i < 120; ++i) cc.update(*arena.mgr, mi, dt);
            return cc.state().position.z - 4.5f;
        };
        const f32 walked = travel(false);
        const f32 ran    = travel(true);
        char m[140];
        std::snprintf(m, sizeof(m), "за две секунды шагом %.1f, бегом %.1f",
                      (double)walked, (double)ran);
        check(walked > 1.f, m);
        check(ran > walked * 1.3f, "бег заметно быстрее шага");
    }
}

// ------------------------------------------------------------
// Витрина: все модели проекта разом.
//
// Модели живут в разных местах — мобы в своём реестре, NPC в своём,
// игрок отдельно. Посмотреть на них ВМЕСТЕ было негде, а ошибка вида
// «у этого ноги короче туловища» видна только рядом с остальными:
// поодиночке она выглядит замыслом.

// ------------------------------------------------------------
// Ходьба никуда не телепортирует
//
// Самая заметная беда в игре: игрока дёргало вбок почти на целый блок
// за кадр, а иногда забрасывало на крышу дома. Все прежние проверки
// управления при этом проходили — они смотрели ТОЛЬКО на высоту
// подъёма на ступень, на ровном полигоне, на 60 кадрах в секунду.
// Ни одна не мерила смещение по горизонтали и ни одна не выходила на
// настоящий рельеф.
//
// Причин было две, и они складывались в цепочку.
//
// 1. `moveAxis` выбирал препятствием КРАЙНИЙ твёрдый блок из всех,
//    что задевает коробка, не проверяя, впереди ли он по ходу. Плюс
//    рамка перебора была расширена на EPS по ВСЕМ осям, поэтому в
//    горизонтальное движение попадал слой пола под ногами — а пол
//    твёрдый в каждом столбце. Игрока приставляло к дальней грани
//    собственного пола, то есть отбрасывало назад на блок.
// 2. Отброшенный назад игрок оказывался внутри геометрии;
//    спасательный подъём выталкивал его вверх, а `snapDown` —
//    функция «притянуть к земле» — ставила его на ВЕРХ того блока, в
//    котором он застрял. Почти целый блок вверх за кадр. Кадр за
//    кадром это и есть подъём по стене дома до конька.
//
// Замер на настоящем рельефе, сорок секунд ходьбы: было 11–12 рывков
// вбок за прогон, худший −1.098 блока за кадр; стало ноль на всех
// частотах кадров.
//
// Проверяется ИНВАРИАНТ, а не отдельный случай: разрешение
// столкновений вправе укоротить шаг, но не развернуть его.
// ------------------------------------------------------------
namespace {

/// Мир из настоящего рельефа, с подгрузкой на ходу.
struct LiveWorld {
    std::unique_ptr<world::ChunkManager> mgr;

    bool build(u64 seed) {
        world::blocks();
        if (!jobs::gJobs.running()) jobs::gJobs.start(2);
        mgr = std::make_unique<world::ChunkManager>(seed, 3);
        for (int i = 0; i < 3000; ++i) {
            mgr->update({ 0.f, 70.f, 0.f });
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            auto c = mgr->findChunk(0, 0);
            if (c && c->generated.load(std::memory_order_acquire)) return true;
        }
        return false;
    }

    f32 surfaceY(f32 x, f32 z) {
        world::VoxelReader rd(*mgr);
        auto& reg = world::blocks();
        for (i32 y = world::CHUNK_SIZE_Y - 2; y >= 0; --y)
            if (reg.isSolid(rd.at((i32)std::floor(x), y, (i32)std::floor(z))))
                return (f32)(y + 1);
        return 64.f;
    }
};

} // namespace

void testWalkingNeverTeleports() {
    group("управление: ходьба никуда не телепортирует");

    world::blocks();

    // ---- 1. Ровное поле, ступни ровно на границе блока ----
    //
    // Простейший случай из всех возможных, и именно его прежний код
    // не проходил: вокруг ни одного блока выше пола, а шаг вперёд
    // отбрасывал игрока назад — препятствием выбирался пол под ногами.
    {
        Arena flat;
        if (!flat.build(0, 9999)) {
            check(false, "полигон не построился");
        } else {
            physics::PlayerBox box;
            const f32 standY = (f32)(Arena::FLOOR + 1);   // ровно на границе
            glm::vec3 pos{ 5.70f, standY, 8.5f };
            glm::vec3 delta{ 0.1f, 0.f, 0.f };

            check(!physics::overlapsSolid(*flat.mgr, box.min(pos), box.max(pos)),
                  "на ровном поле игрок не внутри геометрии");

            const f32 before = pos.x;
            physics::resolveMovement(*flat.mgr, pos, delta, box);

            char m[160];
            std::snprintf(m, sizeof(m),
                          "шаг вперёд на ровном поле сдвинул на %+.4f (ждали +0.1)",
                          (double)(pos.x - before));
            check(pos.x > before + 0.09f, m);
        }
    }

    // ---- 2а. Блок ПОЗАДИ не смеет останавливать движение ----
    //
    // Самый чистый вид ошибки. Игрок стоит вплотную к стене, гранью в
    // грань, и идёт ОТ неё. Прежний перебор находил эту стену (она
    // задевается рамкой) и приставлял игрока к её ДАЛЬНЕЙ грани —
    // то есть швырял вперёд через полтора блока, хотя тот всего лишь
    // отходил.
    {
        Arena wall;
        if (!wall.build(3, 8)) {
            check(false, "полигон со стеной не построился");
        } else {
            physics::PlayerBox box;
            const f32 standY = (f32)(Arena::FLOOR + 1) + 1e-3f;
            // Ровно вплотную к грани стены: 8.0 - halfWidth.
            glm::vec3 pos{ 8.f - box.halfWidth, standY, 8.5f };

            check(!physics::overlapsSolid(*wall.mgr, box.min(pos), box.max(pos)),
                  "вплотную к стене — но не внутри неё");

            const f32 before = pos.x;
            glm::vec3 delta{ -0.2f, 0.f, 0.f };     // отходим ОТ стены
            physics::resolveMovement(*wall.mgr, pos, delta, box);

            char m[180];
            std::snprintf(m, sizeof(m),
                          "отход от стены сдвинул на %+.4f (ждали -0.2)",
                          (double)(pos.x - before));
            check(pos.x < before - 0.19f && pos.x > before - 0.21f, m);

            // И то же самое КРОШЕЧНЫМ шагом.
            //
            // Перебор вокселей идёт по коробке в НОВОМ положении.
            // Уйдя от стены на заметное расстояние, игрок её уже не
            // задевает, и ошибка не всплывает. А вот когда скорость
            // почти погашена — игрок прижат к стене и еле движется —
            // стена остаётся в рамке перебора, и без отсечения по
            // ходу движения её выбирает препятствием: игрока
            // приставляет к ДАЛЬНЕЙ грани, то есть швыряет сквозь
            // стену вперёд на полтора блока.
            glm::vec3 pos2{ 8.f - box.halfWidth, standY, 8.5f };
            const f32 before2 = pos2.x;
            glm::vec3 tiny{ -5e-5f, 0.f, 0.f };
            physics::resolveMovement(*wall.mgr, pos2, tiny, box);
            std::snprintf(m, sizeof(m),
                          "и крошечный отход тоже: %.4f -> %.4f",
                          (double)before2, (double)pos2.x);
            check(pos2.x <= before2 + 1e-3f && pos2.x > before2 - 0.05f, m);
        }
    }

    // ---- 2б. Инвариант: упор укорачивает шаг, но не разворачивает ----
    //
    // Перебором по сетке положений и направлений вокруг стены. Шаг
    // сетки выбран так, чтобы среди положений БЫЛИ приставленные к
    // грани блока вплотную: ошибка живёт именно там, а сетка покрупнее
    // проходит мимо неё и ничего не замечает.
    {
        Arena wall;
        if (!wall.build(3, 8)) {
            check(false, "полигон со стеной не построился");
        } else {
            physics::PlayerBox box;
            const f32 standY = (f32)(Arena::FLOOR + 1) + 1e-3f;
            int reversed = 0, overshot = 0, total = 0, flush = 0;
            f32 worst = 0.f;

            for (int ix = 0; ix <= 80; ++ix) {
                for (int iz = 0; iz < 6; ++iz) {
                    const f32 px = 5.0f + (f32)ix * 0.05f;   // 5.00 .. 9.00
                    const f32 pz = 8.0f + (f32)iz * 0.05f;
                    // Положения вплотную к грани: 7.70 (левая грань
                    // стены) и 8.30 (правая). Их и считаем отдельно —
                    // если их в сетке нет, проверка ни о чём.
                    if (std::fabs(px - (8.f - box.halfWidth)) < 1e-3f ||
                        std::fabs(px - (8.f + box.halfWidth)) < 1e-3f) ++flush;

                    for (int d = 0; d < 4; ++d) {
                      // Два размера шага: обычный и почти нулевой.
                      // Прижатый к стене игрок, у которого погашена
                      // скорость, двигается вторым — и ошибка видна
                      // только на нём.
                      for (int sz2 = 0; sz2 < 2; ++sz2) {
                        for (int dy = 0; dy < 3; ++dy) {
                            glm::vec3 pos{ px, standY + (f32)dy * 0.5f, pz };
                            if (physics::overlapsSolid(*wall.mgr, box.min(pos),
                                                       box.max(pos)))
                                continue;
                            const glm::vec3 start = pos;
                            glm::vec3 delta{ 0.f, 0.f, 0.f };
                            const f32 step = sz2 ? 5e-5f : 0.2f;
                            if (d == 0) delta.x =  step;
                            if (d == 1) delta.x = -step;
                            if (d == 2) delta.z =  step;
                            if (d == 3) delta.z = -step;
                            const glm::vec3 want = delta;

                            physics::resolveMovement(*wall.mgr, pos, delta, box);
                            ++total;

                            for (int ax = 0; ax < 3; ax += 2) {
                                const f32 moved = pos[ax] - start[ax];
                                const f32 asked = want[ax];
                                if (asked == 0.f) {
                                    if (std::fabs(moved) > 1e-3f) {
                                        ++reversed;
                                        worst = std::max(worst, std::fabs(moved));
                                    }
                                    continue;
                                }
                                if (moved * asked < -1e-3f) {
                                    ++reversed;
                                    worst = std::max(worst, std::fabs(moved));
                                }
                                if (std::fabs(moved) > std::fabs(asked) + 1e-3f) {
                                    ++overshot;
                                    worst = std::max(worst, std::fabs(moved));
                                }
                            }
                        }
                      }
                    }
                }
            }
            char m[220];
            std::snprintf(m, sizeof(m), "проверено положений: %d", total);
            check(total > 1000, m);
            std::snprintf(m, sizeof(m),
                          "среди них есть приставленные вплотную к грани: %d", flush);
            check(flush > 0, m);
            std::snprintf(m, sizeof(m),
                          "упор ни разу не отбросил назад (случаев %d, худший %.3f блока)",
                          reversed, (double)worst);
            check(reversed == 0, m);
            std::snprintf(m, sizeof(m),
                          "упор ни разу не протащил дальше просимого (случаев %d)",
                          overshot);
            check(overshot == 0, m);
        }
    }

    // ---- 3. Настоящий рельеф, разные частоты кадров, бег ----
    //
    // Тот самый случай, на котором это и видно в игре. Ровный полигон
    // его не ловит: там нет ни склонов, ни уступов, ни дробных высот.
    {
        LiveWorld w;
        if (!w.build(20260916ULL)) {
            check(false, "мир не построился");
        } else {
            const f32 dts[3]  = { 1.f / 60.f, 1.f / 30.f, 1.f / 20.f };
            const char* nm[3] = { "60", "30", "20" };

            for (int k = 0; k < 3; ++k) {
                const f32 dt = dts[k];
                physics::CharacterController cc;
                const f32 sx = 0.5f, sz = 0.5f;
                cc.setPosition({ sx, w.surfaceY(sx, sz) + 0.1f, sz });

                physics::MoveInput idle{};
                for (int i = 0; i < 30; ++i) cc.update(*w.mgr, idle, dt);

                physics::MoveInput go{};
                go.wishDir = { 0.707f, 0.707f };
                go.sprint  = true;

                // Чего движок вправе двигать за кадр: бег плюс запас
                // на разгон и на подъём на ступень.
                const f32 horizLimit = cc.sprintSpeed * dt * 1.6f + 0.02f;
                const f32 vertLimit  = cc.stepClimbSpeed * dt * 1.6f + 0.02f;

                int jumpH = 0, jumpV = 0, inside = 0;
                f32 worstH = 0.f, worstV = 0.f;
                glm::vec3 prev = cc.state().position;
                const int frames = (int)(12.f / dt);
                for (int i = 0; i < frames; ++i) {
                    w.mgr->update(cc.state().position);
                    cc.update(*w.mgr, go, dt);
                    const glm::vec3 p = cc.state().position;
                    const glm::vec3 d = p - prev;
                    const f32 h = std::sqrt(d.x * d.x + d.z * d.z);
                    if (h > horizLimit) { ++jumpH; worstH = std::max(worstH, h); }
                    // Вниз игрок падает свободно — это не телепорт.
                    if (d.y > vertLimit) { ++jumpV; worstV = std::max(worstV, d.y); }
                    // И ни в одном кадре он не внутри геометрии.
                    // Это и есть то состояние, из которого его потом
                    // выталкивало наверх: пока оно не возникает,
                    // выталкивать нечего.
                    if (physics::overlapsSolid(*w.mgr, cc.box.min(p), cc.box.max(p)))
                        ++inside;
                    prev = p;
                }
                char m[200];
                std::snprintf(m, sizeof(m),
                              "%s к/с: рывков вбок нет (было %d, худший %.3f при пределе %.3f)",
                              nm[k], jumpH, (double)worstH, (double)horizLimit);
                check(jumpH == 0, m);
                std::snprintf(m, sizeof(m),
                              "%s к/с: рывков вверх нет (было %d, худший %.3f при пределе %.3f)",
                              nm[k], jumpV, (double)worstV, (double)vertLimit);
                check(jumpV == 0, m);
                std::snprintf(m, sizeof(m),
                              "%s к/с: ни одного кадра внутри геометрии (было %d из %d)",
                              nm[k], inside, frames);
                check(inside == 0, m);
            }
        }
    }

    // ---- 3б. Быстрое падение не проваливается сквозь пол ----
    //
    // За кадр падающий игрок проходит больше блока: на 20 кадрах в
    // секунду и скорости падения 30 блоков в секунду это полтора
    // блока, а перед самой землёй и больше. В перебор при этом
    // попадает НЕСКОЛЬКО перекрытий сразу, и выбрать нужно САМОЕ
    // ВЕРХНЕЕ из тех, что игрок пересёк. Возьмёшь нижнее — игрок
    // провалится сквозь пол в подвал, и это опять «телепортация».
    {
        Arena flat;
        if (!flat.build(0, 9999)) {
            check(false, "полигон не построился");
        } else {
            // Пол сплошной до FLOOR включительно: перекрытий под ним
            // сколько угодно, все твёрдые.
            physics::PlayerBox box;
            const f32 topY = (f32)(Arena::FLOOR + 1);

            int fellThrough = 0, landed = 0;
            f32 lowest = topY;
            for (int k = 0; k < 40; ++k) {
                // Падаем с разной высоты и с разным шагом кадра, чтобы
                // перекрытие оказывалось в разных местах шага.
                const f32 h  = 3.f + (f32)k * 0.37f;
                glm::vec3 pos{ 4.5f + (f32)(k % 7) * 0.1f, topY + h, 4.5f };
                glm::vec3 delta{ 0.f, -(h + 2.f), 0.f };   // проскок за один кадр
                physics::CollisionFlags f =
                    physics::resolveMovement(*flat.mgr, pos, delta, box);
                if (f.onGround) ++landed;
                if (pos.y < topY - 1e-3f) { ++fellThrough; }
                lowest = std::min(lowest, pos.y);
            }
            char m[200];
            std::snprintf(m, sizeof(m), "приземлений из 40 падений: %d", landed);
            check(landed == 40, m);
            std::snprintf(m, sizeof(m),
                          "сквозь пол не провалился ни разу (случаев %d, ниже всего %.3f при полу %.1f)",
                          fellThrough, (double)lowest, (double)topY);
            check(fellThrough == 0, m);
        }
    }

    // ---- 4. «Притянуть к земле» не подбрасывает ----
    //
    // snapDown ставит игрока на верх найденного под ним блока. Если
    // ступни уже внутри блока, этот верх ВЫШЕ игрока, и функция с
    // именем «вниз» кидала его вверх почти на блок. Замер на рельефе:
    // 29.08 -> 30.00 за один кадр.
    {
        Arena flat;
        if (!flat.build(0, 9999)) {
            check(false, "полигон не построился");
        } else {
            const f32 standY = (f32)(Arena::FLOOR + 1);
            physics::CharacterController cc;
            // Ступни ВНУТРИ верхнего блока пола: ровно то положение,
            // в которое игрока загонял спасательный подъём.
            cc.setPosition({ 4.5f, standY - 0.9f, 4.5f });
            const f32 y0 = cc.state().position.y;

            physics::MoveInput idle{};
            cc.update(*flat.mgr, idle, 1.f / 30.f);
            const f32 y1 = cc.state().position.y;

            char m[180];
            std::snprintf(m, sizeof(m),
                          "из блока игрока не подбрасывает: %.3f -> %.3f за кадр",
                          (double)y0, (double)y1);
            // Спасательный подъём вправе вытолкнуть его на 2.5 блока в
            // секунду — это 0.083 за кадр, но никак не целый блок.
            check(y1 - y0 < 0.2f, m);
        }
    }
}

// ------------------------------------------------------------
void testShowcaseHoldsEveryModel() {
    group("витрина: все модели проекта разом");

    entity::ShowcaseSlot slots[entity::SHOWCASE_MAX];
    const u8 n = entity::buildShowcase(slots, entity::SHOWCASE_MAX, 1.2f, 0.7f);

    char m[200];
    std::snprintf(m, sizeof(m), "на витрине моделей: %u", (unsigned)n);
    check(n >= 1u + (npc::NPC_COUNT - 1) + (mobs::MOB_COUNT - 1), m);

    // ---- Витрина детерминированна ----
    //
    // Ни времени, ни случайности, ни зерна мира. Иначе два прогона
    // дают разное, и сравнивать нечего.
    {
        entity::ShowcaseSlot a[entity::SHOWCASE_MAX], b[entity::SHOWCASE_MAX];
        const u8 na = entity::buildShowcase(a, entity::SHOWCASE_MAX, 1.2f, 0.7f);
        const u8 nb = entity::buildShowcase(b, entity::SHOWCASE_MAX, 1.2f, 0.7f);
        bool same = (na == nb);
        for (u8 i = 0; i < na && same; ++i)
            same = (a[i].rig == b[i].rig) && (a[i].pos == b[i].pos);
        check(same, "два прогона дают одну и ту же витрину");
    }

    // ---- Ни одна модель не сломана ----
    int notGrounded = 0, tooTall = 0, noParts = 0, overlapRow = 0, nan = 0;
    for (u8 i = 0; i < n; ++i) {
        const entity::Rig& rig = *slots[i].rig;
        entity::Pose pose;
        anim::poseFor(rig, pose, slots[i].state);

        entity::ResolvedPart parts[entity::MAX_PARTS];
        const u8 cnt = entity::resolve(rig, pose, slots[i].pos, slots[i].yaw,
                                       parts, entity::MAX_PARTS);
        if (cnt == 0) { ++noParts; continue; }

        f32 lo = parts[0].center.y, hi = lo, wide = 0.f;
        for (u8 k = 0; k < cnt; ++k) {
            const auto& p = parts[k];
            if (!std::isfinite(p.center.x) || !std::isfinite(p.center.y) ||
                !std::isfinite(p.center.z) || !std::isfinite(p.size.y)) ++nan;
            lo = std::min(lo, p.center.y - p.size.y * 0.5f);
            hi = std::max(hi, p.center.y + p.size.y * 0.5f);
            wide = std::max(wide,
                std::fabs(p.center.x - slots[i].pos.x) + p.size.x * 0.5f);
        }

        // В движении подошва отрывается от земли — это шаг, а не
        // парение. Но полметра под землёй или над ней — уже беда.
        if (lo < -0.35f || lo > 0.35f) {
            ++notGrounded;
            std::snprintf(m, sizeof(m), "%s: подошва на %.2f", slots[i].name, lo);
            check(false, m);
        }
        // Витрина не должна превращаться в кучу: соседи стоят через
        // четыре метра.
        if (wide > 2.f) {
            ++overlapRow;
            std::snprintf(m, sizeof(m), "%s: шире соседского места (%.2f)",
                          slots[i].name, wide);
            check(false, m);
        }
        if (hi - lo > 6.f) ++tooTall;
    }
    check(noParts == 0, "у каждой модели есть хоть одна коробка");
    check(nan == 0, "в позах нет не-чисел");
    check(notGrounded == 0, "все модели стоят на земле");
    check(overlapRow == 0, "и не налезают на соседей");
    check(tooTall == 0, "и ни одна не выше шести метров");

    // ---- Походка ложится на все скелеты ----
    //
    // Одна и та же локомоция обслуживает двуногих и четвероногих. Если
    // на каком-то скелете она не даёт движения — значит роли частей
    // этого скелета она не знает, и существо идёт, не шевелясь.
    {
        int frozen = 0;
        for (u8 i = 0; i < n; ++i) {
            const entity::Rig& rig = *slots[i].rig;
            anim::AnimState still; still.phase = 1.2f; still.speedNorm = 0.f;
            anim::AnimState moving; moving.phase = 1.2f; moving.speedNorm = 1.f;

            entity::Pose a, b;
            anim::poseFor(rig, a, still);
            anim::poseFor(rig, b, moving);

            // Смотрим на БЁДРА, а не на что попало. Корпус
            // покачивается на ходу и сам по себе, и первая версия
            // этой проверки принимала его качание за походку: мутация
            // «бёдра не качаются» её пережила.
            f32 worst = 0.f;
            bool hasLegs = false;
            for (u8 k = 0; k < rig.count; ++k) {
                if (!anim::isUpperLimb(rig.parts[k].role)) continue;
                hasLegs = true;
                for (int c = 0; c < 3; ++c)
                    worst = std::max(worst, std::fabs(a.euler[k][c] - b.euler[k][c]));
            }
            if (hasLegs && worst < 0.05f) {
                ++frozen;
                std::snprintf(m, sizeof(m), "%s: на ходу не шевелится", slots[i].name);
                check(false, m);
            }
        }
        check(frozen == 0, "походка оживляет каждый скелет с ногами");
    }
}

// ------------------------------------------------------------
// Цена моделей в коробках.
//
// Оснастка сделала существо дороже: вместо плоского списка из шести
// коробок у него теперь иерархия из полутора десятков, и каждая
// коробка — инстанс. Это прямая цена кадра, и она обязана быть
// НАЗВАНА, а не обнаружена на устройстве.
// ------------------------------------------------------------
void testEntityInstanceBudget() {
    group("цена: сколько коробок стоит сущность");

    char m[220];

    // ---- Ни одна модель не переполняет оснастку ----
    {
        int over = 0;
        u8 worst = 0;
        const char* worstName = "?";
        for (u16 id = 1; id < mobs::MOB_COUNT; ++id) {
            const entity::Rig& r = mobs::rigFor(id);
            if (r.count > entity::MAX_PARTS) ++over;
            if (r.count > worst) { worst = r.count; worstName = mobs::mobRegistry().get(id).name; }
        }
        for (u16 id = 1; id < npc::NPC_COUNT; ++id) {
            const entity::Rig& r = npc::rigFor(id, 0u);
            if (r.count > entity::MAX_PARTS) ++over;
            if (r.count > worst) { worst = r.count; worstName = npc::npcRegistry().get(id).name; }
        }
        std::snprintf(m, sizeof(m), "самая сложная модель — %s, частей %u из %u",
                      worstName, (unsigned)worst, (unsigned)entity::MAX_PARTS);
        check(over == 0, m);
        // Запас нужен: без него следующая примета упрётся в предел и
        // будет молча отброшена rig.add().
        check(worst <= entity::MAX_PARTS - 2, "и до предела остаётся запас");
    }

    // ---- Цена сцены ----
    //
    // Полсотни существ в поле зрения — обычная нагрузка боя у деревни.
    {
        const u32 showcase = entity::showcaseBoxCount(1.2f, 1.f);
        entity::ShowcaseSlot slots[entity::SHOWCASE_MAX];
        const u8 n = entity::buildShowcase(slots, entity::SHOWCASE_MAX, 1.2f, 1.f);
        const f32 perEntity = n ? (f32)showcase / (f32)n : 0.f;

        const u32 fifty = (u32)(perEntity * 50.f);
        std::snprintf(m, sizeof(m),
                      "витрина: %u коробок на %u моделей — %.1f на сущность, "
                      "%u на полсотни",
                      showcase, (unsigned)n, (double)perEntity, fifty);
        check(true, m);

        // Один инстансный вызов рисует их все, и коробка стоит 48
        // байт. Полсотни существ — это единицы килобайт на кадр;
        // бюджет назван, чтобы следующее усложнение моделей упёрлось
        // в проверку, а не в кадр на устройстве.
        check(perEntity <= 20.f, "на сущность уходит не больше двадцати коробок");
        check(fifty * sizeof(render::MobInstance) <= 64u * 1024u,
              "полсотни существ укладываются в 64 КБ инстанс-буфера");
    }

    // ---- Опора выводится, а не задаётся руками ----
    //
    // Сейчас все сборщики кладут ступни ровно в ноль, и groundOffset у
    // всех видов нулевой. Это не повод считать его украшением: он и
    // есть то, что делает «подошва на опоре» верным ПО ПОСТРОЕНИЮ, а
    // не по внимательности автора очередной модели. Проверяем на
    // оснастке, собранной нарочно неправильно — с коробкой ниже нуля.
    {
        entity::Rig sunk;
        entity::Part root;
        root.parent = -1; root.role = entity::PartRole::Root; root.visible = false;
        const u8 iRoot = sunk.add(root);

        entity::Part body;
        body.parent    = (i8)iRoot;
        body.role      = entity::PartRole::Torso;
        body.pivot     = glm::vec3(0.f, -0.6f, 0.f);   // закопан
        body.boxOffset = glm::vec3(0.f);
        body.size      = glm::vec3(1.f);
        sunk.add(body);

        check(entity::lowestPoint(sunk, sunk.rest, 0.f) < -1.f,
              "нарочно закопанная оснастка и правда под землёй");

        sunk.groundOffset = -entity::lowestPoint(sunk, sunk.rest, 0.f);
        check(std::fabs(entity::lowestPoint(sunk, sunk.rest, 0.f)) < 1e-4f,
              "а после вывода groundOffset садится на опору");
    }

    // ---- Поза не аллоцирует ----
    //
    // Сборка позы идёт каждый кадр на каждое существо. Куча здесь —
    // это кадровые заикания, которые на устройстве ищут неделями.
    {
        const entity::Rig& rig = mobs::rigFor(mobs::MOB_WOLF);
        check(sizeof(entity::Pose) <= 16u * entity::MAX_PARTS,
              "поза — простой массив углов, без косвенности");
        check(sizeof(entity::ResolvedPart) <= 64u,
              "разрешённая часть влезает в кэш-линию");
        check(rig.count > 0, "оснастка волка построена");
    }
}

// ------------------------------------------------------------
// Деревня: дом — это дом, а не коробка с кустом на крыше.
//
// Прежний дом был коробкой из WOOD с плоской нашлёпкой из LEAVES
// сверху и лужей ЛАВЫ посередине вместо очага. Дверь всегда стояла в
// стене -Z независимо от того, где центр деревни, поэтому у половины
// домов вход смотрел в поле. Окон не было вовсе.
// ------------------------------------------------------------
// Порядок каналов цвета у инстансных коробок
// ------------------------------------------------------------
void testInstanceColorByteOrder() {
    group("инстансы: цвет доходит до видеокарты в своём порядке");

    // Атрибут объявлен VK_FORMAT_R8G8B8A8_UNORM. Такой формат берёт
    // четыре байта В ПОРЯДКЕ ПАМЯТИ, а не разряды слова. Значит
    // проверять надо именно память, а не число.
    render::MobInstance inst{};
    inst.colorGpu = render::packInstanceColor(0xF2D3B0FFu);   // тон кожи

    u8 bytes[4];
    std::memcpy(bytes, &inst.colorGpu, 4);
    check(bytes[0] == 0xF2, "первый байт — красный");
    check(bytes[1] == 0xD3, "второй — зелёный");
    check(bytes[2] == 0xB0, "третий — синий");
    check(bytes[3] == 0xFF, "четвёртый — альфа");

    // Тот же тон без упаковки лёг бы в память задом наперёд, и
    // шейдер прочёл бы ярко-розовое вместо кожи. Проверка не на
    // реализацию, а на то, что упаковка вообще что-то меняет.
    u32 raw = 0xF2D3B0FFu;
    u8 rawBytes[4];
    std::memcpy(rawBytes, &raw, 4);
    check(rawBytes[0] != 0xF2 || rawBytes[3] != 0xFF,
          "без упаковки порядок байтов был бы другим");

    // Непрозрачное остаётся непрозрачным: раньше альфа приходила из
    // байта R, и у снарядов со смешиванием она выходила произвольной.
    for (u32 c : { 0x000000FFu, 0xFFFFFFFFu, 0x60D060FFu, 0x4090FFFFu }) {
        u8 b4[4];
        const u32 packed = render::packInstanceColor(c);
        std::memcpy(b4, &packed, 4);
        if (b4[3] != 0xFF) { check(false, "альфа непрозрачного цвета уцелела"); break; }
        if (c == 0x4090FFFFu) check(true, "альфа непрозрачного цвета уцелела");
    }

    // Полупрозрачность снаряда доходит как задумана.
    const u32 fade = render::packInstanceColor(0x80C0FF40u);
    u8 fb[4];
    std::memcpy(fb, &fade, 4);
    check(fb[3] == 0x40, "полупрозрачность снаряда доходит как задумана");

    // Обратное преобразование ничего не теряет.
    const u32 back = render::packInstanceColor(render::packInstanceColor(0x123456A5u));
    check(back == 0x123456A5u, "упаковка обратима сама себе");

    // ---- Исходники ----
    //
    // Поле называется colorGpu нарочно: забытое место присваивания не
    // соберётся. Проверка стережёт и это имя, и формат атрибута — на
    // нём держится вся раскладка байтов.
    const std::string h = readSource("app/src/main/cpp/src/render/mob_renderer.h");
    if (!h.empty()) {
        check(h.find("u32       colorGpu;") != std::string::npos,
              "поле инстанса называется colorGpu");
        check(h.find("VK_FORMAT_R8G8B8A8_UNORM") != std::string::npos,
              "атрибут цвета читает байты в порядке памяти");
        check(h.find("static_assert(offsetof(MobInstance, colorGpu) == 24);")
              != std::string::npos, "смещение цвета сверяется компилятором");
    }
    usize raws = 0;
    for (const char* f : { "app/src/main/cpp/src/render/mob_renderer.cpp",
                           "app/src/main/cpp/src/render/npc_renderer.cpp",
                           "app/src/main/cpp/src/render/item_renderer.cpp",
                           "app/src/main/cpp/src/render/projectile_renderer.cpp" }) {
        const std::string s = readSource(f);
        for (usize i = s.find("inst.color"); i != std::string::npos;
             i = s.find("inst.color", i + 1))
            if (s.compare(i, 14, "inst.colorGpu ") != 0 &&
                s.compare(i, 13, "inst.colorGpu") != 0) ++raws;
    }
    check(raws == 0, "цвет инстанса нигде не присваивается в обход упаковки");
}

// ------------------------------------------------------------
// Динамическое состояние задают все проходы
// ------------------------------------------------------------
void testEveryPassSetsViewport() {
    group("проходы: вьюпорт и ножницы задаёт каждый, кто рисует");

    // Вьюпорт и ножницы объявлены динамическим состоянием у каждого
    // конвейера. Динамическое состояние НЕ наследуется между
    // командными буферами и не имеет значения по умолчанию: рисовать,
    // не задав его в этом буфере, — неопределённое поведение.
    //
    // Задавали его не все: трава, вода и контур блока пользовались
    // тем, что оставил предыдущий проход. Держалось это на их
    // порядке, а выключатель проходов (render_passes) сделал порядок
    // непостоянным — сняв ландшафт, сущности и небо, оставшиеся
    // рисовали вовсе без вьюпорта. Слоя проверки на устройстве нет,
    // сказать об этом было некому.
    static const char* files[] = {
        "app/src/main/cpp/src/render/chunk_renderer.cpp",
        "app/src/main/cpp/src/render/instanced_renderer.cpp",
        "app/src/main/cpp/src/render/mob_renderer.cpp",
        "app/src/main/cpp/src/render/npc_renderer.cpp",
        "app/src/main/cpp/src/render/item_renderer.cpp",
        "app/src/main/cpp/src/render/projectile_renderer.cpp",
        "app/src/main/cpp/src/render/block_outline.cpp",
        "app/src/main/cpp/src/render/skybox.cpp",
        "app/src/main/cpp/src/ui/ui_renderer.cpp",
    };

    usize draws = 0, missing = 0;
    std::string firstBad;
    for (const char* f : files) {
        const std::string src = readSource(f);
        if (src.empty()) continue;
        // Тела функций верхнего уровня: между закрывающими скобками в
        // первой колонке. Грубо, но для этих файлов ровно так и есть.
        usize from = 0;
        while (from < src.size()) {
            usize end = src.find("\n}\n", from);
            const std::string body =
                src.substr(from, (end == std::string::npos ? src.size() : end) - from);
            from = (end == std::string::npos) ? src.size() : end + 3;

            // Точка входа прохода — та, которой передали контекст и
            // которая привязывает свой конвейер. Внутренние помощники
            // вроде ChunkRenderer::drawMesh рисуют по чужому
            // командному буферу и уже привязанным конвейером: вьюпорт
            // за них задал позвавший.
            if (body.find("vkCmdBindPipeline") == std::string::npos) continue;
            if (body.find("vk::Context& ctx") == std::string::npos) continue;
            ++draws;
            if (body.find("setFullViewport") != std::string::npos) continue;
            ++missing;
            if (firstBad.empty()) {
                const usize s = body.rfind("\nvoid ");
                firstBad = std::string(f) + ": " +
                           (s == std::string::npos ? std::string("?")
                                                   : body.substr(s + 1, 60));
            }
        }
    }

    check(draws >= 10, "проходы, которые рисуют, найдены");
    if (draws < 9) std::printf("       найдено только %zu\n", draws);
    check(missing == 0, "каждый из них задаёт вьюпорт и ножницы сам");
    if (missing) std::printf("       первый без вьюпорта: %s\n", firstBad.c_str());

    // Определение одно на всех: семь дословных копий этого блока и
    // были тем, из-за чего три прохода про него забыли.
    const std::string h = readSource("app/src/main/cpp/src/vk/vk_context.h");
    if (!h.empty())
        check(h.find("void setFullViewport(VkCommandBuffer cmd) const")
              != std::string::npos, "и берёт его из одного определения");
    usize copies = 0;
    for (const char* f : files) {
        const std::string src = readSource(f);
        if (src.find("vkCmdSetViewport") != std::string::npos) ++copies;
    }
    check(copies == 0, "своих копий vkCmdSetViewport у проходов не осталось");
}

// ------------------------------------------------------------
void testEveryUiCallbackIsWired() {
    group("интерфейс: у каждого коллбэка есть обработчик");

    // UiSystem не делает ничего сам: он зовёт std::function, которую
    // ему выдали снаружи. Незаданная std::function пустая, и вызов
    // через `if (cb) cb();` просто не случается — молча, без ошибки.
    // Так кнопка «в пояс» два выпуска подряд снимала выделение и не
    // перекладывала предмет: onMoveItem не назначал никто.
    const std::string hdr =
        readSource("app/src/main/cpp/src/ui/ui_system.h");
    const std::string mainSrc =
        readSource("app/src/main/cpp/src/main.cpp");
    check(!hdr.empty() && !mainSrc.empty(), "исходники прочитаны");
    if (hdr.empty() || mainSrc.empty()) return;

    // Имена коллбэков: `std::function<...> onЧтоТо;` из раздела
    // «Коллбэки». Дальше по файлу std::function встречается и внутри
    // вложенных структур (Confirm::onYes) — там обработчик задаёт не
    // main.cpp, а тот, кто открыл окно.
    const usize secFrom = hdr.find("---- \u041a\u043e\u043b\u043b\u0431\u044d\u043a\u0438 ----");
    const usize secTo   = (secFrom == std::string::npos)
                        ? std::string::npos : hdr.find("struct ", secFrom);
    check(secFrom != std::string::npos, "раздел коллбэков найден");
    if (secFrom == std::string::npos) return;

    std::vector<std::string> names;
    usize p = secFrom;
    while ((p = hdr.find("std::function<", p)) != std::string::npos &&
           (secTo == std::string::npos || p < secTo)) {
        const usize semi = hdr.find(';', p);
        if (semi == std::string::npos) break;
        const std::string decl = hdr.substr(p, semi - p);
        // Искать «on» с начала нельзя: оно есть уже в «std::function».
        // Имя стоит после закрывающей скобки шаблона.
        const usize gt = decl.rfind('>');
        const usize on = (gt == std::string::npos)
                       ? std::string::npos : decl.find("on", gt);
        if (on != std::string::npos) {
            usize e = on;
            while (e < decl.size() &&
                   (std::isalnum((unsigned char)decl[e]) || decl[e] == '_')) ++e;
            const std::string n = decl.substr(on, e - on);
            if (n.size() > 2) names.push_back(n);
        }
        p = semi + 1;
    }

    check(names.size() >= 10, "коллбэки в заголовке найдены");
    if (names.size() < 10) std::printf("       найдено %zu\n", names.size());

    usize unwired = 0;
    std::string firstBad;
    for (const auto& n : names) {
        // Присваивание обработчика: `ui->имя = `.
        if (mainSrc.find("ui->" + n + " =") != std::string::npos) continue;
        ++unwired;
        if (firstBad.empty()) firstBad = n;
    }
    check(unwired == 0, "каждому назначен обработчик в main.cpp");
    if (unwired) std::printf("       первый без обработчика: %s\n",
                             firstBad.c_str());
}

// ------------------------------------------------------------
void testEveryAudioEventIsFired() {
    group("звук: каждое событие кто-то вызывает");

    // AudioEvents — это список того, что игра умеет озвучить.
    // Метод, который написан и ни разу не позван, выглядит в
    // заголовке как работающий звук, а на устройстве его просто нет.
    // Так молчали попадание стрелы, попадание заклинания, замах моба
    // и закрытие окна: четыре события из тридцати одного.
    const std::string hdr =
        readSource("app/src/main/cpp/src/audio/audio_events.h");
    check(!hdr.empty(), "заголовок звуковых событий прочитан");
    if (hdr.empty()) return;

    // Имена методов: строки вида `    void имя(` внутри class.
    std::vector<std::string> names;
    usize p = 0;
    while ((p = hdr.find("void ", p)) != std::string::npos) {
        const usize s0 = p + 5;
        usize e = s0;
        while (e < hdr.size() &&
               (std::isalnum((unsigned char)hdr[e]) || hdr[e] == '_')) ++e;
        if (e < hdr.size() && hdr[e] == '(') {
            const std::string n = hdr.substr(s0, e - s0);
            if (n != "setEngine") names.push_back(n);
        }
        p = e;
    }
    check(names.size() >= 25, "события в заголовке найдены");
    if (names.size() < 25) std::printf("       найдено %zu\n", names.size());

    // Весь код игры, кроме самого модуля звука: там эти имена стоят
    // в определениях, а не в вызовах.
    std::string all;
    collectSources("app/src/main/cpp/src",
                   { "audio_events.h", "audio_events.cpp" }, all);
    check(all.size() > 100000, "исходники игры прочитаны");
    if (all.size() <= 100000) return;

    usize silent = 0;
    std::string firstBad;
    for (const auto& n : names) {
        if (all.find("()." + n + "(") != std::string::npos) continue;
        ++silent;
        if (firstBad.empty()) firstBad = n;
    }
    check(silent == 0, "ни одно не осталось без вызова");
    if (silent) std::printf("       первое без вызова: %s (всего %zu)\n",
                            firstBad.c_str(), silent);
}

// ------------------------------------------------------------
// Убитый житель не возвращается.
//
// Тело убитого npc_ai убирал через три секунды, а спавнер считал
// деревню заселённой, пока жив ХОТЬ ОДИН её житель. Отойти на 260
// метров и вернуться — и деревня стояла в полном составе, вместе с
// убитыми. Сохранение для этого не требовалось: хватало прогулки.
//
// Механизм «убитых» при этом был написан дважды. Спавнер считал
// постоянный ключ u64 и тратил его на цвет рубахи; сохранение
// считало свой, u32, из других полей, записывало — и при загрузке
// выбрасывало: «Пока просто читаем и игнорируем».
// ------------------------------------------------------------
// Кактус сделан из кактуса.
//
// В пустыне стояли деревянные столбики цвета дубовой коры: ствол
// кактуса строился из WOOD с пометкой «временно; в идеале — CACTUS»,
// а блока CACTUS в реестре не было вовсе.
// ------------------------------------------------------------
// Узлы ветки Мудрости и правда работают.
//
// Три из двадцати четырёх узлов Древа считались в computeDerived и
// никуда не доходили: potionPowerMult, craftTierBonus и
// maxResonanceMult не читал никто. Очки в них уходили впустую, а два
// из трёх — обязательные родители для работающих узлов ниже по ветке.
// ------------------------------------------------------------
// Репутацию можно потерять, и это что-то меняет.
//
// Reputation::add звали ровно из одного места — награды за квест, и
// только в плюс. Половина шкалы (Unfriendly, Hostile, Hated) была
// недостижима вовсе, а RelationModifiers::talksToPlayer и ::hostile
// не читал никто: вырезав полдеревни, игрок брал квест у следующего
// жителя и спокойно ходил мимо стражи.
// ------------------------------------------------------------
// Перенос предмета пальцем и правда переносит.
//
// ui::DragDrop был написан целиком — захват, порог перетаскивания,
// деление стека долгим тапом, отмена — и DragDrop::begin() не звал
// никто. drag.active не поднимался ни разу за всю жизнь программы, и
// вместе с модулем без применения стояла половина API инвентаря.
//
// Кадр интерфейса строится целиком на процессоре, поэтому проверка
// гоняет НАСТОЯЩИЙ экран инвентаря: те же прямоугольники ячеек, тот
// же разбор касаний, тот же код перекладки.
// ------------------------------------------------------------
// Эликсиры что-то делают.
//
// Четыре эликсира не восстанавливали ни здоровья, ни маны, ни
// выносливости, а ItemDef::effectDuration — единственное, что у них
// было заполнено, — не читал никто. applyConsumable возвращал
// «эффекта нет», и предмет даже не тратился: четыре предмета в игре
// не делали ровно ничего.
// ------------------------------------------------------------
// Батут летит, ложится и подбрасывает.
//
// Бросок идёт не по дуге, а лучом: место падения ищется взглядом и
// опускается до земли. Дуга выглядела бы красивее, но батут кидают
// ровно туда, куда собираются прыгнуть, — и промах на два блока
// означал бы, что прыжок не состоялся.
// ------------------------------------------------------------
// Рывок уносит вперёд, стоит выносливости и не проходит сквозь стены.
//
// Рывок — это скорость на короткое время, а не перенос позиции.
// Перенос пришлось бы проверять на проходимость самому, и по дороге
// он протаскивал бы сквозь стены; скорость идёт через то же
// разрешение коллизий, что и обычный шаг, и упирается сама.
// ------------------------------------------------------------
// Сюрикен летит и бьёт.
//
// Урон у него не зависит от того, что в руках: сюрикен — не оружие, а
// расходник, и вкладывать в него ни ковку, ни зачарование некуда.
// Летит настильно, без гравитации: на его дистанции дуга была бы
// только помехой прицелу.
// ------------------------------------------------------------
void testShurikenFliesAndHits() {
    group("сюрикен: летит настильно и бьёт");

    world::blocks();
    items::items();

    check(items::items().get(items::ITEM_SHURIKEN).category ==
          items::ItemCategory::Throwable, "сюрикен — метательный предмет");
    check(items::items().get(items::ITEM_SHURIKEN).maxStack >= 16,
          "и носится большой стопкой");

    // Мир настоящий: снаряд на каждом шаге проверяет вокселя, и в
    // несгенерированных чанках он гибнет о «неизвестно» на первом же
    // метре. Игрока с целью ставим высоко над рельефом — там воздух.
    jobs::gJobs.start(2);
    world::ChunkManager world(0x5417, 2);
    bool ready = false;
    for (int i = 0; i < 600 && !ready; ++i) {
        world.update({ 8.f, 70.f, 8.f });
        ready = world.isReadyAt(8, 8) && world.pendingJobs() == 0;
        if (!ready) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    check(ready, "мир построен");
    if (!ready) { jobs::gJobs.stop(); return; }
    const f32 air = (f32)world.generator().surfaceHeight(8, 8) + 20.f;

    ecs::Registry reg;
    player::Player pl;
    pl.init(reg, glm::vec3(8.5f, air, 8.5f));
    const ecs::Entity me = pl.entity();

    auto* inv = pl.inventory();
    check(inv != nullptr, "инвентарь есть");
    if (!inv) { jobs::gJobs.stop(); return; }
    auto& slot = inv->activeSlot();
    slot.itemId = items::ITEM_SHURIKEN;
    slot.count  = 5;

    // ---- Бросок ----
    const items::UseResult r = pl.useItem(
        world, items::INV_HOTBAR_OFFSET + inv->activeHotbar);
    check(r == items::UseResult::Consumed, "сюрикен брошен");
    check(inv->activeSlot().count == 4, "и списан из стопки");
    check(reg.pool<combat::Projectile>().size() == 1,
          "в мире появился ровно один снаряд");

    {
        auto& pool = reg.pool<combat::Projectile>();
        auto* p = pool.get(pool.entityAt(0));
        check(p != nullptr, "снаряд настоящий");
        if (p) {
            check(!p->affectedByGravity, "летит настильно");
            check(p->ownerEntity == (u32)me, "и помнит, кто бросил");
            check(std::fabs(p->damage.amount - items::SHURIKEN_DAMAGE) < 0.01f,
                  "урон — свой собственный, не от оружия в руках");
        }
    }

    // ---- Долетает и бьёт ----
    //
    // Цель ставим по курсу: прицел по умолчанию смотрит на север.
    const ecs::Entity target = reg.create();
    {
        ecs::Transform tf;
        tf.position = pl.eyePosition() + glm::vec3(0.f, -0.9f, -6.f);
        reg.add(target, tf);
        reg.add(target, ecs::Health{ 100.f, 100.f, 0.f, 0.f });
        reg.add(target, ecs::Collider{ glm::vec3(0.5f, 0.9f, 0.5f) });
        // Враждебность снаряд определяет по МЕТКЕ, а не по полю
        // Combatant::faction: Faction::of смотрит PlayerTag/EnemyTag
        // /NPCTag и Kind, и цель без метки для него никто.
        reg.add(target, ecs::EnemyTag{});
        reg.add(target, combat::Combatant{});
        reg.add(target, combat::StatusEffects{});
    }

    for (int i = 0; i < 60; ++i) combat::updateProjectiles(world, reg, 1.f / 60.f);

    auto* hp = reg.get<ecs::Health>(target);
    check(hp != nullptr, "цель на месте");
    if (hp) {
        std::printf("       здоровья у цели осталось %.1f\n", (double)hp->current);
        check(hp->current < 100.f, "сюрикен долетел и ударил");
    }
    check(reg.pool<combat::Projectile>().size() == 0,
          "и на этом снаряд кончился");

    // ---- В того, кто бросил, не попадает ----
    {
        auto* mine = reg.get<ecs::Health>(me);
        check(mine != nullptr, "здоровье игрока есть");
        if (mine) {
            const f32 before = mine->current;
            pl.useItem(world, items::INV_HOTBAR_OFFSET + inv->activeHotbar);
            for (int i = 0; i < 60; ++i)
                combat::updateProjectiles(world, reg, 1.f / 60.f);
            check(mine->current >= before, "бросивший себя не ранит");
        }
    }
    jobs::gJobs.stop();
}

// ------------------------------------------------------------
void testDashMovesForward() {
    group("рывок: уносит вперёд и упирается в стену");

    world::blocks();
    items::items();

    jobs::gJobs.start(2);
    {
        world::ChunkManager world(0xDA54, 2);
        bool ready = false;
        for (int i = 0; i < 600 && !ready; ++i) {
            world.update({ 8.f, 70.f, 8.f });
            ready = world.isReadyAt(8, 8) && world.pendingJobs() == 0;
            if (!ready) std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        check(ready, "мир построен");
        if (!ready) { jobs::gJobs.stop(); return; }

        // Ровная площадка: рельеф не должен решать, докуда дорвёшь.
        const i32 surf = world.generator().surfaceHeight(8, 8) + 4;
        for (i32 z = -4; z <= 24; ++z)
            for (i32 x = -4; x <= 12; ++x) {
                world.setVoxel(x, surf - 1, z, world::STONE);
                for (i32 y = surf; y < surf + 3; ++y)
                    world.setVoxel(x, y, z, world::AIR);
            }

        ecs::Registry reg;
        player::Player pl;
        pl.init(reg, glm::vec3(4.5f, (f32)surf, 4.5f));

        auto* st = reg.get<ecs::Stamina>(pl.entity());
        check(st != nullptr, "выносливость есть");
        if (!st) { jobs::gJobs.stop(); return; }

        auto runFrames = [&](player::PlayerInput in, int n) {
            for (int i = 0; i < n; ++i) {
                pl.update(world, in, 1.f / 60.f, 0.f, 0.f);
                in.dashPressed = false;   // нажатие, а не удержание
            }
        };

        // ---- Рывок вперёд ----
        // При нулевом рыскании «вперёд» — это +Z.
        st->current = st->max;
        const glm::vec3 before = pl.controller.state().position;
        player::PlayerInput dash;
        dash.dashPressed = true;
        runFrames(dash, 20);
        const glm::vec3 after = pl.controller.state().position;

        const f32 gone = after.z - before.z;
        std::printf("       унесло на %.2f блока\n", (double)gone);
        check(gone > 3.f, "рывок уносит больше чем на три блока");
        check(std::fabs(after.x - before.x) < 0.5f, "и ровно вперёд");
        check(st->current < st->max - 1.f, "выносливость потрачена");

        // ---- Откат ----
        //
        // Сперва даём инерции сойти: после рывка игрок ещё катится, и
        // мерить «сдвинулся или нет» прямо сейчас значило бы мерить
        // накат, а не второй рывок.
        for (int i = 0; i < 20; ++i)
            pl.update(world, player::PlayerInput{}, 1.f / 60.f, 0.f, 0.f);
        check(pl.controller.state().dashCooldown > 0.f, "откат ещё идёт");

        const glm::vec3 p2 = pl.controller.state().position;
        st->current = st->max;
        const f32 stBefore = st->current;
        runFrames(dash, 10);
        const f32 again = pl.controller.state().position.z - p2.z;
        check(again < 0.5f, "пока откат не вышел, рывка не происходит");
        check(st->current >= stBefore - 0.5f,
              "и выносливость за несостоявшийся рывок не берут");

        // ---- Без выносливости рывка нет ----
        for (int i = 0; i < 200; ++i) pl.update(world, player::PlayerInput{},
                                                1.f / 60.f, 0.f, 0.f);
        const glm::vec3 p3 = pl.controller.state().position;
        st->current = 1.f;
        runFrames(dash, 20);
        check(pl.controller.state().position.z - p3.z < 1.f,
              "без выносливости рывка нет");
        check(st->current >= 1.f, "и она при этом не списана");

        // ---- В стену не проходит ----
        pl.controller.setPosition(glm::vec3(4.5f, (f32)surf, 4.5f));
        for (i32 y = surf; y < surf + 3; ++y)
            for (i32 x = 0; x <= 12; ++x)
                world.setVoxel(x, y, 7, world::STONE);

        for (int i = 0; i < 200; ++i) pl.update(world, player::PlayerInput{},
                                                1.f / 60.f, 0.f, 0.f);
        st->current = st->max;
        runFrames(dash, 20);
        const f32 z = pl.controller.state().position.z;
        std::printf("       упёрся на z = %.2f\n", (double)z);
        check(z < 7.f, "рывок упирается в стену, а не проходит сквозь");
    }
    jobs::gJobs.stop();
}

// ------------------------------------------------------------
void testTrampolineThrowAndBounce() {
    group("батут: бросается, ложится и подбрасывает");

    world::blocks();
    items::items();

    check(items::items().get(items::ITEM_TRAMPOLINE).category ==
          items::ItemCategory::Throwable, "батут — метательный предмет");

    // ---- Бросать некуда: земли под точкой падения нет ----
    {
        ecs::Registry reg;
        player::Player pl;
        pl.init(reg, glm::vec3(0.f, 64.f, 0.f));
        world::ChunkManager empty(0xE3, 1);   // чанки не строились

        auto* inv = pl.inventory();
        check(inv != nullptr, "инвентарь есть");
        if (!inv) return;
        auto& slot = inv->activeSlot();
        slot.itemId = items::ITEM_TRAMPOLINE;
        slot.count  = 2;

        const items::UseResult r = pl.useItem(
            empty, items::INV_HOTBAR_OFFSET + inv->activeHotbar);
        check(r == items::UseResult::NoEffect, "в пустоту батут не кидается");
        check(inv->activeSlot().count == 2,
              "и из рук при этом не пропадает");
    }

    jobs::gJobs.start(2);
    {
        world::ChunkManager world(0x7A3B, 2);
        bool ready = false;
        for (int i = 0; i < 600 && !ready; ++i) {
            world.update({ 8.f, 70.f, 8.f });
            ready = world.isReadyAt(8, 8) && world.pendingJobs() == 0;
            if (!ready) std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        check(ready, "мир под ногами построен");

        if (ready) {
            const i32 surf = world.generator().surfaceHeight(8, 8);

            ecs::Registry reg;
            player::Player pl;
            pl.init(reg, glm::vec3(8.5f, (f32)surf, 8.5f));
            auto* inv = pl.inventory();
            auto& slot = inv->activeSlot();
            slot.itemId = items::ITEM_TRAMPOLINE;
            slot.count  = 2;

            // ---- Бросок ----
            const items::UseResult r = pl.useItem(
                world, items::INV_HOTBAR_OFFSET + inv->activeHotbar);
            check(r == items::UseResult::Consumed, "батут брошен");
            check(inv->activeSlot().count == 1, "и списан из пояса");

            auto& pool = reg.pool<items::Trampoline>();
            check(pool.size() == 1, "в мире появился ровно один батут");
            if (pool.size() != 1) { jobs::gJobs.stop(); return; }

            const ecs::Entity te = pool.entityAt(0);
            auto* ttf = reg.get<ecs::Transform>(te);
            check(ttf != nullptr, "у него есть место в мире");
            if (!ttf) { jobs::gJobs.stop(); return; }

            // Лёг на землю, а не завис и не утонул в камне.
            world::VoxelReader rd(world);
            const i32 bx = (i32)std::floor(ttf->position.x);
            const i32 by = (i32)std::floor(ttf->position.y);
            const i32 bz = (i32)std::floor(ttf->position.z);
            check(rd.at(bx, by, bz) == world::AIR, "сам он в воздухе");
            check(world::blocks().isSolid(rd.at(bx, by - 1, bz)),
                  "а под ним твёрдый блок");

            // И лёг ВПЕРЕДИ: прицел по умолчанию смотрит на север.
            check(ttf->position.z < 8.5f - 1.f,
                  "лёг перед игроком, а не под ноги");

            // ---- Подброс ----
            const f32 up = items::trampolineBounceAt(
                reg, ttf->position, -6.f);
            check(up > 5.f, "падающего он подбрасывает");

            // Сразу второй раз — нет: иначе батут ловит игрока на
            // первом же кадре подъёма.
            check(items::trampolineBounceAt(reg, ttf->position, -6.f) == 0.f,
                  "и не подбрасывает второй раз подряд");

            // Летящего вверх не трогает вовсе.
            items::updateTrampolines(reg, 1.f);
            check(items::trampolineBounceAt(reg, ttf->position, 6.f) == 0.f,
                  "летящего вверх не трогает");

            // Мимо площадки — тоже нет.
            check(items::trampolineBounceAt(
                      reg, ttf->position + glm::vec3(3.f, 0.f, 0.f), -6.f) == 0.f,
                  "и в стороне от площадки не срабатывает");

            // ---- Через весь путь игрока ----
            pl.controller.setPosition(ttf->position + glm::vec3(0.f, 0.2f, 0.f));
            pl.controller.state().velocity.y = -4.f;
            pl.update(world, player::PlayerInput{}, 1.f / 60.f, 0.f, 0.f);
            check(pl.controller.state().velocity.y > 5.f,
                  "наступивший улетает вверх");

            // ---- Не лежит вечно ----
            for (int i = 0; i < 200; ++i) items::updateTrampolines(reg, 1.f);
            check(reg.pool<items::Trampoline>().size() == 0,
                  "со временем батут исчезает");
        }
    }
    jobs::gJobs.stop();
}

// ------------------------------------------------------------
void testElixirsGrantTimedBuff() {
    group("эликсиры: временная прибавка к атрибуту");

    world::blocks();
    items::items();

    ecs::Registry reg;
    player::Player pl;
    pl.init(reg, glm::vec3(0.f, 64.f, 0.f));
    const ecs::Entity e = pl.entity();

    progression::tickProgression(reg, 0.001f);
    auto* prog = reg.get<progression::Progression>(e);
    check(prog != nullptr, "прогрессия есть");
    if (!prog) return;
    const f32 baseHealth = prog->derived.maxHealth;
    check(baseHealth > 0.f, "здоровье до эликсира известно");

    auto* inv = pl.inventory();
    check(inv != nullptr, "инвентарь есть");
    if (!inv) return;
    auto& slot = inv->activeSlot();
    slot.itemId = items::ITEM_ELIXIR_ENDURANCE;
    slot.count  = 2;

    const items::UseResult r = items::useItemFromSlot(
        reg, e, items::INV_HOTBAR_OFFSET + inv->activeHotbar);
    check(r == items::UseResult::Consumed, "эликсир выпит, а не отвергнут");
    check(inv->activeSlot().count == 1, "и списан со стопки");

    auto* buffs = reg.get<progression::AttributeBuffs>(e);
    check(buffs != nullptr, "прибавка записана");
    if (!buffs) return;
    check(buffs->add[3] == 5, "стойкость поднята на пять");
    check(buffs->timeLeft[3] > 59.f, "на минуту");

    progression::tickProgression(reg, 0.001f);
    const f32 buffedHealth = prog->derived.maxHealth;
    check(buffedHealth > baseHealth, "и здоровье выросло вместе с ней");

    // Базовый атрибут при этом НЕ тронут: он сохраняется, и эликсир
    // не должен оседать в сейве прибавкой навсегда.
    auto* attr = reg.get<ecs::Attributes>(e);
    check(attr && attr->endurance == 10, "базовая стойкость не тронута");

    // ---- Минута прошла — всё вернулось ----
    for (int i = 0; i < 61 * 60; ++i) progression::tickProgression(reg, 1.f / 60.f);
    check(!buffs->any(), "действие кончилось");
    check(std::fabs(prog->derived.maxHealth - baseHealth) < 0.01f,
          "и здоровье вернулось к прежнему");

    // ---- Второй эликсир продлевает, а не складывается ----
    items::useItemFromSlot(reg, e, items::INV_HOTBAR_OFFSET + inv->activeHotbar);
    progression::tickProgression(reg, 1.f / 60.f);
    check(buffs->add[3] == 5, "одна прибавка");
    for (int i = 0; i < 30 * 60; ++i) progression::tickProgression(reg, 1.f / 60.f);
    const f32 leftAfter30 = buffs->timeLeft[3];
    slot.itemId = items::ITEM_ELIXIR_ENDURANCE;
    slot.count  = 1;
    items::useItemFromSlot(reg, e, items::INV_HOTBAR_OFFSET + inv->activeHotbar);
    check(buffs->add[3] == 5, "и после второго эликсира она всё та же");
    check(buffs->timeLeft[3] > leftAfter30 + 25.f, "но время продлилось");
}

// ------------------------------------------------------------
void testInventoryDragMovesItems() {
    group("инвентарь: предмет переносится пальцем");

    world::blocks();
    items::items();

    constexpr i32 W = 1600, H = 900, DPI = 400;

    ecs::Registry reg;
    player::Player pl;
    pl.init(reg, glm::vec3(0.f, 64.f, 0.f));
    world::ChunkManager world(0xD2A6, 1);

    auto* inv = pl.inventory();
    check(inv != nullptr, "инвентарь есть");
    if (!inv) return;

    ui::UiSystem sys;
    sys.setDensityDpi(DPI);
    sys.setScreenSize(W, H);
    sys.screen = ui::Screen::Inventory;

    // Та же раскладка, по которой экран и рисует: не копия её правил,
    // а она сама.
    const ui::HudLayout L((f32)W, (f32)H,
                          ui::theme::Metrics::fromDensityDpi(
                              DPI, config::settingsConst().uiScale),
                          ui::SafeInsets{});
    const ui::HudLayout::CellGrid mainGrid =
        L.cellGrid(L.invLeft(), items::INV_MAIN_SLOTS);

    auto centre = [&](u32 slot) {
        const ui::Rect r = mainGrid.at(slot);
        return glm::vec2{ r.x + r.w * 0.5f, r.y + r.h * 0.5f };
    };
    auto frame = [&]() {
        sys.tickUi(1.f / 60.f);
        sys.buildFrame(pl, world, 60.f);
    };
    auto total = [&]() {
        u32 n = 0;
        for (u32 i = 0; i < items::INV_TOTAL_SLOTS; ++i) n += inv->at(i).count;
        return n;
    };

    // ---- Перенос целого стека в пустую ячейку ----
    inv->at(0).itemId = items::ITEM_STONE;
    inv->at(0).count  = 7;
    const u32 before = total();

    frame();
    const glm::vec2 from = centre(0), to = centre(3);
    check(sys.routeTouch(1, from.x, from.y, 0), "нажатие попало в ячейку");
    frame();
    check(!sys.drag.active, "одно нажатие переноса ещё не начинает");

    // Сдвиг дальше порога — и перенос пошёл.
    sys.routeTouch(1, from.x + ui::DragDrop::DRAG_THRESHOLD + 4.f, from.y, 2);
    frame();
    check(sys.drag.active, "сдвиг дальше порога начинает перенос");
    check(sys.drag.fromSlot == 0, "и помнит, откуда несут");
    check(sys.drag.stack.count == 7, "несут весь стек");
    check(inv->at(0).count == 7,
          "а из сумки предмет не вынут: выход из экрана его не потеряет");

    sys.routeTouch(1, to.x, to.y, 2);
    frame();
    sys.routeTouch(1, to.x, to.y, 1);
    frame();
    check(!sys.drag.active, "отпускание завершает перенос");
    check(inv->at(0).empty(), "источник опустел");
    check(inv->at(3).itemId == items::ITEM_STONE && inv->at(3).count == 7,
          "а стек целиком лёг в цель");
    check(total() == before, "ничего не размножилось и не пропало");

    // ---- Долгий тап делит стек пополам ----
    frame();
    const glm::vec2 h0 = centre(3);
    check(sys.routeTouch(1, h0.x, h0.y, 0), "нажатие на стек принято");
    for (int i = 0; i < 40 && !sys.drag.active; ++i) frame();
    check(sys.drag.active, "долгий тап на месте начинает перенос");
    check(sys.drag.isSplit, "и это перенос половины");
    check(sys.drag.stack.count == 3, "семь делится на три и четыре");

    const glm::vec2 h1 = centre(5);
    sys.routeTouch(1, h1.x, h1.y, 2);
    frame();
    sys.routeTouch(1, h1.x, h1.y, 1);
    frame();
    check(inv->at(3).count == 4, "в источнике остался остаток");
    check(inv->at(5).count == 3, "а половина уехала");
    check(total() == before, "и сумма опять сошлась");

    // ---- Отпускание мимо ячеек ничего не теряет ----
    frame();
    const glm::vec2 g0 = centre(3);
    sys.routeTouch(1, g0.x, g0.y, 0);
    frame();
    sys.routeTouch(1, g0.x + ui::DragDrop::DRAG_THRESHOLD + 4.f, g0.y, 2);
    frame();
    check(sys.drag.active, "перенос начат");
    sys.routeTouch(1, (f32)W - 2.f, (f32)H - 2.f, 2);
    frame();
    sys.routeTouch(1, (f32)W - 2.f, (f32)H - 2.f, 1);
    frame();
    check(!sys.drag.active, "перенос завершён");
    check(inv->at(3).count == 4, "предмет остался на месте");
    check(total() == before, "и сумма цела");

    // ---- Несовместимое в занятую ячейку меняется местами ----
    inv->at(7).itemId = items::ITEM_DIRT;
    inv->at(7).count  = 2;
    const u32 before2 = total();

    frame();
    const glm::vec2 s0 = centre(3), s1 = centre(7);
    sys.routeTouch(1, s0.x, s0.y, 0);
    frame();
    sys.routeTouch(1, s0.x + ui::DragDrop::DRAG_THRESHOLD + 4.f, s0.y, 2);
    frame();
    sys.routeTouch(1, s1.x, s1.y, 2);
    frame();
    sys.routeTouch(1, s1.x, s1.y, 1);
    frame();
    check(inv->at(3).itemId == items::ITEM_DIRT && inv->at(3).count == 2,
          "ячейки поменялись местами: в источнике то, что лежало в цели");
    check(inv->at(7).itemId == items::ITEM_STONE && inv->at(7).count == 4,
          "а в цели — то, что несли");
    check(total() == before2, "и сумма цела");
}

// ------------------------------------------------------------
void testReputationCanBeLost() {
    group("репутация: за убийство приходит счёт");

    world::blocks();
    items::items();
    npc::npcRegistry();

    world::ChunkManager world(0x5EED, 1);

    ecs::Registry reg;
    player::Player pl;
    pl.init(reg, glm::vec3(0.f, 64.f, 0.f));
    const ecs::Entity player = pl.entity();

    auto* rep = reg.get<factions::Reputation>(player);
    check(rep != nullptr, "репутация у игрока есть");
    if (!rep) return;
    check(rep->tier(factions::FactionId::Villagers) ==
          factions::ReputationTier::Neutral, "и начинается с нейтралитета");

    auto makeNpc = [&](u16 typeId, const glm::vec3& at) {
        const ecs::Entity e = reg.create();
        ecs::Transform tf; tf.position = at;
        reg.add(e, tf);
        reg.add(e, ecs::Velocity{});
        const auto& def = npc::npcRegistry().get(typeId);
        reg.add(e, ecs::Health{ def.maxHealth, def.maxHealth, 0.f, 0.f });
        reg.add(e, ecs::Kind{ ecs::EntityKind::NPC });
        reg.add(e, ecs::NPCTag{});
        combat::Combatant cmb;
        cmb.faction = combat::Faction::NPC;
        reg.add(e, cmb);
        reg.add(e, combat::StatusEffects{});
        npc::NpcTag tag; tag.id = typeId; tag.persistKey = 1000 + (u64)typeId;
        reg.add(e, tag);
        npc::NpcAI ai; ai.homePos = at; ai.state = npc::NpcAI::Idle;
        reg.add(e, ai);
        return e;
    };

    auto murder = [&]() {
        const ecs::Entity v = makeNpc(npc::NPC_VILLAGER, glm::vec3(2.f, 64.f, 0.f));
        combat::DamageInstance dmg{};
        dmg.amount       = 9999.f;
        dmg.type         = combat::DamageType::Physical;
        dmg.sourceEntity = (u32)player;
        combat::applyDamage(reg, v, dmg);
    };

    // ---- Одно убийство: цена известна и тир поехал вниз ----
    murder();
    check(rep->get(factions::FactionId::Villagers) == -factions::REP_MURDER_PENALTY,
          "убитый житель стоит ровно объявленной цены");
    check(rep->tier(factions::FactionId::Villagers) ==
          factions::ReputationTier::Unfriendly,
          "и одного убийства хватает, чтобы перестать быть своим");

    // ---- Четыре: с врагом деревни не разговаривают ----
    for (int i = 0; i < 3; ++i) murder();
    check(rep->tier(factions::FactionId::Villagers) ==
          factions::ReputationTier::Hostile, "четыре убийства — вражда");

    {
        const ecs::Entity witness =
            makeNpc(npc::NPC_VILLAGER, glm::vec3(1.f, 64.f, 0.f));
        const auto& def = npc::npcRegistry().get(npc::NPC_VILLAGER);
        check(!npc::startDialogue(reg, world, (u32)player, (u32)witness,
                                  def.dialogueRoot),
              "враг деревни не может заговорить с жителем");
        reg.destroy(witness);
    }

    // ---- Семь: стража бьёт ----
    for (int i = 0; i < 3; ++i) murder();
    check(rep->tier(factions::FactionId::Villagers) ==
          factions::ReputationTier::Hated, "семь убийств — ненависть");

    {
        const glm::vec3 guardPos{ 3.f, 64.f, 0.f };
        const ecs::Entity guard = makeNpc(npc::NPC_GUARD, guardPos);
        auto* gai = reg.get<npc::NpcAI>(guard);
        check(gai != nullptr, "стражник создан");
        if (gai) {
            // Патрулирование: в этой ветке стража осматривается без
            // проверки прямой видимости, и пустой мир ей не мешает.
            gai->state = npc::NpcAI::Wander;
            gai->wanderTarget = guardPos + glm::vec3(20.f, 0.f, 0.f);
            gai->stateTime = 0.f;

            npc::updateNpcs(world, reg, player,
                            pl.controller.state().position, 1.f / 60.f);

            check(gai->state == npc::NpcAI::Combat,
                  "стража переходит в бой с ненавистным");
            check(gai->guardTarget == (u32)player,
                  "и целью выбирает именно игрока");
        }
    }

    // ---- Игрок узнаёт о случившемся ----
    //
    // reputationFlashActive не поднимал никто: экран смены тира был
    // написан целиком и не показывался ни разу.
    pl.update(world, player::PlayerInput{}, 1.f / 60.f, 0.f, 0.f);
    check(pl.reputationFlashActive, "смена тира показывается на экране");
    check(pl.lastRepFaction == factions::FactionId::Villagers,
          "и названа та фракция, у которой она произошла");
    check(pl.lastRepTier == factions::ReputationTier::Hated,
          "и тот тир, до которого докатились");
}

// ------------------------------------------------------------
void testWisdomNodesActuallyWork() {
    group("древо: узлы Мудрости доходят до игры");

    world::blocks();
    items::items();

    // ---- Alchemist: зелье лечит сильнее ----
    auto healWith = [](u8 rank) -> f32 {
        ecs::Registry reg;
        player::Player pl;
        pl.init(reg, glm::vec3(0.f, 64.f, 0.f));
        const ecs::Entity e = pl.entity();

        if (auto* tree = reg.get<progression::SkillTree>(e))
            tree->ranks[(u16)progression::SkillNodeId::Wis_Alchemist] = rank;
        if (auto* prog = reg.get<progression::Progression>(e))
            prog->derivedDirty = true;
        progression::tickProgression(reg, 0.001f);

        auto* h = reg.get<ecs::Health>(e);
        if (!h) return 0.f;
        h->current = 1.f;
        const f32 before = h->current;

        auto* inv = pl.inventory();
        if (!inv) return 0.f;
        auto& slot = inv->activeSlot();
        slot.itemId = items::ITEM_POTION_HEALTH_SMALL;
        slot.count  = 1;
        items::useItemFromSlot(reg, e,
                               items::INV_HOTBAR_OFFSET + inv->activeHotbar);
        return h->current - before;
    };

    const f32 plain = healWith(0);
    const f32 maxed = healWith(2);
    check(plain > 0.f, "зелье лечит и без алхимии");
    // Два ранга по +20% — ровно 1.4 от базового.
    check(std::fabs(maxed - plain * 1.4f) < 0.01f,
          "а с двумя рангами Alchemist — на 40% больше");

    // ---- Craft Master: рецепт берётся на уровень раньше ----
    {
        crafting::Recipe r{};
        r.id = 1;
        r.requiredLevel = 5;
        r.station = crafting::StationType::None;
        r.output.itemId = items::ITEM_STONE;
        r.output.count  = 1;

        items::Inventory inv{};
        crafting::CraftContext ctx{};
        ctx.inventory   = &inv;
        ctx.playerLevel = 4;

        check(crafting::canCraft(ctx, r) == crafting::CraftStatus::LevelTooLow,
              "на четвёртом уровне рецепт пятого недоступен");
        ctx.craftTierBonus = 1;
        check(crafting::canCraft(ctx, r) != crafting::CraftStatus::LevelTooLow,
              "а мастеру крафта — доступен");
    }

    // ---- Resonance Master: запас выше, ступени на месте ----
    {
        ecs::Registry reg;
        player::Player pl;
        pl.init(reg, glm::vec3(0.f, 64.f, 0.f));
        const ecs::Entity e = pl.entity();

        auto* res = reg.get<combat::ResonanceState>(e);
        check(res != nullptr, "резонанс у игрока есть");
        if (!res) return;
        check(std::fabs(res->maxValue - combat::RESONANCE_MAX) < 0.01f,
              "без узла потолок обычный");

        if (auto* tree = reg.get<progression::SkillTree>(e))
            tree->ranks[(u16)progression::SkillNodeId::Wis_ResonanceMaster] = 3;
        if (auto* prog = reg.get<progression::Progression>(e))
            prog->derivedDirty = true;
        progression::tickProgression(reg, 0.001f);

        res = reg.get<combat::ResonanceState>(e);
        check(std::fabs(res->maxValue - combat::RESONANCE_MAX * 1.45f) < 0.01f,
              "с тремя рангами — на 45% выше");

        // Копим до упора: запас набирается СВЕРХ пятой ступени, а сама
        // ступень остаётся на прежней отметке. Иначе «+15% максимума»
        // было бы ослаблением — до пятой пришлось бы бить дольше.
        for (int i = 0; i < 40; ++i) res->onHit(false);
        check(res->value > combat::RESONANCE_MAX + 1.f,
              "накопить можно выше прежнего потолка");
        check(res->value <= res->maxValue + 0.01f, "но не выше своего");
        check(res->stack == combat::RESONANCE_MAX_STACKS,
              "и пятая ступень всё равно достигается");

        // Запас утекает первым — ступень держится дольше обычного.
        f32 held = 0.f;
        for (int i = 0; i < 1000 && res->stack == combat::RESONANCE_MAX_STACKS; ++i) {
            res->update(1.f / 60.f);
            held += 1.f / 60.f;
        }
        const f32 decayDelay = combat::RESONANCE_DECAY_DELAY;
        check(held > decayDelay + 0.5f,
              "и держится дольше, чем просто задержка распада");
    }
}

// ------------------------------------------------------------
void testDesertGrowsCactus() {
    group("мир: кактус сделан из кактуса");

    world::blocks();
    items::items();

    check(world::blocks().get(world::CACTUS).isSolid,
          "блок кактуса есть в реестре и он твёрдый");
    check(items::items().blockToItem(world::CACTUS) == items::ITEM_CACTUS,
          "и у него есть предмет: сломанный кактус не пропадает");

    // Чанк строится целиком и на месте, той же функцией, что и в игре:
    // планировщик задач для этого не нужен, а результат детерминирован
    // по (seed, координате чанка).
    //
    // Кактусов приходится искать долго: treeDensity у пустыни 0.05,
    // то есть один кактус на двадцать чанков, и биом берётся по ЦЕНТРУ
    // чанка. Полтора десятка пустынных чанков ничего не докажут —
    // ноль там законен. Поэтому обход идёт по сотням.
    constexpr u64 SEED = 0xCAC705ULL;
    world::ChunkManager mgr(SEED, 1);
    const auto& gen = mgr.generator();

    constexpr usize WANT_CHUNKS = 80;
    usize desertChunks = 0, withCactus = 0, cactusVoxels = 0, woodVoxels = 0;
    std::vector<world::TerrainGenerator::Column> cols;

    for (i32 cz = -64; cz <= 64 && desertChunks < WANT_CHUNKS; ++cz) {
        for (i32 cx = -64; cx <= 64 && desertChunks < WANT_CHUNKS; ++cx) {
            const i32 midX = cx * world::CHUNK_SIZE + world::CHUNK_SIZE / 2;
            const i32 midZ = cz * world::CHUNK_SIZE + world::CHUNK_SIZE / 2;
            if (gen.biomeAt(midX, midZ) != world::Desert) continue;
            ++desertChunks;

            auto chunk = std::make_unique<world::Chunk>();
            chunk->coord = { cx, 0, cz };
            world::computeChunkColumns(gen, cx, cz, cols);
            world::generateChunkVoxels(*chunk, gen, cols.data(), SEED);

            usize here = 0;
            for (i32 lz = 0; lz < world::CHUNK_SIZE; ++lz)
                for (i32 lx = 0; lx < world::CHUNK_SIZE; ++lx)
                    for (i32 y = 0; y < world::CHUNK_SIZE_Y; ++y) {
                        const u16 b = chunk->voxels[world::chunkIndex(lx, y, lz)];
                        if (b == world::CACTUS) { ++here; ++cactusVoxels; }
                        if (b == world::WOOD)   ++woodVoxels;
                    }
            if (here) ++withCactus;
        }
    }

    std::printf("       пустынных чанков %zu, с кактусом %zu, вокселей кактуса %zu\n",
                desertChunks, withCactus, cactusVoxels);
    check(desertChunks == WANT_CHUNKS, "пустынные чанки нашлись");
    // Ровно эта проверка отличает «кактус есть» от «кактус из дуба»:
    // до правки ствол ставился блоком WOOD, и CACTUS в мире не
    // появлялся ни разу.
    check(cactusVoxels > 0, "в пустыне вырос кактус");
    check(withCactus >= 2, "и не в одном-единственном чанке на весь обход");
    // Дерево в пустыне встречается и законно — деревни строятся из
    // него в любом биоме, — поэтому «дерева нет» проверять нельзя.
    (void)woodVoxels;
}

// ------------------------------------------------------------
void testDeadNpcStaysDead() {
    group("NPC: убитый не возвращается");

    world::blocks();
    items::items();
    npc::npcRegistry();

    constexpr u64 SEED = 4242;
    world::ChunkManager mgr(SEED, 1);

    // Та же деревня, что и в проверке домов, и найденная тем же
    // способом — через общую раскладку, а не копию её правил.
    world::VillageSite site;
    for (i32 r = 0; r < 12 && !site.exists; ++r)
        for (i32 a = -r; a <= r && !site.exists; ++a)
            for (i32 b = -r; b <= r && !site.exists; ++b) {
                if (std::max(std::abs(a), std::abs(b)) != r) continue;
                const auto s2 = world::villageAt(a, b, SEED, &mgr.generator());
                if (s2.exists) site = s2;
            }
    if (!site.exists) { check(false, "деревня не нашлась"); return; }

    const glm::vec3 home{
        (f32)site.center.x,
        (f32)mgr.generator().surfaceHeight(site.center.x, site.center.z),
        (f32)site.center.z };

    ecs::Registry reg;
    npc::NpcSpawner spawner;

    auto pump = [&](const glm::vec3& pos, int frames) {
        for (int i = 0; i < frames; ++i) spawner.update(mgr, reg, pos, SEED);
    };
    auto liveKeys = [&](ecs::Registry& r) {
        std::set<u64> out;
        auto& pool = r.pool<npc::NpcAI>();
        for (usize i = 0; i < pool.size(); ++i) {
            auto* tag = r.get<npc::NpcTag>(pool.entityAt((u32)i));
            if (tag && tag->persistKey) out.insert(tag->persistKey);
        }
        return out;
    };

    pump(home, 60);
    const std::set<u64> born = liveKeys(reg);
    check(born.size() >= 5, "жители деревни вселились");
    check(born.size() == reg.pool<npc::NpcAI>().size(),
          "и у каждого свой постоянный ключ, а не общий");
    if (born.empty()) return;

    // ---- Убиваем одного ----
    const u64 victim = *born.begin();
    {
        auto& pool = reg.pool<npc::NpcAI>();
        for (usize i = 0; i < pool.size(); ++i) {
            ecs::Entity e = pool.entityAt((u32)i);
            auto* tag = reg.get<npc::NpcTag>(e);
            if (!tag || tag->persistKey != victim) continue;
            if (auto* hp = reg.get<ecs::Health>(e)) hp->current = 0.f;
            if (auto* ai = pool.get(e)) ai->state = npc::NpcAI::Dead;
            break;
        }
    }
    pump(home, 1);
    check(spawner.isDead(victim), "спавнер запомнил убитого");
    check(reg.pool<npc::NpcAI>().size() == born.size(),
          "и оставил тело дожить свою анимацию, а не убрал сразу");

    // Тело убирает npc_ai через три секунды — здесь за него.
    {
        auto& pool = reg.pool<npc::NpcAI>();
        for (usize i = 0; i < pool.size(); ++i) {
            ecs::Entity e = pool.entityAt((u32)i);
            auto* tag = reg.get<npc::NpcTag>(e);
            if (tag && tag->persistKey == victim) { reg.destroy(e); break; }
        }
    }

    // ---- Прогулка на 1400 метров и обратно ----
    pump(home + glm::vec3(1000.f, 0.f, 1000.f), 200);
    {
        const std::set<u64> away = liveKeys(reg);
        usize left = 0;
        for (u64 k : born) if (away.count(k)) ++left;
        check(left == 0, "пока игрок далеко, деревня опустела");
    }

    // Возвращаемся. Смотрим не только итог, но и каждый кадр:
    // отказ вселять убитого и уборка по загруженному списку — два
    // разных места, и достаточно снять первое, чтобы убитый на один
    // кадр появлялся снова — со своим прилавком, кошельком и всем
    // остальным, что заводится при вселении.
    bool victimEverSeen = false;
    for (int i = 0; i < 120; ++i) {
        spawner.update(mgr, reg, home, SEED);
        if (liveKeys(reg).count(victim)) victimEverSeen = true;
    }
    check(!victimEverSeen, "убитый не вселился ни на один кадр");
    {
        const std::set<u64> back = liveKeys(reg);
        check(back.count(victim) == 0, "убитый не вернулся");
        usize missing = 0;
        for (u64 k : born) if (k != victim && !back.count(k)) ++missing;
        check(missing == 0, "а все остальные вернулись");
    }

    // ---- Список переживает сохранение ----
    {
        save::ByteWriter w;
        save::serializeNpcState(w, spawner);
        save::ByteReader r(w.data());
        npc::NpcSpawner restored;
        check(save::deserializeNpcState(r, restored),
              "список убитых записан и прочитан");
        check(restored.isDead(victim),
              "и убитый остался убитым после загрузки");
        check(!restored.isDead(victim ^ 1ull),
              "а посторонний ключ в список не попал");
    }

    // ---- Загруженный список выгоняет уже стоящего жителя ----
    //
    // Загрузку делают, стоя посреди деревни: те, кого убили до
    // сохранения, уже вселены. Список должен убрать их сразу, а не
    // ждать, пока игрок отойдёт на 260 метров.
    {
        ecs::Registry reg2;
        npc::NpcSpawner sp2;
        for (int i = 0; i < 60; ++i) sp2.update(mgr, reg2, home, SEED);
        const std::set<u64> live = liveKeys(reg2);
        check(live.count(victim) == 1, "до загрузки житель на месте");

        sp2.setDeadKeys({ victim });
        sp2.update(mgr, reg2, home, SEED);
        const std::set<u64> after = liveKeys(reg2);
        check(after.count(victim) == 0, "после загрузки его убрали");
        check(after.size() == live.size() - 1,
              "и убрали ровно его одного");
    }
}

// ------------------------------------------------------------
void testVillageHousesAreBuildings() {
    group("деревня: дом построен, а не насыпан");

    world::blocks();
    constexpr u64 SEED = 4242;
    world::ChunkManager mgr(SEED, 1);

    // Находим настоящую деревню — ту же, что найдёт спавнер NPC.
    world::VillageSite site;
    i32 sx = 0, sz = 0;
    for (i32 r = 0; r < 12 && !site.exists; ++r)
        for (i32 a = -r; a <= r && !site.exists; ++a)
            for (i32 b = -r; b <= r && !site.exists; ++b) {
                if (std::max(std::abs(a), std::abs(b)) != r) continue;
                const auto s2 = world::villageAt(a, b, SEED, &mgr.generator());
                if (s2.exists) { site = s2; sx = a; sz = b; }
            }
    if (!site.exists) { check(false, "деревня не нашлась ни в одном супер-чанке"); return; }
    check(true, "деревня найдена по общей раскладке");
    // И ВСЕ деревни на суше, а не только найденная.
    //
    // Размещение структур смотрит только на хэш и про рельеф не знает
    // ничего, поэтому деревни исправно вырастали в океане: дома по
    // колено в воде, дорожки на дне, жители посреди моря. Проверять
    // одну найденную мало — она-то как раз сухая.
    {
        int villages = 0, drowned = 0;
        // Диапазон широкий намеренно: деревня на пятачке суши
        // посреди залива — случай редкий, и на двух десятках
        // супер-чанков он просто не встречается.
        for (i32 a = -25; a <= 25; ++a)
            for (i32 b = -25; b <= 25; ++b) {
                const auto v = world::villageAt(a, b, SEED, &mgr.generator());
                if (!v.exists) continue;
                ++villages;
                const i32 minLand = world::TerrainGenerator::SEA_LEVEL + 2;
                if (mgr.generator().surfaceHeight(v.center.x, v.center.z) < minLand) {
                    ++drowned;
                    continue;
                }
                // Мало сухого колодца: дома стоят на кольце радиусом
                // до 36, и деревня на пятачке суши посреди залива —
                // это всё та же деревня в море.
                int wet = 0;
                const i32 probe[8][2] = {
                    { 36, 0 }, { -36, 0 }, { 0, 36 }, { 0, -36 },
                    { 26, 26 }, { 26, -26 }, { -26, 26 }, { -26, -26 },
                };
                for (const auto& q : probe)
                    if (mgr.generator().surfaceHeight(v.center.x + q[0],
                                                      v.center.z + q[1]) < minLand)
                        ++wet;
                if (wet > 2) ++drowned;
            }
        char vm[140];
        std::snprintf(vm, sizeof(vm), "деревень в округе %d, из них в воде %d",
                      villages, drowned);
        check(villages > 5, vm);
        check(drowned == 0, "ни одна деревня не стоит в море");
    }
    (void)sx; (void)sz;

    // Радиус осмотра. Дома стоят на кольце радиусом до 35, плюс
    // половина размера дома и блок свеса кровли: окна в ±40 не
    // хватало, и до проверки дверей доходил один дом из десяти.
    constexpr i32 VR = 56;

    // Генерируем чанки вокруг колодца с запасом на целые чанки.
    const i32 c0x = (i32)std::floor((f32)(site.center.x - VR) / world::CHUNK_SIZE) - 1;
    const i32 c1x = (i32)std::floor((f32)(site.center.x + VR) / world::CHUNK_SIZE) + 1;
    const i32 c0z = (i32)std::floor((f32)(site.center.z - VR) / world::CHUNK_SIZE) - 1;
    const i32 c1z = (i32)std::floor((f32)(site.center.z + VR) / world::CHUNK_SIZE) + 1;

    struct Col { i32 cx, cz; std::unique_ptr<world::Chunk> ch; };
    std::vector<Col> world_;
    std::vector<world::TerrainGenerator::Column> cols;
    for (i32 cx = c0x; cx <= c1x; ++cx)
        for (i32 cz = c0z; cz <= c1z; ++cz) {
            auto ch = std::make_unique<world::Chunk>();
            ch->coord = { cx, 0, cz };
            world::computeChunkColumns(mgr.generator(), cx, cz, cols);
            world::generateChunkVoxels(*ch, mgr.generator(), cols.data(), SEED);
            world_.push_back({ cx, cz, std::move(ch) });
        }

    auto blockAt = [&](i32 wx, i32 wy, i32 wz) -> u16 {
        const i32 cx = (i32)std::floor((f32)wx / world::CHUNK_SIZE);
        const i32 cz = (i32)std::floor((f32)wz / world::CHUNK_SIZE);
        for (const auto& e : world_)
            if (e.cx == cx && e.cz == cz) {
                const i32 lx = wx - cx * world::CHUNK_SIZE;
                const i32 lz = wz - cz * world::CHUNK_SIZE;
                if (!e.ch->inBounds(lx, wy, lz)) return world::AIR;
                return e.ch->at(lx, wy, lz);
            }
        return world::UNKNOWN;
    };

    // ---- 1. Материалы дома ----
    int thatch = 0, plank = 0, glass = 0, lantern = 0, lavaInVillage = 0;
    i32 yLo = 200, yHi = 0;
    for (i32 wx = site.center.x - VR; wx <= site.center.x + VR; ++wx)
        for (i32 wz = site.center.z - VR; wz <= site.center.z + VR; ++wz)
            for (i32 y = 0; y < world::CHUNK_SIZE_Y; ++y) {
                const u16 b = blockAt(wx, y, wz);
                if (b == world::THATCH)  { ++thatch;  yLo = std::min(yLo, y); yHi = std::max(yHi, y); }
                else if (b == world::PLANK)   ++plank;
                else if (b == world::GLASS)   ++glass;
                else if (b == world::LANTERN) ++lantern;
                else if (b == world::LAVA) {
                    // Природная лава рядом с деревней — дело мира.
                    // Лава В ДОМЕ — та, у которой рядом стена из
                    // доски: именно она и лежала вместо очага.
                    bool indoors = false;
                    for (i32 ox = -3; ox <= 3 && !indoors; ++ox)
                        for (i32 oz = -3; oz <= 3 && !indoors; ++oz)
                            if (blockAt(wx + ox, y, wz + oz) == world::PLANK)
                                indoors = true;
                    if (indoors) ++lavaInVillage;
                }
            }

    {
        char dm[200];
        std::snprintf(dm, sizeof(dm),
                      "деревня разобрана: доска %d, солома %d, стекло %d, фонарей %d",
                      plank, thatch, glass, lantern);
        check(true, dm);
    }
    check(plank   > 0, "стены дома из доски");
    check(thatch  > 0, "кровля соломенная");
    check(glass   > 0, "в домах есть окна");
    check(lantern > 0, "и фонарь вместо лужи лавы");
    check(lavaInVillage == 0, "лавы в домах нет");

    // ---- 2. Кровля двускатная, а не плоская ----
    //
    // Считать солому по уровням НЕЛЬЗЯ: дома стоят на разной высоте,
    // и уровни разных домов складываются в кашу. Первая версия этой
    // проверки так и делала — и пережила мутацию «крыша снова
    // плоская».
    //
    // Смотрим на ОДИН дом: собираем связные пятна соломы и в каждом
    // сравниваем высоту конька с высотой свеса. У ската разница есть,
    // у плиты все столбцы одной высоты.
    {
        check(yHi > yLo + 1, "кровля занимает несколько уровней по высоте");

        const i32 x0 = site.center.x - VR, x1 = site.center.x + VR;
        const i32 z0 = site.center.z - VR, z1 = site.center.z + VR;
        const i32 w = x1 - x0 + 1, d = z1 - z0 + 1;

        // Верх соломы в каждом столбце; -1 — соломы нет.
        std::vector<i32> top((usize)w * d, -1);
        for (i32 x = x0; x <= x1; ++x)
            for (i32 z = z0; z <= z1; ++z)
                for (i32 y = yHi; y >= yLo; --y)
                    if (blockAt(x, y, z) == world::THATCH) {
                        top[(usize)(x - x0) * d + (z - z0)] = y;
                        break;
                    }

        std::vector<bool> seen((usize)w * d, false);
        int roofs = 0, pitched = 0;
        for (i32 ix = 0; ix < w; ++ix)
            for (i32 iz = 0; iz < d; ++iz) {
                const usize s0 = (usize)ix * d + iz;
                if (seen[s0] || top[s0] < 0) continue;

                // Волна по связному пятну соломы — это один дом.
                std::vector<usize> stack{ s0 };
                seen[s0] = true;
                i32 lo = top[s0], hi = top[s0], cells = 0;
                while (!stack.empty()) {
                    const usize cur = stack.back(); stack.pop_back();
                    ++cells;
                    lo = std::min(lo, top[cur]);
                    hi = std::max(hi, top[cur]);
                    const i32 cxi = (i32)(cur / (usize)d), czi = (i32)(cur % (usize)d);
                    const i32 dx[4] = { 1, -1, 0, 0 }, dz[4] = { 0, 0, 1, -1 };
                    for (int k = 0; k < 4; ++k) {
                        const i32 nx = cxi + dx[k], nz = czi + dz[k];
                        if (nx < 0 || nz < 0 || nx >= w || nz >= d) continue;
                        const usize ns = (usize)nx * d + nz;
                        if (seen[ns] || top[ns] < 0) continue;
                        seen[ns] = true;
                        stack.push_back(ns);
                    }
                }
                if (cells < 12) continue;      // не крыша, а обрывок
                ++roofs;
                if (hi - lo >= 2) ++pitched;
            }

        check(roofs > 0, "крыши нашлись");
        check(pitched == roofs,
              "у каждой крыши есть конёк и свес — это скат, а не плита");
    }

    // Двери, найденные при разборе стен, — их же потом проходят
    // дорожками.
    std::vector<glm::ivec3> doorCells;

    // ---- 3. В дом можно войти ----
    //
    // Ищем проёмы: воздух в стене из доски на уровне земли. Дверь
    // высотой в два блока — иначе в неё не пройти.
    {
        int doors = 0;
        for (i32 wx = site.center.x - VR; wx <= site.center.x + VR; ++wx)
            for (i32 wz = site.center.z - VR; wz <= site.center.z + VR; ++wz)
                for (i32 y = 1; y < world::CHUNK_SIZE_Y - 3; ++y) {
                    if (blockAt(wx, y, wz) != world::AIR) continue;
                    if (blockAt(wx, y + 1, wz) != world::AIR) continue;
                    if (blockAt(wx, y - 1, wz) != world::STONE) continue;
                    // По бокам — доска: значит это проём в стене.
                    auto wall = [&](i32 bx, i32 by, i32 bz) {
                        const u16 b = blockAt(bx, by, bz);
                        // Косяком бывает и угловая стойка из бревна.
                        return b == world::PLANK || b == world::WOOD;
                    };
                    const bool jambX = wall(wx-1, y, wz) && wall(wx+1, y, wz);
                    const bool jambZ = wall(wx, y, wz-1) && wall(wx, y, wz+1);
                    if (!jambX && !jambZ) continue;
                    // Дверь принадлежит ДОМУ, а над домом кровля.
                    // Просвет между столбами изгороди на каменной
                    // дорожке выглядел ровно так же.
                    bool roofed = false;
                    for (i32 yy = y + 1; yy < world::CHUNK_SIZE_Y; ++yy)
                        if (blockAt(wx, yy, wz) == world::THATCH) { roofed = true; break; }
                    if (roofed) ++doors;
                }
        char dm2[120];
        std::snprintf(dm2, sizeof(dm2), "дверных проёмов в два блока: %d", doors);
        check(doors > 0, dm2);

        // И смотрят они НА ДЕРЕВНЮ. Раньше дверь всегда была в стене
        // -Z, и у половины домов вход выходил в поле.
        //
        // Где у проёма «внутрь», подсказывает фонарь: он висит под
        // коньком, то есть в середине дома. Фундамент для этого не
        // годится — он выходит на блок за стену со ВСЕХ сторон.
        // Наружу — туда, где кончается дом.
        //
        // Раньше внутреннюю сторону подсказывал ближайший фонарь, и
        // уличный фонарь у дорожки сбивал определение. Дом кончается
        // там, где кончается кровля: в двух шагах от проёма наружу
        // её уже нет, а внутрь — есть.
        auto underRoof = [&](i32 bx, i32 by, i32 bz) {
            for (i32 y = by + 1; y < world::CHUNK_SIZE_Y; ++y)
                if (blockAt(bx, y, bz) == world::THATCH) return true;
            return false;
        };

        int facingWell = 0, facingAway = 0, dropped = 0, zeroDot = 0;
        doorCells.clear();
        for (i32 wx = site.center.x - VR; wx <= site.center.x + VR; ++wx)
            for (i32 wz = site.center.z - VR; wz <= site.center.z + VR; ++wz)
                for (i32 y = 1; y < world::CHUNK_SIZE_Y - 3; ++y) {
                    if (blockAt(wx, y, wz) != world::AIR) continue;
                    if (blockAt(wx, y + 1, wz) != world::AIR) continue;
                    if (blockAt(wx, y - 1, wz) != world::STONE) continue;

                    auto wall2 = [&](i32 bx, i32 by, i32 bz) {
                        const u16 b = blockAt(bx, by, bz);
                        return b == world::PLANK || b == world::WOOD;
                    };
                    const bool jambX = wall2(wx-1, y, wz) && wall2(wx+1, y, wz);
                    const bool jambZ = wall2(wx, y, wz-1) && wall2(wx, y, wz+1);
                    if (!jambX && !jambZ) continue;
                    if (!underRoof(wx, y, wz)) continue;   // не изгородь

                    // Ось поперёк стены и та её сторона, где дома нет.
                    i32 nx = 0, nz = 0;
                    if (jambX) {
                        const bool outPos = !underRoof(wx, y, wz + 2);
                        const bool outNeg = !underRoof(wx, y, wz - 2);
                        if (outPos == outNeg) { ++dropped; continue; }
                        nz = outPos ? 1 : -1;
                    } else {
                        const bool outPos = !underRoof(wx + 2, y, wz);
                        const bool outNeg = !underRoof(wx - 2, y, wz);
                        if (outPos == outNeg) { ++dropped; continue; }
                        nx = outPos ? 1 : -1;
                    }

                    const i32 dot = nx * (site.center.x - wx)
                                  + nz * (site.center.z - wz);
                    doorCells.push_back({ wx, y, wz });
                    if (dot > 0) ++facingWell; else if (dot < 0) ++facingAway;
                    else ++zeroDot;
                }
        char dm[160];
        std::snprintf(dm, sizeof(dm),
                      "двери к колодцу %d, от него %d, неясных %d, поперёк %d",
                      facingWell, facingAway, dropped, zeroDot);
        check(facingWell > 0, dm);
        std::snprintf(dm, sizeof(dm),
                      "и таких — подавляющее большинство (к %d против %d)",
                      facingWell, facingAway);
        check(facingAway * 3 <= facingWell, dm);
    }

    // ---- 4. Дом не заливает соломой изнутри ----
    //
    // Скат кладётся слоями, и если не вычищать пространство под ним,
    // чердак оказывается сплошным. Фонарь под коньком — свидетель:
    // он обязан стоять в воздухе, а не в толще соломы.
    {
        int buried = 0, checked = 0;
        for (i32 wx = site.center.x - VR; wx <= site.center.x + VR; ++wx)
            for (i32 wz = site.center.z - VR; wz <= site.center.z + VR; ++wz)
                for (i32 y = 1; y < world::CHUNK_SIZE_Y - 1; ++y) {
                    if (blockAt(wx, y, wz) != world::LANTERN) continue;
                    // Уличный фонарь стоит на столбе, и под ним дерево —
                    // это не завал. Домовой отличается кровлей над ним.
                    bool roofed = false;
                    for (i32 yy = y + 1; yy < world::CHUNK_SIZE_Y; ++yy)
                        if (blockAt(wx, yy, wz) == world::THATCH) { roofed = true; break; }
                    if (!roofed) continue;
                    ++checked;
                    // Под фонарём — внутренность дома, она обязана быть
                    // проходимой.
                    if (blockAt(wx, y - 1, wz) != world::AIR) ++buried;
                }
        check(checked > 0, "фонари нашлись");
        check(buried == 0, "под фонарём — жилое пространство, а не завал");
    }

    // ---- 4б. Кровля не дырявая ----
    //
    // Скаты обязаны сойтись. Пока каждый уровень просто сдвигался
    // внутрь на блок, при ширине в пять они останавливались, не
    // встретившись, и вдоль всего конька оставалась щель: дом стоял с
    // открытым верхом. Проверка это и нашла.
    //
    // Смотрим сверху: над каждым фонарём в доме обязана быть кровля.
    // Фонарь висит под коньком, то есть ровно там, где щель и была.
    {
        int roofed = 0, open_ = 0;
        for (i32 wx = site.center.x - VR; wx <= site.center.x + VR; ++wx)
            for (i32 wz = site.center.z - VR; wz <= site.center.z + VR; ++wz)
                for (i32 y = 1; y < world::CHUNK_SIZE_Y - 1; ++y) {
                    if (blockAt(wx, y, wz) != world::LANTERN) continue;
                    // Уличный фонарь стоит на столбе из дерева.
                    if (blockAt(wx, y - 1, wz) == world::WOOD) continue;
                    bool cover = false;
                    for (i32 yy = y + 1; yy < world::CHUNK_SIZE_Y; ++yy)
                        if (blockAt(wx, yy, wz) == world::THATCH) { cover = true; break; }
                    if (cover) ++roofed; else ++open_;
                }
        check(roofed > 0, "дома под кровлей");
        check(open_ == 0, "и ни одного с щелью по коньку");
    }

    // ---- 4в. Деревня обжита ----
    //
    // Шесть домов и колодец на пустой траве — это не деревня, а
    // макет. Обжитой её делают вещи, которые ставят вокруг себя
    // живущие люди: дорожка, поленница, стог, грядка, изгородь,
    // фонарь у дороги.
    {
        int streetLamps = 0, hay = 0, garden = 0;
        for (i32 wx = site.center.x - VR; wx <= site.center.x + VR; ++wx)
            for (i32 wz = site.center.z - VR; wz <= site.center.z + VR; ++wz)
                for (i32 y = 1; y < world::CHUNK_SIZE_Y - 1; ++y) {
                    const u16 b = blockAt(wx, y, wz);
                    if (b == world::LANTERN && blockAt(wx, y - 1, wz) == world::WOOD)
                        ++streetLamps;
                    // Стог — солома под открытым небом, а не кровля.
                    if (b == world::THATCH) {
                        bool sky = true;
                        for (i32 yy = y + 1; yy < world::CHUNK_SIZE_Y; ++yy)
                            if (blockAt(wx, yy, wz) != world::AIR) { sky = false; break; }
                        if (sky && blockAt(wx, y - 1, wz) == world::THATCH) ++hay;
                    }
                    // Всходы на вскопанной земле.
                    if (b == world::LEAVES && blockAt(wx, y - 1, wz) == world::DIRT)
                        ++garden;
                }
        // Дорожка проверяется СВЯЗНОСТЬЮ, а не количеством камня на
        // поверхности: камень выходит наружу и сам по себе — на
        // склонах и осыпях, — и первая версия проверки пережила
        // мутацию «дорожек нет», насчитав восемьдесят пять природных
        // блоков.
        //
        // Идём от каждой двери по прямой к колодцу и смотрим, мощёная
        // ли под ногами земля.
        auto topSolid = [&](i32 bx, i32 bz) -> u16 {
            for (i32 y = world::CHUNK_SIZE_Y - 1; y >= 1; --y) {
                const u16 b = blockAt(bx, y, bz);
                if (b != world::AIR && b != world::UNKNOWN && b != world::WATER)
                    return b;
            }
            return world::AIR;
        };

        int paved = 0, walked = 0;
        for (const auto& dpos : doorCells) {
            const i32 dx = site.center.x - dpos.x, dz = site.center.z - dpos.z;
            const i32 steps = std::max(std::abs(dx), std::abs(dz));
            if (steps < 6) continue;
            // Первые и последние пару шагов пропускаем: у порога
            // лежит фундамент, у колодца — его оголовок.
            for (i32 t = 3; t <= steps - 3; ++t) {
                const i32 x = dpos.x + dx * t / steps;
                const i32 z = dpos.z + dz * t / steps;
                ++walked;
                if (topSolid(x, z) == world::STONE) ++paved;
            }
        }
        char pm[180];
        std::snprintf(pm, sizeof(pm),
                      "дорожки: мощёных шагов %d из %d, фонарей %d, стогов %d, всходов %d",
                      paved, walked, streetLamps, hay, garden);
        check(true, pm);
        check(walked > 0, "от дверей до колодца есть что мерить");
        check(paved * 10 >= walked * 7,
              "и путь под ногами мощёный, а не трава");
        check(streetLamps > 0, "у дороги стоят фонари");
        check(hay + garden > 0, "во дворах есть утварь: стога и грядки");
    }

    // ---- 5. Жители появляются В деревне ----
    //
    // Спавнер раскладку деревни СПРАШИВАЕТ у генератора, а не
    // повторяет его правила у себя. Пока повторял, расхождение было
    // бы молчаливым: сдвинутый порог — и жители стоят в чистом поле.
    {
        ecs::Registry reg;
        npc::NpcSpawner sp;
        const glm::vec3 at{ (f32)site.center.x, 64.f, (f32)site.center.z };
        // Спавнер копит свой таймер по 1/60 за вызов и просыпается
        // раз в 0.75 с. Один вызов не делает ничего.
        for (int i = 0; i < 60; ++i) sp.update(mgr, reg, at, SEED);

        auto& pool = reg.pool<npc::NpcTag>();
        int far = 0, total = 0;
        for (usize i = 0; i < pool.size(); ++i) {
            const ecs::Entity e = pool.entityAt((u32)i);
            const auto* tf = reg.get<ecs::Transform>(e);
            if (!tf) continue;
            ++total;
            const f32 dx = tf->position.x - (f32)site.center.x;
            const f32 dz = tf->position.z - (f32)site.center.z;
            if (std::sqrt(dx * dx + dz * dz) > 48.f) ++far;
        }
        check(total > 0, "жители появились");
        check(far == 0, "и все — внутри той деревни, которую построил генератор");
    }
}

// ------------------------------------------------------------
// Переходы: покой, шаг, бег, прыжок, падение, приземление.
//
// Анимация знала ровно два положения — покой и шаг — и смешивала их
// по скорости. Прыжка, падения и приземления не было вовсе, поэтому
// существо падало с обрыва, перебирая ногами по воздуху.
// ------------------------------------------------------------
void testLocomotionStatesAndTransitions() {
    group("переходы: шаг, бег, полёт, посадка");

    using State = ecs::Locomotion::State;
    const f32 dt = 1.f / 60.f;

    // ---- Состояние выбирается скоростью и опорой ----
    {
        ecs::Locomotion lo{};
        anim::advanceLocomotion(lo, 0.f, 0.f, true, dt);
        check(lo.state == State::Idle, "стоит — покой");

        anim::advanceLocomotion(lo, 0.3f, 0.f, true, dt);
        check(lo.state == State::Walk, "медленно — шаг");

        anim::advanceLocomotion(lo, 0.95f, 0.f, true, dt);
        check(lo.state == State::Run, "быстро — бег");

        anim::advanceLocomotion(lo, 0.5f, 4.f, false, dt);
        check(lo.state == State::Jump, "оторвался вверх — прыжок");

        anim::advanceLocomotion(lo, 0.5f, -6.f, false, dt);
        check(lo.state == State::Fall, "летит вниз — падение");

        anim::advanceLocomotion(lo, 0.5f, 0.f, true, dt);
        check(lo.state == State::Land, "коснулся земли — приземление");
    }

    // ---- Опора не угадывается по вертикальной скорости ----
    //
    // В верхней точке прыжка она нулевая. Существо, которое в этот
    // миг считает себя стоящим, на мгновение встаёт в позу покоя
    // прямо в воздухе.
    {
        ecs::Locomotion lo{};
        anim::advanceLocomotion(lo, 0.f, 0.f, false, dt);
        check(lo.state == State::Fall || lo.state == State::Jump,
              "в высшей точке прыжка существо всё ещё в воздухе");
    }

    // ---- Переход не мгновенный ----
    {
        ecs::Locomotion lo{};
        anim::advanceLocomotion(lo, 0.f, -5.f, false, dt);
        const f32 afterOne = lo.air;
        check(afterOne > 0.f && afterOne < 1.f,
              "воздушная поза нарастает, а не включается кадром");

        for (int i = 0; i < 60; ++i)
            anim::advanceLocomotion(lo, 0.f, -5.f, false, dt);
        check(lo.air > 0.99f, "и за время перехода доходит до полной");

        // Вернулись на землю — гаснет так же плавно.
        anim::advanceLocomotion(lo, 0.f, 0.f, true, dt);
        const f32 first = lo.air;
        check(first < 1.f && first > 0.f, "и гаснет не мгновенно");
    }

    // ---- Приземление проходит само ----
    {
        ecs::Locomotion lo{};
        for (int i = 0; i < 10; ++i)
            anim::advanceLocomotion(lo, 0.f, -8.f, false, dt);
        anim::advanceLocomotion(lo, 0.f, 0.f, true, dt);
        check(lo.state == State::Land, "сел");
        check(lo.land > 0.5f, "и присед глубок в момент касания");

        f32 prev = lo.land;
        bool monotone = true, easing = true;
        for (int i = 0; i < 40; ++i) {
            anim::advanceLocomotion(lo, 0.f, 0.f, true, dt);
            if (lo.land > prev + 1e-5f) monotone = false;
            // Мало «не растёт»: присед обязан РАСПРЯМЛЯТЬСЯ каждый
            // кадр. Ступенька — тот же рывок, только отложенный: она
            // проходит проверку на монотонность и всё равно щёлкает.
            if (prev > 0.02f && lo.land > prev - 1e-6f) easing = false;
            prev = lo.land;
        }
        check(monotone, "присед обратно не проваливается");
        check(easing, "и распрямляется плавно, а не ступенькой");
        check(lo.land < 0.01f, "и проходит сам");
        check(lo.state == State::Idle, "после чего существо снова стоит");
    }

    // ---- Позы действительно разные ----
    {
        const entity::Rig& rig = player::rig();

        auto poseOf = [&](const anim::AnimState& st) {
            entity::Pose p;
            anim::poseFor(rig, p, st);
            return p;
        };
        auto differ = [&](const entity::Pose& a, const entity::Pose& b) {
            f32 worst = 0.f;
            for (u8 i = 0; i < rig.count; ++i)
                for (int k = 0; k < 3; ++k)
                    worst = std::max(worst, std::fabs(a.euler[i][k] - b.euler[i][k]));
            return worst;
        };

        anim::AnimState walk;  walk.phase = 1.2f; walk.speedNorm = 0.35f;
        anim::AnimState run;   run.phase  = 1.2f; run.speedNorm  = 1.0f;
        anim::AnimState fall;  fall.phase = 1.2f; fall.speedNorm = 0.35f;
        fall.air = 1.f; fall.rise = -1.f;
        anim::AnimState jump = fall; jump.rise = 1.f;
        anim::AnimState land;  land.phase = 1.2f; land.speedNorm = 0.f;
        land.land = 1.f;

        check(differ(poseOf(walk), poseOf(run)) > 0.05f,
              "бег отличается от шага не только частотой");
        check(differ(poseOf(walk), poseOf(fall)) > 0.2f,
              "в падении поза своя, а не продолжение шага");
        check(differ(poseOf(jump), poseOf(fall)) > 0.2f,
              "взлетая ноги поджаты, падая — вытянуты");
        check(differ(poseOf(land), poseOf(walk)) > 0.2f,
              "приземление — отдельная поза");

        // Наклон корпуса на бегу — это то, чем бег и отличается.
        {
            f32 torsoWalk = 0.f, torsoRun = 0.f;
            const entity::Pose w = poseOf(walk), r = poseOf(run);
            for (u8 i = 0; i < rig.count; ++i)
                if (rig.parts[i].role == entity::PartRole::Torso) {
                    torsoWalk = w.euler[i].x; torsoRun = r.euler[i].x;
                }
            check(torsoRun > torsoWalk + 0.05f, "на бегу корпус наклонён вперёд");
        }
    }
}

// ------------------------------------------------------------
// Селяне отличаются друг от друга.
//
// Раньше все жители деревни были побайтово одинаковы: оснастка
// строилась одна на вид, и шесть селян отличались только
// координатами. Деревня выглядела складом одинаковых кукол.
// ------------------------------------------------------------
void testNpcsVaryBetweenIndividuals() {
    group("NPC: облик особи, а не вида");

    auto measure = [](const entity::Rig& r) {
        entity::ResolvedPart p[entity::MAX_PARTS];
        const u8 n = entity::resolve(r, r.rest, glm::vec3(0.f), 0.f,
                                     p, entity::MAX_PARTS);
        f32 top = 0.f, wide = 0.f;
        u32 shirt = 0, skin = 0;
        u8 w = 0;
        for (u8 i = 0; i < r.count && w < n; ++i) {
            if (!r.parts[i].visible) continue;
            top  = std::max(top, p[w].center.y + p[w].size.y * 0.5f);
            wide = std::max(wide, p[w].size.x);
            if (r.parts[i].role == entity::PartRole::Torso) shirt = p[w].color;
            if (r.parts[i].role == entity::PartRole::Head)  skin  = p[w].color;
            ++w;
        }
        struct R { f32 top, wide; u32 shirt, skin; };
        return R{ top, wide, shirt, skin };
    };

    // ---- Облики действительно разные ----
    {
        std::set<u32> shirts, skins;
        std::set<int> heights;
        for (u32 v = 0; v < npc::NPC_VARIANTS; ++v) {
            // Семя подбираем так, чтобы попасть в каждый облик.
            u32 seed = 0;
            for (u32 t = 0; t < 100000u; ++t)
                if (npc::variantOf(t) == (u8)v) { seed = t; break; }
            const auto m = measure(npc::rigFor(npc::NPC_VILLAGER, seed));
            shirts.insert(m.shirt);
            skins.insert(m.skin);
            heights.insert((int)std::lround(m.top * 1000.f));
        }
        check(heights.size() >= npc::NPC_VARIANTS - 2,
              "почти каждый облик своего роста");
        check(shirts.size() >= 8, "и своего оттенка рубахи");
        check(skins.size() >= 4, "оттенков кожи несколько");
    }

    // ---- Но все они остаются селянами ----
    //
    // Вариация меняет ОСОБЬ, а не вид: если рост гуляет вдвое, а
    // рубаха перекрашивается в произвольный цвет, роль перестаёт
    // читаться и деревня превращается в балаган.
    {
        const auto& def = npc::npcRegistry().get(npc::NPC_VILLAGER);
        int tooTall = 0, tooOff = 0;
        for (u32 seed = 0; seed < 400u; ++seed) {
            const auto m = measure(npc::rigFor(npc::NPC_VILLAGER, seed));
            if (std::fabs(m.top - def.bodyHeight) > def.bodyHeight * 0.10f)
                ++tooTall;

            // Оттенок рубахи гуляет по яркости, но не по тону.
            const auto ch = [](u32 c, int sh) { return (f32)((c >> sh) & 0xFF); };
            const f32 r0 = ch(def.bodyColor, 24), g0 = ch(def.bodyColor, 16);
            const f32 r1 = ch(m.shirt, 24),       g1 = ch(m.shirt, 16);
            if (r0 > 1.f && g0 > 1.f && r1 > 1.f && g1 > 1.f) {
                const f32 ratio = (r1 / g1) / (r0 / g0);
                if (ratio < 0.9f || ratio > 1.1f) ++tooOff;
            }
        }
        check(tooTall == 0, "рост всех обликов держится в пределах десятой");
        check(tooOff == 0, "и тон рубахи остаётся тоном своей роли");
    }

    // ---- Облик устойчив ----
    //
    // Он берётся из постоянного ключа NPC. Если бы одно и то же семя
    // давало разный облик, селянин менялся бы в лице при каждой
    // перезагрузке чанка.
    {
        const auto a = measure(npc::rigFor(npc::NPC_VILLAGER, 12345u));
        const auto b = measure(npc::rigFor(npc::NPC_VILLAGER, 12345u));
        check(a.top == b.top && a.shirt == b.shirt && a.skin == b.skin,
              "одно семя — один и тот же облик");
    }

    // ---- Шаг соразмерен ногам особи ----
    //
    // Низкий селянин с длинным шагом скользил бы ногами точно так же,
    // как раньше скользили все.
    {
        f32 minS = 1e9f, maxS = 0.f;
        int mismatched = 0;
        for (u32 seed = 0; seed < 200u; ++seed) {
            const entity::Rig& r = npc::rigFor(npc::NPC_VILLAGER, seed);
            minS = std::min(minS, r.strideLength);
            maxS = std::max(maxS, r.strideLength);
            const auto m = measure(r);
            // Выше — значит и шаг длиннее: связь обязана быть.
            if (r.strideLength <= 0.f || m.top <= 0.f) ++mismatched;
        }
        check(mismatched == 0, "у каждого облика шаг положителен");
        check(maxS > minS * 1.02f, "и у разных обликов он разный");
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
// Меш накрывает землю на всю высоту чанка
//
// Эта проверка была написана про огрублённые уровни детализации:
// именно на них крыша мира проваливалась под поверхность, и по
// картинке это выглядело как дыра в земле вдали. Уровней больше нет,
// но проверять надо ровно то же самое — что меш накрывает КАЖДУЮ
// колонку чанка и накрывает её на настоящей высоте.
//
// Высоты здесь разбросаны по всей толще, от низины до почти потолка.
// Ровная земля на тридцатом уровне такую ошибку не ловит: мешер,
// которому урезали высоту разбора, на ней отработает как ни в чём не
// бывало, а горы срежет.
// ------------------------------------------------------------
void testMeshCoversGround() {
    group("world: меш накрывает землю на всю высоту чанка");

    world::blocks();
    auto chunk = std::make_unique<world::Chunk>();
    chunk->coord = {0, 0, 0};

    // Ступени и склоны по всей толще мира: у ровной земли нет тех
    // уступов, на которых меширование и спотыкается.
    std::vector<i32> surf((usize)world::CHUNK_SIZE * world::CHUNK_SIZE, -1);
    for (i32 z = 0; z < world::CHUNK_SIZE; ++z)
        for (i32 x = 0; x < world::CHUNK_SIZE; ++x) {
            const i32 h = 20 + (x * 3) + ((z / 4) % 7) + ((x + z) % 3);
            const i32 top = h < world::CHUNK_SIZE_Y - 2
                          ? h : world::CHUNK_SIZE_Y - 2;
            for (i32 y = 0; y <= top; ++y)
                chunk->setUnlocked(x, y, z,
                                   y == top ? world::GRASS : world::STONE);
            surf[(usize)z * world::CHUNK_SIZE + x] = top;
        }

    // Проверка имеет смысл, только если рельеф и правда достаёт до
    // верхней половины мира: иначе урезанный по высоте разбор её
    // пройдёт, ничего не заметив.
    i32 highest = 0;
    for (i32 v : surf) if (v > highest) highest = v;
    check(highest > world::CHUNK_SIZE_Y / 2 + 8,
          "рельеф достаёт до верхней половины мира");

    world::ChunkNeighbors nb;
    std::vector<world::Quad> quads;
    world::buildGreedyMesh(*chunk, nb, quads);

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
    char msg[160];
    std::snprintf(msg, sizeof(msg),
                  "все %zu колонок накрыты сверху (без крыши %d)",
                  cover.size(), uncovered);
    check(uncovered == 0, msg);
    std::snprintf(msg, sizeof(msg),
                  "крыша не проваливается под поверхность (провалов %d)",
                  sunken);
    check(sunken == 0, msg);
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
    world::buildGreedyMesh(*chunk, nb, quads);

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

    npc::NpcSpawner spawner;
    const save::SaveStatus ws = mgr.save(slot, world, reg, player, deltas,
                                         SEED, 777, day, spawner);
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

    npc::NpcSpawner spawner2;
    const save::SaveStatus rs = mgr.load(slot, world2, reg2, player2, deltas2,
                                         &outSeed, &outPlay, &day2, spawner2);
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
        return r.pendingVertices();
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

    bool allFit = true, anyGeometry = false;
    {
        world::buildGreedyMesh(*chunk, nb, quads);
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
    testMeshCoversGround();
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
    testJobsOutlivingTheirWorldAreSafe();
    testChunkStreamingIsBudgeted();
    testSurfaceHeightCacheMatchesGenerator();
    testUnloadAcceptsTighterRadius();
    testWorldStaysWithinMemoryBudget();
    testMeshQueueLosesNothing();
    testFailedUploadGoesBackToTheQueue();
    testFramePassOrder();
    testFrameGpuBreakdown();
    testFrameRateIsMeasuredByWallClock();
    testFrameRateLimitSetting();
    testClipboardLogTrimming();
    testWaterSortedByWaterCenter();
    testSettingsTogglesActuallyToggle();
    testUiTapSurvivesRedraw();
    testHudAndButtonsDoNotOverlap();
    testLoadingIsAPanelNotACurtain();
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
    testNpcsVaryBetweenIndividuals();
    testLocomotionStatesAndTransitions();
    testShurikenFliesAndHits();
    testDashMovesForward();
    testTrampolineThrowAndBounce();
    testElixirsGrantTimedBuff();
    testInventoryDragMovesItems();
    testReputationCanBeLost();
    testWisdomNodesActuallyWork();
    testDesertGrowsCactus();
    testDeadNpcStaysDead();
    testVillageHousesAreBuildings();
    testBlockEconomy();
    testEveryButtonHasAnAction();
    testSprintByDoubleTap();
    testPlayerControls();
    testWalkingNeverTeleports();
    testShowcaseHoldsEveryModel();
    testEntityInstanceBudget();
    testInstanceColorByteOrder();
    testEveryPassSetsViewport();
    testEveryUiCallbackIsWired();
    testEveryAudioEventIsFired();
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
