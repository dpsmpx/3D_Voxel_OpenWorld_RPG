/**
 * @file day_cycle.h
 * @brief Мир: чанки, процедурная генерация, биомы, структуры, цикл суток.
 */
#pragma once
#include "../core/types.h"
#include <glm/glm.hpp>
#include <cmath>

namespace world {

/// Игровые сутки.
///
/// Время идёт независимо от системных часов и сохраняется вместе с
/// миром, поэтому загруженная игра продолжается с того же рассвета.
/// От этого зависят: спавн мобов, направление солнца, цвет неба и
/// обновление ассортимента торговцев.
class DayCycle {
public:
    /// Длина полных суток в реальных секундах.
    static constexpr f32 DAY_LENGTH_SEC = 1200.f;   // 20 минут

    /// Границы суток в долях дня: рассвет, закат.
    static constexpr f32 DAWN = 0.22f;
    static constexpr f32 DUSK = 0.78f;

    /// Новый мир начинается утром, а не в полночь.
    void reset(f32 timeOfDay = 0.30f, u32 day = 0) {
        timeOfDay_ = timeOfDay;
        day_       = day;
        dayJustChanged_ = false;
    }

    void tick(f32 dt) {
        dayJustChanged_ = false;
        timeOfDay_ += dt / DAY_LENGTH_SEC;
        while (timeOfDay_ >= 1.f) {
            timeOfDay_ -= 1.f;
            ++day_;
            dayJustChanged_ = true;
        }
    }

    /// Время суток в [0, 1): 0 — полночь, 0.5 — полдень.
    f32  timeOfDay() const { return timeOfDay_; }
    u32  day()       const { return day_; }

    /// true ровно в тот кадр, когда наступили новые сутки.
    bool dayJustChanged() const { return dayJustChanged_; }

    bool isNight() const { return timeOfDay_ < DAWN || timeOfDay_ >= DUSK; }

    /// Высота солнца над горизонтом, [-1, 1]. Отрицательная — ночь.
    f32 sunElevation() const {
        // Полдень (0.5) — зенит, полночь — надир.
        return -std::cos(timeOfDay_ * 6.283185f);
    }

    /// Направление НА солнце. Солнце ходит по дуге восток→запад.
    glm::vec3 sunDirection() const {
        const f32 a = timeOfDay_ * 6.283185f;
        return glm::normalize(glm::vec3(std::sin(a) * 0.6f,
                                        -std::cos(a),
                                        0.35f));
    }

    /// Освещённость от неба, [0.12, 1]: ночью не абсолютная темнота.
    f32 skyLight() const {
        const f32 e = sunElevation();
        const f32 t = glm::clamp(e * 2.f + 0.5f, 0.f, 1.f);
        return 0.12f + t * 0.88f;
    }

    /// Цвет тумана и неба: от ночного индиго через рассветный
    /// янтарь к дневной лазури.
    glm::vec3 skyColor() const {
        const glm::vec3 night{0.05f, 0.07f, 0.14f};
        const glm::vec3 dawn {0.85f, 0.55f, 0.35f};
        const glm::vec3 day  {0.55f, 0.72f, 0.92f};

        const f32 e = sunElevation();
        if (e <= -0.15f) return night;
        if (e >= 0.25f)  return day;
        if (e < 0.05f) {
            const f32 t = (e + 0.15f) / 0.20f;
            return glm::mix(night, dawn, t);
        }
        const f32 t = (e - 0.05f) / 0.20f;
        return glm::mix(dawn, day, t);
    }

    /// ---- Сериализация ----
    f32  rawTime() const { return timeOfDay_; }
    void setRaw(f32 timeOfDay, u32 day) {
        timeOfDay_ = glm::clamp(timeOfDay, 0.f, 0.999f);
        day_ = day;
    }

private:
    f32  timeOfDay_ = 0.30f;
    u32  day_       = 0;
    bool dayJustChanged_ = false;
};

} // namespace world
