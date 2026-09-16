// ============================================================
// tools/soak/soak.cpp — длинная игровая сессия на хосте.
//
// Всё, что выше по инструментам, меряет ОДНУ операцию: сколько
// стоит смешировать чанк, сколько стоит найти путь. Но провалы
// долгой сессии — не в стоимости операции. Они в том, что между
// первой минутой и двадцатой что-то накапливается: очередь, память,
// чанки, которые никто не выгрузил, задачи, которые никто не забрал.
// Такое видно только если прогнать сессию целиком.
//
// Здесь крутится настоящий ChunkManager на настоящем планировщике,
// с настоящей генерацией, мешированием, правками блоков, выгрузкой
// чанков и сохранениями. Рендера нет — вместо него потребитель,
// который забирает готовые меши и освобождает квады ровно так, как
// это делает ChunkRenderer::uploadChunks.
//
//   ./tools/soak/run.sh                 — 20 минут игрового времени
//   ./tools/soak/run.sh --minutes 5     — короче
//   ./tools/soak/run.sh --profile sprint — какой сценарий движения
// ============================================================
#include "core/job_system.h"
#include "world/block.h"
#include "world/chunk.h"
#include "world/chunk_manager.h"
#include "world/terrain.h"
#include "save/save_manager.h"
#include "save/world_delta.h"
#include "ecs/registry.h"
#include "ecs/components.h"
#include "items/item_def.h"
#include "items/currency.h"
#include "world/day_cycle.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;

// ---- Память процесса ----------------------------------------
// VmRSS из /proc: единственное, что на хосте отвечает на вопрос
// «растёт ли потребление за сессию». Строчка резидентной памяти в
// килобайтах.
usize rssKb() {
    std::FILE* f = std::fopen("/proc/self/status", "r");
    if (!f) return 0;
    char line[256];
    usize kb = 0;
    while (std::fgets(line, sizeof(line), f)) {
        if (std::strncmp(line, "VmRSS:", 6) == 0) {
            kb = (usize)std::strtoull(line + 6, nullptr, 10);
            break;
        }
    }
    std::fclose(f);
    return kb;
}

/// Сводка одной величины за сессию: начало, конец, пик.
struct Track {
    double first = -1.0, last = 0.0, peak = 0.0, sum = 0.0;
    u64    n = 0;
    /// Все замеры по порядку: утечку видно только по ним.
    std::vector<double> series;
    void add(double v) {
        if (first < 0.0) first = v;
        last = v;
        if (v > peak) peak = v;
        sum += v; ++n;
        series.push_back(v);
    }
    double avg() const { return n ? sum / (double)n : 0.0; }
    /// Значение на доле p сессии (0..1).
    double at(double p) const {
        if (series.empty()) return 0.0;
        usize i = (usize)(p * (double)(series.size() - 1));
        return series[i];
    }
};

/// Профиль движения игрока за сессию.
enum class Profile { Walk, Sprint, Teleport, Idle };

const char* profileName(Profile p) {
    switch (p) {
        case Profile::Walk:     return "ходьба (4 блока/с)";
        case Profile::Sprint:   return "бег по прямой (12 блоков/с)";
        case Profile::Teleport: return "рывки через полмира";
        case Profile::Idle:     return "стоим на месте";
    }
    return "?";
}

glm::vec3 positionAt(Profile p, f32 t) {
    switch (p) {
        case Profile::Idle:
            return { 8.f, 70.f, 8.f };
        case Profile::Walk: {
            // Круг радиусом 60 блоков: чанки въезжают и выезжают из
            // поля зрения непрерывно, как при обычной игре.
            const f32 a = t * 0.0667f;
            return { 60.f * std::cos(a), 70.f, 60.f * std::sin(a) };
        }
        case Profile::Sprint:
            return { t * 12.f, 70.f, t * 2.f };
        case Profile::Teleport: {
            // Каждые 20 секунд — прыжок на 400 блоков. Худший случай
            // для потоковой загрузки: весь набор чанков меняется разом.
            const i32 hop = (i32)(t / 20.f);
            return { (f32)(hop * 400), 70.f, (f32)(hop * 137) };
        }
    }
    return { 0.f, 70.f, 0.f };
}

} // namespace

int main(int argc, char** argv) {
    double minutes = 20.0;
    Profile profile = Profile::Walk;
    bool quiet = false;
    // Во сколько раз быстрее реального времени крутить сессию.
    //
    // По умолчанию — один к одному, и это не придирка. Фоновые
    // задачи разбираются воркерами в РЕАЛЬНОМ времени: прогони
    // двадцать минут игры за полсекунды, и очередь просто не успеет
    // разобраться ни разу. Тогда всё, что сессия скажет про очереди,
    // память и выгрузку, будет про несуществующий режим работы.
    double speed = 1.0;
    // Ноль воркеров — планировщик не запускается вовсе, задачи
    // копятся в очереди и никогда не выполняются. Само по себе это
    // не режим игры, но это единственный способ отделить стоимость
    // САМОЙ потоковой загрузки от конкуренции за замок карты чанков
    // с работающими воркерами.
    u32 workers = 3;

    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--minutes" && i + 1 < argc)      minutes = std::atof(argv[++i]);
        else if (a == "--profile" && i + 1 < argc) {
            const std::string p = argv[++i];
            if      (p == "walk")     profile = Profile::Walk;
            else if (p == "sprint")   profile = Profile::Sprint;
            else if (p == "teleport") profile = Profile::Teleport;
            else if (p == "idle")     profile = Profile::Idle;
        }
        else if (a == "--speed" && i + 1 < argc)   speed = std::atof(argv[++i]);
        else if (a == "--workers" && i + 1 < argc) workers = (u32)std::atoi(argv[++i]);
        else if (a == "--quiet") quiet = true;
    }
    if (speed <= 0.0) speed = 1.0;
    {
    }

    world::blocks();
    items::items();

    // Столько же воркеров, сколько движок просит на телефоне.
    if (workers > 0) jobs::gJobs.start(workers);

    constexpr f32 DT = 1.f / 60.f;
    const u64 frames = (u64)(minutes * 60.0 * 60.0);

    std::printf("soak: сессия %.1f мин (%llu кадров), профиль: %s\n",
                minutes, (unsigned long long)frames, profileName(profile));
    std::printf("      воркеров %u, скорость x%.0f реального времени\n\n",
                workers, speed);

    int rc = 0;
    {
        world::ChunkManager world(0x5EEDULL, 8);

        save::SaveManager saves;
        saves.init("build/soak");
        save::WorldDeltaStore deltas;
        world::DayCycle day;

        ecs::Registry reg;
        const ecs::Entity player = reg.create();
        reg.add(player, ecs::Transform{});
        reg.add(player, items::Wallet{});

        Track rss, pending, loaded, readyBacklog;
        // Кадр меряем не средним. Среднее прячет ровно то, что видно
        // глазом: редкий кадр в тридцать миллисекунд читается как
        // рывок, а в среднем по сессии его не существует.
        std::vector<double> frameMsAll;
        frameMsAll.reserve((usize)frames);
        // Разбивка кадра по фазам: средний кадр мира — сотые доли
        // миллисекунды, а редкий — двадцать. Без разбивки непонятно,
        // какая из пяти операций это делает, а гадать тут нельзя.
        Track phUpdate, phPoll, phUnload, phEdit, phSave;
        // Сколько кадров и когда именно провалились. Один пик на
        // старте и пик каждые двадцать секунд — разные болезни, а в
        // строчке «пик 21 мс» они выглядят одинаково.
        u64 slow2 = 0, slow8 = 0, slow16 = 0;
        std::vector<std::pair<double, double>> worstFrames;   // {секунда, мс}

        u64 meshesTaken = 0, chunksUnloaded = 0, blockEdits = 0;
        u64 saveOk = 0, saveFail = 0, loadOk = 0, loadFail = 0;
        u64 emptyPolls = 0;

        // Отсечки для отчёта: начало, четверти, конец.
        const u64 mark = frames / 4 ? frames / 4 : 1;

        f32  unloadTimer = 0.f;
        f32  saveTimer   = 0.f;
        const auto wall0 = Clock::now();
        double worstFrameMs = 0.0, worldUpdateMsSum = 0.0;

        for (u64 f = 0; f < frames; ++f) {
            const f32 t = (f32)f * DT;
            const glm::vec3 pos = positionAt(profile, t);

            const auto fr0 = Clock::now();
            auto phase = [](const Clock::time_point& a) {
                return std::chrono::duration<double, std::milli>(Clock::now() - a).count();
            };

            // --- то же, что делает кадр игры ---
            auto p0 = Clock::now();
            world.setCameraPosition(pos);
            world.update(pos);
            {
                const double ms = phase(p0);
                phUpdate.add(ms);
                if (ms > 2.0)  ++slow2;
                if (ms > 8.0)  ++slow8;
                if (ms > 16.0) ++slow16;
                if (ms > 2.0 && worstFrames.size() < 24)
                    worstFrames.push_back({ (double)t, ms });
            }

            // Потребитель мешей: ровно как ChunkRenderer::uploadChunks —
            // берём не больше дюжины за кадр и освобождаем квады.
            p0 = Clock::now();
            auto ready = world.pollMeshesReady(12);
            if (ready.empty()) ++emptyPolls;
            for (const auto& m : ready) {
                if (!m.chunk) continue;
                ++meshesTaken;
                std::lock_guard lk(m.chunk->meshMutex);
                m.chunk->mesh.built = false;
                std::vector<world::Quad>().swap(m.chunk->mesh.quads);
            }
            phPoll.add(phase(p0));

            unloadTimer += DT;
            if (unloadTimer >= 2.f) {
                unloadTimer = 0.f;
                p0 = Clock::now();
                auto doomed = world.collectUnloadCandidates(pos);
                chunksUnloaded += doomed.size();
                world.removeChunks(doomed);
                phUnload.add(phase(p0));
            }

            // Правка блоков: копаем под собой раз в секунду. Это путь
            // setVoxel -> перестройка своего чанка и соседей.
            if (f % 60 == 0 && world.isReadyAt((i32)pos.x, (i32)pos.z)) {
                p0 = Clock::now();
                const i32 y = world.generator().surfaceHeight((i32)pos.x, (i32)pos.z);
                world.setVoxel((i32)pos.x, y, (i32)pos.z, world::AIR);
                phEdit.add(phase(p0));
                ++blockEdits;
            }

            // Сохранение раз в 30 секунд игрового времени, как автосейв.
            saveTimer += DT;
            if (saveTimer >= 30.f) {
                saveTimer = 0.f;
                const save::SaveSlot slot = saves.slots().slot(0, 0);
                if (auto* tf = reg.get<ecs::Transform>(player)) tf->position = pos;
                p0 = Clock::now();
                const auto st = saves.save(slot, world, reg, player, deltas,
                                           0x5EEDULL, (u32)t, day);
                phSave.add(phase(p0));
                (st == save::SaveStatus::Ok) ? ++saveOk : ++saveFail;
            }

            const double frameMs =
                std::chrono::duration<double, std::milli>(Clock::now() - fr0).count();
            if (frameMs > worstFrameMs) worstFrameMs = frameMs;
            worldUpdateMsSum += frameMs;
            frameMsAll.push_back(frameMs);

            // Держим шаг с реальным временем: воркеры разбирают
            // очередь в нём, а не в игровом.
            const double budgetMs = (double)DT * 1000.0 / speed;
            if (frameMs < budgetMs) {
                std::this_thread::sleep_for(std::chrono::duration<double, std::milli>(
                    budgetMs - frameMs));
            }

            // Замеры раз в секунду игрового времени.
            if (f % 60 == 0) {
                rss.add((double)rssKb());
                pending.add((double)world.pendingJobs());
                loaded.add((double)world.loadedChunks());
            }

            if (!quiet && f && f % mark == 0) {
                std::printf("  %5.1f мин: чанков %5zu, задач в работе %4zu, "
                            "RSS %6.1f МБ\n",
                            t / 60.f, world.loadedChunks(), world.pendingJobs(),
                            (double)rssKb() / 1024.0);
                std::fflush(stdout);
            }
        }

        // Даём фоновым задачам догореть: то, что осталось в очереди
        // после конца сессии, — тоже результат.
        const usize pendingAtEnd = world.pendingJobs();
        for (int i = 0; i < 600 && world.pendingJobs() > 0; ++i) {
            world.pollMeshesReady();
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        const usize pendingAfterDrain = world.pendingJobs();

        // Чтение сейва обратно — в свежий мир, как при входе в игру.
        {
            const save::SaveSlot slot = saves.slots().slot(0, 0);
            world::ChunkManager w2(0x5EEDULL, 8);
            ecs::Registry r2;
            const ecs::Entity p2 = r2.create();
            r2.add(p2, ecs::Transform{});
            r2.add(p2, items::Wallet{});
            save::WorldDeltaStore d2;
            u64 seed = 0; u32 play = 0;
            world::DayCycle day2;
            const auto st = saves.load(slot, w2, r2, p2, d2, &seed, &play, &day2);
            (st == save::SaveStatus::Ok) ? ++loadOk : ++loadFail;
        }

        const double wallS =
            std::chrono::duration<double>(Clock::now() - wall0).count();

        std::printf("\n  ── итоги сессии ──\n");
        std::printf("  реального времени          %.1f с на %.1f мин игровых\n",
                    wallS, minutes);
        std::printf("  чанков в памяти            начало %.0f, конец %.0f, пик %.0f\n",
                    loaded.first, loaded.last, loaded.peak);
        std::printf("  чанков выгружено           %llu\n",
                    (unsigned long long)chunksUnloaded);
        std::printf("  мешей забрано              %llu\n",
                    (unsigned long long)meshesTaken);
        std::printf("  правок блоков              %llu\n",
                    (unsigned long long)blockEdits);
        std::printf("  сохранений                 %llu ok / %llu ошибок\n",
                    (unsigned long long)saveOk, (unsigned long long)saveFail);
        std::printf("  загрузок сейва             %llu ok / %llu ошибок\n",
                    (unsigned long long)loadOk, (unsigned long long)loadFail);
        std::printf("  задач в работе             сред. %.1f, пик %.0f\n",
                    pending.avg(), pending.peak);
        std::printf("  задач на конец сессии      %zu, после ожидания %zu\n",
                    pendingAtEnd, pendingAfterDrain);
        std::printf("  RSS                        начало %.1f МБ, конец %.1f МБ, "
                    "пик %.1f МБ\n",
                    rss.first / 1024.0, rss.last / 1024.0, rss.peak / 1024.0);
        std::sort(frameMsAll.begin(), frameMsAll.end());
        auto pct = [&](double p) {
            if (frameMsAll.empty()) return 0.0;
            usize i = (usize)(p * (double)(frameMsAll.size() - 1));
            return frameMsAll[i];
        };
        std::printf("  кадр мира (без рендера)    p50 %.3f мс, p99 %.3f мс, "
                    "худший %.2f мс\n",
                    pct(0.50), pct(0.99), worstFrameMs);
        std::printf("  средний кадр               %.3f мс\n",
                    worldUpdateMsSum / (double)frames);
        std::printf("\n  ── по фазам (сред. / пик, мс) ──\n");
        std::printf("  world.update (потоковая)   %.3f / %.2f\n",
                    phUpdate.avg(), phUpdate.peak);
        std::printf("  забор готовых мешей        %.3f / %.2f\n",
                    phPoll.avg(), phPoll.peak);
        std::printf("  выгрузка чанков            %.3f / %.2f\n",
                    phUnload.avg(), phUnload.peak);
        std::printf("  правка блока               %.3f / %.2f\n",
                    phEdit.avg(), phEdit.peak);
        std::printf("  сохранение                 %.3f / %.2f\n",
                    phSave.avg(), phSave.peak);
        std::printf("\n  ── провалы потоковой загрузки ──\n");
        std::printf("  кадров с update > 2 мс     %llu\n", (unsigned long long)slow2);
        std::printf("  кадров с update > 8 мс     %llu\n", (unsigned long long)slow8);
        std::printf("  кадров с update > 16 мс    %llu\n", (unsigned long long)slow16);
        if (!worstFrames.empty()) {
            std::printf("  когда (секунда сессии: мс):");
            for (const auto& w : worstFrames)
                std::printf(" %.0fс:%.0f", w.first, w.second);
            std::printf("\n");
        }

        // ---- приговор ----
        std::printf("\n  ── проверки ──\n");
        auto verdict = [&](bool ok, const char* what) {
            std::printf("    %s %s\n", ok ? "ok  " : "ПЛОХО", what);
            if (!ok) rc = 1;
        };

        // Число чанков обязано стабилизироваться: потоковая загрузка
        // держит круг вокруг игрока, и он не растёт со временем.
        verdict(loaded.last <= loaded.peak && loaded.peak < 1200,
                "число чанков в памяти ограничено");
        // Утечка — это рост ПОСЛЕ выхода на полку, а не разница между
        // первым и последним замером.
        //
        // Первый замер снимается на первом кадре, когда мир ещё пуст:
        // потоковая загрузка заводит чанки по бюджету, и круг
        // наполняется за первые секунды. Считать эту разницу ростом
        // значит объявлять утечкой саму загрузку мира. Сравниваем
        // четверть сессии с концом: к этому времени круг давно полон,
        // и всё, что прибавилось дальше, уже некуда списать.
        const double plateauMb = rss.at(0.25) / 1024.0;
        const double endMb     = rss.last / 1024.0;
        const double growMb    = endMb - plateauMb;
        std::printf("    (RSS: загрузка %.1f МБ -> полка %.1f МБ -> конец %.1f МБ; "
                    "рост после полки %+.1f МБ)\n",
                    rss.first / 1024.0, plateauMb, endMb, growMb);
        verdict(growMb < 16.0, "после выхода на полку резидентная память не растёт");
        verdict(pendingAfterDrain == 0, "очередь задач разбирается досуха");
        verdict(saveFail == 0 && loadFail == 0, "сейвы пишутся и читаются");
        verdict(meshesTaken > 0, "меши доезжают до потребителя");
    }

    if (workers > 0) jobs::gJobs.stop();
    std::printf("\n  %s\n", rc ? "soak: ЕСТЬ ЗАМЕЧАНИЯ" : "soak: замечаний нет");
    return rc;
}
