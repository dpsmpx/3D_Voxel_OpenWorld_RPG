/**
 * @file playtime.h
 * @brief Настройки, локализация, счётчик игрового времени.
 */
#pragma once
#include "../core/types.h"

namespace config {

/// Точный учёт игрового времени.
///
/// Проблема простого `playtimeSec += (u32)dt`: при fps=60
/// (dt≈0.0167) это даёт 0 прибавления, кроме кадров, где
/// накопление перепрыгнет 0.5. Итого время может считать
/// быстрее или медленнее реального.
///
/// Решение: float-аккумулятор, отдельно для игрового времени.
/// При сохранении — округляем до целых секунд.
struct PlaytimeTracker {
    f64 totalSeconds = 0.0;

    /// Вызывается каждый кадр с dt (в секундах).
    void tick(f64 dt) {
        if (dt <= 0.0) return;
        if (dt > 0.5) dt = 0.5;   // защита от больших скачков
        totalSeconds += dt;
    }

    /// Целое значение для сохранения в сейв.
    u32 seconds() const {
        if (totalSeconds < 0.0) return 0;
        return (u32)totalSeconds;
    }

    /// Сброс при загрузке сейва.
    void set(u32 s) {
        totalSeconds = (f64)s;
    }

    /// Форматирование "1h 23m 45s".
    void format(char* buf, usize bufSize) const;
};

} // namespace config
