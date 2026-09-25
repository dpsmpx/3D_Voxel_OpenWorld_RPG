/**
 * @file impact.h
 * @brief Физика: удар падающего тела об опору и нырок в жидкость.
 */
#pragma once
#include "../core/types.h"
#include "../world/block.h"
#include <algorithm>
#include <cmath>

namespace physics {

/// Падение меряется ВЫСОТОЙ, которой оно стоит, а не скоростью.
///
/// Скорость упирается в предел падения и обнуляется рывком, а
/// высота честна: с какого верха начался полёт, столько он и стоит.
/// Мягкая опора и вода эту высоту понижают — гасят часть падения, —
/// и урон при посадке считается от того, что осталось. Высота и
/// квадрат скорости пропорциональны (v² = 2gh), поэтому гашение —
/// просто вычитание высот.

/// Итог удара об опору.
struct ImpactOutcome {
    /// Сколько высоты падения осталось после опоры: с ней тело
    /// бьётся о неё — или летит дальше сквозь неё, если проломило.
    f32  dropAfter = 0.f;
    /// Опора проломлена: блок разрушен, тело падает дальше.
    bool gaveWay   = false;
};

/// Падение с высоты `drop` блоков на опору с такими свойствами.
///
/// Проламывается опора от того удара, что пришёл, а не от
/// оставшегося: листва, погасившая падение целиком, но принявшая для
/// этого сильный удар, всё равно ломается — так в крону и прыгают.
inline ImpactOutcome resolveImpact(const world::BlockImpact& m, f32 drop) {
    ImpactOutcome o;
    if (drop <= 0.f) return o;
    o.dropAfter = std::max(0.f, drop - m.absorb);
    o.gaveWay = m.giveWay > 0.f && drop >= m.giveWay;
    return o;
}

/// Скорость удара после падения с `drop` блоков, и обратно.
inline f32 speedForDrop(f32 drop, f32 gravity) {
    return std::sqrt(2.f * std::fabs(gravity) * std::max(0.f, drop));
}
inline f32 dropForSpeed(f32 speed, f32 gravity) {
    const f32 g = std::max(1e-3f, std::fabs(gravity));
    return speed * speed / (2.f * g);
}

/// Нырок: скорость после того, как тело прошло жидкость.
///
/// Сопротивление квадратичное, dv/dt = −drag·v², и по пройденной
/// глубине скорость убывает как e^(−drag·x). `dragDepth` — сумма
/// drag·глубина по всему, что пройдено (вода, лава). Медленнее
/// `floor` — скорости плавания — жидкость не гасит: там у тела своя
/// физика, и нырок кончился.
inline f32 plungeSpeed(f32 speed, f32 dragDepth, f32 floor) {
    if (speed <= floor || dragDepth <= 0.f) return speed;
    return std::max(floor, speed * std::exp(-dragDepth));
}

} // namespace physics
