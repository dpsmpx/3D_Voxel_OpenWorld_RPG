/**
 * @file pass_sweep.cpp
 * @brief Рендер: меширование чанков, LOD, отсечение, инстансинг, камера.
 */
#include "pass_sweep.h"
#include "../core/log.h"

namespace render {

void PassSweep::start() {
    active_   = true;
    finished_ = false;
    step_     = 0;
    round_    = 0;
    elapsed_  = 0.f;
    for (u32 i = 0; i < STEP_COUNT; ++i) { sum_[i] = 0.0; frames_[i] = 0; }
}

bool PassSweep::tick(f32 dt, f32 gpuMs) {
    if (!active_) return false;

    elapsed_ += dt;
    // Разгон выбрасываем: сразу после смены маски кадр идёт по другим
    // конвейерам, а в очереди ещё доигрывают кадры с прежней.
    if (elapsed_ > WARMUP_SEC && gpuMs > 0.f) {
        sum_[step_] += (f64)gpuMs;
        ++frames_[step_];
    }
    if (elapsed_ < SLICE_SEC) return false;

    elapsed_ = 0.f;
    ++step_;
    if (step_ < STEP_COUNT) return false;

    // Круг замкнулся. Комбинации чередуются, а не идут подряд: так
    // нагрев телефона размазывается по всем поровну.
    step_ = 0;
    ++round_;
    if (round_ < ROUNDS) return false;

    active_   = false;
    finished_ = true;
    return true;
}

void PassSweep::report() const {
    auto avg = [&](u32 i) {
        return frames_[i] ? (f32)(sum_[i] / (f64)frames_[i]) : 0.f;
    };
    const f32 base  = avg(0);
    const f32 empty = avg(1);

    LOGI("развёртка проходов: полное время кадра по меткам GPU, "
         "%u кругов по %.2f с на комбинацию", ROUNDS, (double)SLICE_SEC);
    LOGI("  %-16s %6.2f мс   (опорное, кадров %u)",
         STEPS[0].name, (double)base, frames_[0]);
    LOGI("  %-16s %6.2f мс   -> ВСЁ рисование кадра %.2f мс (кадров %u)",
         STEPS[1].name, (double)empty, (double)(base - empty), frames_[1]);

    for (u32 i = 2; i < STEP_COUNT; ++i) {
        const f32 cost = base - avg(i);
        LOGI("  %-16s %6.2f мс   -> %s %.2f мс%s (кадров %u)",
             STEPS[i].name, (double)avg(i), STEPS[i].what,
             (double)(cost < 0.f ? -cost : cost),
             cost < 0.f ? " — ДЕШЕВЛЕ ШУМА ЗАМЕРА" : "",
             frames_[i]);
    }

    // Главный вывод печатаем прямо, а не оставляем читателю. Пустой
    // кадр — это проход рендера, очистка, показ и ожидание картинки
    // цепочки; если он стоит почти столько же, сколько полный, то
    // кадр упирается НЕ в рисование, и оптимизировать шейдеры
    // бессмысленно.
    if (base > 0.f) {
        const f32 drawn = base - empty;
        LOGI("  итог: на рисование уходит %.0f%% кадра, остальное — "
             "проход рендера, показ и ожидание картинки цепочки",
             (double)(drawn / base * 100.f));
    }
}

} // namespace render
