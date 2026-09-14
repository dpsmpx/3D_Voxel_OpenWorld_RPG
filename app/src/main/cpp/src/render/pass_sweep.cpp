/**
 * @file pass_sweep.cpp
 * @brief Рендер: меширование чанков, LOD, отсечение, инстансинг, камера.
 */
#include "pass_sweep.h"
#include "../core/log.h"

namespace render {

bool PassSweep::tick(f32 dt, f32 gpuMs) {
    if (!active_) return false;

    elapsed_ += dt;
    // Разгон выбрасываем: сразу после смены маски кадр идёт по другим
    // конвейерам, а первые его отправки ещё несут прежнюю работу.
    if (elapsed_ > WARMUP_SEC && gpuMs > 0.f) {
        sumMs_ += (f64)gpuMs;
        ++frames_;
    }
    if (elapsed_ < STEP_SEC) return false;

    result_[step_]  = frames_ ? (f32)(sumMs_ / (f64)frames_) : 0.f;
    counted_[step_] = frames_;
    if (done_ < STEP_COUNT) ++done_;

    elapsed_ = 0.f;
    reset();
    ++step_;
    if (step_ < STEP_COUNT) return false;

    step_ = 0;          // круг замкнулся, идём заново
    return true;
}

void PassSweep::report() const {
    if (done_ < STEP_COUNT) return;

    const f32 base = result_[0];
    LOGI("развёртка проходов: полное время кадра по меткам GPU, "
         "по %.0f с на комбинацию", (double)STEP_SEC);
    for (u32 i = 0; i < STEP_COUNT; ++i) {
        if (i == 0) {
            LOGI("  %-16s %6.2f мс   (опорное, кадров %u)",
                 STEPS[i].name, (double)result_[i], counted_[i]);
        } else {
            // Разность — это то, что выключенный проход стоил. Она
            // может выйти отрицательной: значит проход дешевле шума
            // замера, и по этим данным сказать про него нечего.
            const f32 cost = base - result_[i];
            LOGI("  %-16s %6.2f мс   -> %s %.2f мс%s (кадров %u)",
                 STEPS[i].name, (double)result_[i], STEPS[i].what,
                 (double)(cost < 0.f ? -cost : cost),
                 cost < 0.f ? " — ДЕШЕВЛЕ ШУМА ЗАМЕРА" : "",
                 counted_[i]);
        }
    }
}

} // namespace render
