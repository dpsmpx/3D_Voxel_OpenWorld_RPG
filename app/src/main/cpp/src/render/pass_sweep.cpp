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
    LOGI("  %-18s %6.2f мс   (опорное, кадров %u)",
         STEPS[0].name, (double)base, frames_[0]);
    LOGI("  %-18s %6.2f мс   <- проход рендера, показ и ожидание картинки;"
         " ниже этого не опустить",
         STEPS[1].name, (double)empty);
    if (base > empty && base > 0.f)
        LOGI("  на рисование уходит %.2f мс, это %.0f%% кадра",
             (double)(base - empty), (double)((base - empty) / base * 100.f));

    // Накопительно: разность соседних строк — то, что добавил очередной
    // проход поверх уже нарисованного. Подменять здесь некому.
    LOGI("  -- по одному сверху вниз, от пустого кадра --");
    f32 prev = empty;
    for (u32 i = CUMUL_FIRST; i <= CUMUL_LAST && i < STEP_COUNT; ++i) {
        const f32 cur = avg(i);
        LOGI("  %-18s %6.2f мс   -> %s добавил %.2f мс (кадров %u)",
             STEPS[i].name, (double)cur, STEPS[i].adds,
             (double)(cur - prev), frames_[i]);
        prev = cur;
    }

    // Выключение по одному — для сравнения. Расхождение с накопительным
    // ярусом и есть работа, перетекающая между проходами: небо рисуется
    // последним и с проверкой глубины, поэтому забирает себе ровно те
    // пиксели, которые не закрыл ландшафт.
    LOGI("  -- выключением по одному (числа занижены: проходы подменяют "
         "друг друга на экране) --");
    for (u32 i = CUMUL_LAST + 1; i < STEP_COUNT; ++i)
        LOGI("  %-18s %6.2f мс   -> разность с опорным %.2f мс (кадров %u)",
             STEPS[i].name, (double)avg(i),
             (double)(base - avg(i)), frames_[i]);
}

} // namespace render
