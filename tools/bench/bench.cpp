// ============================================================
// tools/bench/bench.cpp — замеры горячих участков кадра на хосте,
// без устройства и без Vulkan.
//
// Смысл: до сих пор решения о производительности принимались по
// одной сводке с телефона и по рассуждению «здесь наверняка дорого».
// Дважды это было верно (A* и остановка GPU), но проверить заранее
// было нечем. Здесь считаются те же функции, что крутятся в кадре,
// и видно, сколько стоит каждая.
//
// Числа с хоста нельзя переносить на телефон один в один: там другой
// процессор и другая память. Но соотношения переносятся, а именно
// они и нужны, чтобы выбрать, что чинить следующим.
//
//   ./tools/bench/run.sh            — все замеры
//   ./tools/bench/run.sh meshing    — только по подстроке в названии
// ============================================================
#include "core/job_system.h"
#include "audio/audio_engine.h"
#include "audio/sound_registry.h"
#include "world/block.h"
#include "world/chunk.h"
#include "world/chunk_manager.h"
#include "world/features.h"
#include "world/terrain.h"
#include "world/ai/pathfinding.h"
#include "render/mesh_builder.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <shared_mutex>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;

std::string g_filter;

/// Один замер: повторяем, пока не накопится хотя бы minMs, и
/// сообщаем время одного повтора. Разброс берём как разницу между
/// лучшим и медианным прогоном — если она велика, числу верить нельзя.
struct Result {
    double bestUs = 0.0;
    double medUs  = 0.0;
    u64    unitsPerRun = 1;
};

template<typename F>
Result measure(F&& body, int runs = 7, int itersPerRun = 0) {
    // Калибровка: сколько повторов набирают хотя бы 20 мс.
    if (itersPerRun <= 0) {
        itersPerRun = 1;
        for (;;) {
            const auto t0 = Clock::now();
            for (int i = 0; i < itersPerRun; ++i) body();
            const double ms = std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
            if (ms >= 20.0 || itersPerRun >= (1 << 22)) break;
            itersPerRun *= 2;
        }
    }

    std::vector<double> times;
    times.reserve((usize)runs);
    for (int r = 0; r < runs; ++r) {
        const auto t0 = Clock::now();
        for (int i = 0; i < itersPerRun; ++i) body();
        const double us = std::chrono::duration<double, std::micro>(Clock::now() - t0).count();
        times.push_back(us / (double)itersPerRun);
    }
    std::sort(times.begin(), times.end());
    Result r;
    r.bestUs = times.front();
    r.medUs  = times[times.size() / 2];
    return r;
}

/// Ширина строки в знаках, а не в байтах: printf выравнивает по
/// байтам, а кириллица весит по два — колонки разъезжались.
usize displayWidth(const char* s) {
    usize n = 0;
    for (const unsigned char* p = (const unsigned char*)s; *p; ++p)
        if ((*p & 0xC0) != 0x80) ++n;
    return n;
}

void report(const char* name, const Result& r, const char* unit, double units) {
    const double spread = r.medUs > 0.0 ? (r.medUs - r.bestUs) / r.medUs * 100.0 : 0.0;
    constexpr usize COL = 38;
    const usize w = displayWidth(name);
    std::printf("  %s%*s %9.2f мкс  (разброс %4.1f%%)",
                name, (int)(w < COL ? COL - w : 1), "", r.medUs, spread);
    if (units > 0.0 && unit)
        std::printf("   %.1f нс / %s", r.medUs * 1000.0 / units, unit);
    std::printf("\n");
}

bool wanted(const char* name) {
    return g_filter.empty() || std::string(name).find(g_filter) != std::string::npos;
}

template<typename F>
void bench(const char* name, F&& body, const char* unit = nullptr, double units = 0.0) {
    if (!wanted(name)) return;
    body();                       // прогрев: первый проход платит за кэш и аллокации
    report(name, measure(body), unit, units);
}

} // namespace

int main(int argc, char** argv) {
    if (argc > 1) g_filter = argv[1];

    std::printf("bench: горячие участки кадра (хост, %s)\n\n",
#if defined(__OPTIMIZE__)
                "с оптимизацией"
#else
                "БЕЗ оптимизации — числа не показательны"
#endif
    );

    world::blocks();
    const u64 seed = 20260913ull;
    world::TerrainGenerator gen(seed);

    // ---- Генерация ----
    std::printf("генерация мира\n");
    std::vector<world::TerrainGenerator::Column> cols;
    auto scratch = std::make_unique<world::Chunk>();
    scratch->coord = { 7, 0, -3 };

    // Сам объект чанка, до всякой генерации. Чанк — это массив
    // вокселей 32x128x32 по два байта, то есть четверть мегабайта,
    // и при создании он обнуляется весь. Потоковая загрузка
    // создаёт их пачками по две сотни за кадр.
    std::printf("  (sizeof(Chunk) = %.0f КБ)\n", (double)sizeof(world::Chunk) / 1024.0);
    bench("создание пустого чанка",
          [&] {
              auto c = std::make_shared<world::Chunk>();
              // Чтобы компилятор не выбросил создание целиком.
              c->coord.x = (i32)(usize)c.get();
          });

    bench("колонки чанка (32x32)",
          [&] { world::computeChunkColumns(gen, 7, -3, cols); },
          "колонка", 32.0 * 32.0);

    world::computeChunkColumns(gen, 7, -3, cols);
    bench("генерация чанка целиком",
          [&] { world::generateChunkVoxels(*scratch, gen, cols.data(), seed); },
          "воксель", (double)world::CHUNK_SIZE * world::CHUNK_SIZE * world::CHUNK_SIZE_Y);

    // Готовый чанк с соседями — на нём считаем меширование.
    auto makeChunk = [&](i32 cx, i32 cz) {
        auto c = std::make_shared<world::Chunk>();
        c->coord = { cx, 0, cz };
        std::vector<world::TerrainGenerator::Column> cc;
        world::computeChunkColumns(gen, cx, cz, cc);
        world::generateChunkVoxels(*c, gen, cc.data(), seed);
        c->generated.store(true);
        return c;
    };
    auto center = makeChunk(0, 0);
    auto nx = makeChunk(-1, 0), px = makeChunk(1, 0);
    auto nz = makeChunk(0, -1), pz = makeChunk(0, 1);
    world::ChunkNeighbors nb{};
    nb.nx = nx.get(); nb.px = px.get(); nb.nz = nz.get(); nb.pz = pz.get();

    // ---- Меширование ----
    std::printf("\nмеширование и вершины\n");
    std::vector<world::Quad> quads;
    bench("жадный меш чанка", [&] { world::buildGreedyMesh(*center, nb, quads); });

    world::buildGreedyMesh(*center, nb, quads);
    std::vector<render::VoxelVertex> verts;
    std::vector<u32> idx;
    u32 opaque = 0;
    bench("сборка вершин из квадов",
          [&] { render::buildChunkVertices(*center, quads, verts, idx, opaque); },
          "квад", (double)quads.size());
    std::printf("    (квадов %zu, вершин %zu, индексов %zu)\n",
                quads.size(), verts.size(), idx.size());

    // ---- Чтение вокселей ----
    // Физика, поиск пути и звук шагов ходят сюда тысячами раз за кадр,
    // и каждый вызов берёт два разделяемых замка.
    std::printf("\nчтение мира\n");
    // Чанки строит планировщик задач: без него карта наполнится
    // пустыми чанками, getVoxel уйдёт по короткому пути «ещё не
    // сгенерирован», и замер покажет не то, что происходит в игре.
    jobs::gJobs.start(4);
    {
        // Радиус 3 чанка — чтобы под миникартой (128x128 блоков,
        // то есть четыре чанка по стороне) лежал настоящий мир, а не
        // пустота. Иначе чтения уходят по короткому пути «чанка нет»,
        // и замер покажет не ту работу, что в игре.
        world::ChunkManager mgr(seed, 3);
        // Ждём настоящих данных, а не просто записей в карте.
        // Потоковая загрузка заводит чанки по бюджету на кадр, так
        // что кадров нужно много — как и в игре.
        for (int i = 0; i < 900; ++i) {
            mgr.update({ 8.f, 70.f, 8.f });
            const bool centreReady =
                mgr.getVoxel(8, world::CHUNK_SIZE_Y - 1, 8) == world::AIR &&
                mgr.getVoxel(8, 0, 8) == world::BEDROCK;
            if (centreReady && mgr.isReadyAt(-64, -64) && mgr.isReadyAt(64, 64) &&
                mgr.pendingJobs() == 0)
                break;
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        std::printf("    (чанков в памяти: %zu)\n", mgr.loadedChunks());

        volatile u32 sink = 0;
        bench("getVoxel: столб до потолка мира",
              [&] {
                  u32 s2 = 0;
                  for (i32 y = 64; y < world::CHUNK_SIZE_Y; ++y) s2 += mgr.getVoxel(8, y, 8);
                  sink = s2;
              },
              "воксель", (double)(world::CHUNK_SIZE_Y - 64));

        bench("getVoxel: куб 16x16x16 (как у физики)",
              [&] {
                  u32 s2 = 0;
                  for (i32 x = 0; x < 16; ++x)
                      for (i32 y = 60; y < 76; ++y)
                          for (i32 z = 0; z < 16; ++z) s2 += mgr.getVoxel(x, y, z);
                  sink = s2;
              },
              "воксель", 16.0 * 16.0 * 16.0);

        bench("VoxelReader: столб до потолка мира",
              [&] {
                  u32 s2 = 0;
                  world::VoxelReader rd(mgr);
                  for (i32 y = 64; y < world::CHUNK_SIZE_Y; ++y) s2 += rd.at(8, y, 8);
                  sink = s2;
              },
              "воксель", (double)(world::CHUNK_SIZE_Y - 64));

        bench("VoxelReader: куб 16x16x16",
              [&] {
                  u32 s2 = 0;
                  world::VoxelReader rd(mgr);
                  for (i32 x = 0; x < 16; ++x)
                      for (i32 y = 60; y < 76; ++y)
                          for (i32 z = 0; z < 16; ++z) s2 += rd.at(x, y, z);
                  sink = s2;
              },
              "воксель", 16.0 * 16.0 * 16.0);

        // Сколько из этого — замки и поиск чанка, а не само чтение.
        // Тот же столб, но чанк найден один раз и замок взят один раз.
        auto chunk = mgr.findChunk(0, 0);
        if (chunk) {
            bench("прямое чтение столба (замок один раз)",
                  [&] {
                      u32 s2 = 0;
                      std::shared_lock lk(chunk->voxelMutex);
                      for (i32 y = 64; y < world::CHUNK_SIZE_Y; ++y)
                          s2 += chunk->voxels[world::chunkIndex(8, y, 8)];
                      sink = s2;
                  },
                  "воксель", (double)(world::CHUNK_SIZE_Y - 64));

            bench("прямое чтение столба (совсем без замка)",
                  [&] {
                      u32 s2 = 0;
                      for (i32 y = 64; y < world::CHUNK_SIZE_Y; ++y)
                          s2 += chunk->voxels[world::chunkIndex(8, y, 8)];
                      sink = s2;
                  },
                  "воксель", (double)(world::CHUNK_SIZE_Y - 64));
        }
        (void)sink;

        // ---- Миникарта ----
        // Полная перерисовка: на каждый пиксель — высота поверхности
        // (то есть полный расчёт колонки по шуму) и вертикальный
        // проход по вокселям.
        std::printf("\nминикарта\n");
        bench("полная перерисовка 128x128 (как сейчас)",
              [&] {
                  u32 s2 = 0;
                  world::VoxelReader rdm(mgr);
                  for (i32 py = 0; py < 128; ++py)
                      for (i32 px = 0; px < 128; ++px) {
                          const i32 wx = px - 64, wz = py - 64;
                          const i32 sy = rdm.surfaceAt(wx, wz);
                          for (i32 y = sy + 4; y >= (sy - 8 < 1 ? 1 : sy - 8); --y) {
                              const u16 b = rdm.at(wx, y, wz);
                              if (b != world::AIR && b != world::WATER) { s2 += b; break; }
                          }
                      }
                  sink = s2;
              },
              "пиксель", 128.0 * 128.0);

        bench("высоты из чанка 128x128",
              [&] {
                  u32 s2 = 0;
                  world::VoxelReader rdm(mgr);
                  for (i32 py = 0; py < 128; ++py)
                      for (i32 px = 0; px < 128; ++px)
                          s2 += (u32)rdm.surfaceAt(px - 64, py - 64);
                  sink = s2;
              },
              "пиксель", 128.0 * 128.0);

        // Как было до кэша высот: тот же проход, но высота считается
        // генератором заново на каждый пиксель.
        bench("полная перерисовка 128x128 (было: высота от генератора)",
              [&] {
                  u32 s2 = 0;
                  world::VoxelReader rdm(mgr);
                  for (i32 py = 0; py < 128; ++py)
                      for (i32 px = 0; px < 128; ++px) {
                          const i32 wx = px - 64, wz = py - 64;
                          const i32 sy = gen.surfaceHeight(wx, wz);
                          for (i32 y = sy + 4; y >= (sy - 8 < 1 ? 1 : sy - 8); --y) {
                              const u16 b = rdm.at(wx, y, wz);
                              if (b != world::AIR && b != world::WATER) { s2 += b; break; }
                          }
                      }
                  sink = s2;
              },
              "пиксель", 128.0 * 128.0);

        bench("было: только высоты от генератора 128x128",
              [&] {
                  u32 s2 = 0;
                  for (i32 py = 0; py < 128; ++py)
                      for (i32 px = 0; px < 128; ++px)
                          s2 += (u32)gen.surfaceHeight(px - 64, py - 64);
                  sink = s2;
              },
              "пиксель", 128.0 * 128.0);

        // ---- Поиск пути ----
        // Главный потребитель чтения вокселей: один поиск делает их
        // тысячами. Именно он раньше съедал по пятнадцать миллисекунд
        // из шестнадцати, когда в кадр попадало сразу несколько мобов.
        std::printf("\nпоиск пути\n");
        const i32 surf = gen.surfaceHeight(8, 8);
        const glm::ivec3 from{ 8, surf, 8 };
        world::ai::MoveParams mp;
        for (int dist : { 8, 24 }) {
            const i32 s2 = gen.surfaceHeight(8 + dist, 8 + dist);
            const glm::ivec3 to{ 8 + dist, s2, 8 + dist };
            char name[96];
            std::snprintf(name, sizeof(name), "A* на %d блоков", dist);
            world::ai::PathResult res;
            if (!wanted(name)) continue;
            res = world::ai::findPath(mgr, from, to, mp, 2000);
            report(name, measure([&] {
                auto r2 = world::ai::findPath(mgr, from, to, mp, 2000);
                sink = (u32)r2.waypoints.size();
            }), nullptr, 0.0);
            std::printf("    (путь найден: %s, точек %zu)\n",
                        res.ok ? "да" : "нет", res.waypoints.size());
        }
    }
    // Мир разрушен — теперь можно останавливать планировщик. Обратный
    // порядок повесил бы выход: деструктор мира ждёт фоновых задач.
    jobs::gJobs.stop();

    // ---- Звук ----
    // mixInto крутится на потоке реального времени: буфер надо отдать
    // до срока, иначе в динамике будет щелчок.
    std::printf("\nзвук\n");
    {
        const auto t0 = Clock::now();
        audio::SoundRegistry::instance().init(48000);
        std::printf("    (генерация звуков при запуске: %.0f мс)\n",
                    std::chrono::duration<double, std::milli>(Clock::now() - t0).count());

        audio::AudioEngine snd;
        snd.setMasterVolume(1.f);
        snd.setSfxVolume(1.f);
        constexpr u32 AFRAMES = 256;
        std::vector<f32> abuf((usize)AFRAMES * 2);

        // Четыре музыкальные петли, как в игре: слышна одна.
        audio::VoiceHandle music[4];
        const audio::SoundId tracks[4] = {
            audio::SOUND_MUSIC_EXPLORE, audio::SOUND_MUSIC_COMBAT,
            audio::SOUND_MUSIC_DUNGEON, audio::SOUND_MUSIC_VILLAGE };
        for (int i = 0; i < 4; ++i) {
            music[i] = snd.play(tracks[i], i == 0 ? 1.f : 0.f, true);
            snd.setVoiceIsMusic(music[i], true);
        }
        snd.setMusicVolume(1.f);
        snd.update(0.016f, {});
        bench("mixInto: музыка (слышна одна из четырёх)",
              [&] { snd.mixInto(abuf.data(), AFRAMES); },
              "кадр", (double)AFRAMES);

        for (int i = 0; i < 4; ++i) snd.setVoiceGain(music[i], 1.f);
        snd.update(0.016f, {});
        bench("mixInto: музыка (слышны все четыре)",
              [&] { snd.mixInto(abuf.data(), AFRAMES); },
              "кадр", (double)AFRAMES);
    }

    std::printf("\n");
    return 0;
}
