/**
 * @file locomotion.h
 * @brief Поза сущности: качание суставов по фазе шага.
 */
#pragma once
#include "rig.h"
#include <cmath>

namespace anim {

/// Насколько сильно качается сустав при полной скорости, радианы.
///
/// Разные по роли: бедро ходит шире голени, рука — меньше ноги.
/// Это и есть «анимация соответствует анатомии» вместо одного
/// синуса на всё.
struct Swing {
    f32 upperLeg = 0.62f;
    f32 lowerLeg = 0.40f;
    f32 arm      = 0.45f;
    f32 tail     = 0.22f;
    f32 ear      = 0.30f;
    f32 head     = 0.05f;
};

/// Знак фазы для конечности: диагональные пары шагают вместе.
///
/// У четвероногого правая передняя идёт с левой задней — иначе
/// получается иноходь, и существо переваливается боком.
inline f32 limbPhaseSign(entity::PartRole r) {
    switch (r) {
        case entity::PartRole::UpperLegFR:
        case entity::PartRole::LowerLegFR:
        case entity::PartRole::UpperLegBL:
        case entity::PartRole::LowerLegBL:
        case entity::PartRole::UpperArmL:
        case entity::PartRole::LowerArmL:
        case entity::PartRole::HandL:
            return 1.f;
        case entity::PartRole::UpperLegFL:
        case entity::PartRole::LowerLegFL:
        case entity::PartRole::UpperLegBR:
        case entity::PartRole::LowerLegBR:
        case entity::PartRole::UpperArmR:
        case entity::PartRole::LowerArmR:
        case entity::PartRole::HandR:
            return -1.f;
        default:
            return 0.f;
    }
}

inline bool isUpperLimb(entity::PartRole r) {
    switch (r) {
        case entity::PartRole::UpperLegFL: case entity::PartRole::UpperLegFR:
        case entity::PartRole::UpperLegBL: case entity::PartRole::UpperLegBR:
            return true;
        default: return false;
    }
}
inline bool isLowerLimb(entity::PartRole r) {
    switch (r) {
        case entity::PartRole::LowerLegFL: case entity::PartRole::LowerLegFR:
        case entity::PartRole::LowerLegBL: case entity::PartRole::LowerLegBR:
            return true;
        default: return false;
    }
}
/// В какую сторону складывается сгиб конечности.
///
/// Это анатомия, а не вкус. Поворот сустава на ПОЛОЖИТЕЛЬНЫЙ угол по
/// X уводит нижнее звено назад, в -Z (проверено численно), поэтому:
///
///   * передние ноги и руки складываются НАЗАД — колено (запястье)
///     уходит пяткой к корпусу; знак +1;
///   * задние ноги складываются ВПЕРЁД — коленный сустав зверя смотрит
///     вперёд, а скакательный назад; знак -1.
///
/// Раньше знак был один на всех и отрицательный: голень уезжала в +Z,
/// то есть КАЖДОЕ колено выгибалось вперёд, как у птицы. Проверка это
/// пропускала, потому что требовала «угол не положительный» — то есть
/// сторожила знак, а не анатомию.
inline f32 kneeBendSign(entity::PartRole r) {
    switch (r) {
        case entity::PartRole::LowerLegBL:
        case entity::PartRole::LowerLegBR:
            return -1.f;
        default:
            return 1.f;
    }
}

inline bool isArm(entity::PartRole r) {
    switch (r) {
        case entity::PartRole::UpperArmL: case entity::PartRole::UpperArmR:
        case entity::PartRole::LowerArmL: case entity::PartRole::LowerArmR:
        case entity::PartRole::HandL:     case entity::PartRole::HandR:
            return true;
        default: return false;
    }
}

/// Поза шага.
///
/// Суставы ВРАЩАЮТСЯ, а не сдвигаются: нога описывает дугу вокруг
/// бедра, как у живого существа. Прежняя анимация двигала коробку
/// параллельно себе, отчего походка выглядела «плывущей».
inline void walkPose(const entity::Rig& rig, entity::Pose& pose,
                     f32 phase, f32 speedNorm, const Swing& sw = Swing{})
{
    pose.clear();
    const f32 s = std::sin(phase);
    // Голень отстаёт от бедра на четверть периода: так колено
    // сгибается в переносе и распрямляется в опоре.
    const f32 sLower = std::sin(phase - 1.5707963f);

    for (u8 i = 0; i < rig.count && i < entity::MAX_PARTS; ++i) {
        const entity::PartRole r = rig.parts[i].role;
        const f32 sign = limbPhaseSign(r);

        if (isUpperLimb(r)) {
            pose.euler[i].x = s * sw.upperLeg * speedNorm * sign;
        } else if (isLowerLimb(r)) {
            // Колено гнётся только в одну сторону — свою для передних
            // и задних ног, см. kneeBendSign.
            const f32 bend = (sLower * sign + 1.f) * 0.5f;
            pose.euler[i].x = kneeBendSign(r) * bend * sw.lowerLeg * speedNorm;
        } else if (isArm(r)) {
            pose.euler[i].x = s * sw.arm * speedNorm * sign;
        } else if (r == entity::PartRole::Tail) {
            // Все слагаемые ходьбы обязаны гаснуть вместе со
            // скоростью: иначе у стоящего зверя хвост продолжает
            // мотаться в такт шагу, которого нет. Покачивание в покое
            // — дело idlePose, и оно идёт временем, а не фазой.
            pose.euler[i].y = std::sin(phase * 0.5f) * sw.tail * speedNorm;
            pose.euler[i].x = std::sin(phase) * sw.tail * 0.4f * speedNorm;
        } else if (r == entity::PartRole::Ear) {
            // Ухо отстаёт от головы: оно мягкое, его мотает шагом.
            // Без этого зверь на бегу выглядит чучелом.
            pose.euler[i].x = std::sin(phase * 2.f - 0.7f) * sw.ear * speedNorm;
            pose.euler[i].z = std::sin(phase) * sw.ear * 0.5f * speedNorm;
        } else if (r == entity::PartRole::Head) {
            pose.euler[i].x = std::sin(phase * 2.f) * sw.head * speedNorm;
        } else if (r == entity::PartRole::Torso) {
            // Корпус чуть покачивается в такт — иначе низ идёт, а
            // верх стоит, и существо выглядит катящимся.
            pose.euler[i].z = std::sin(phase) * 0.04f * speedNorm;
            pose.euler[i].y = std::sin(phase * 0.5f) * 0.05f * speedNorm;
        }
    }
}

/// Поза покоя: едва заметное дыхание.
inline void idlePose(const entity::Rig& rig, entity::Pose& pose, f32 t) {
    pose.clear();
    const f32 breathe = std::sin(t * 1.6f);
    for (u8 i = 0; i < rig.count && i < entity::MAX_PARTS; ++i) {
        const entity::PartRole r = rig.parts[i].role;
        if (r == entity::PartRole::Torso)
            pose.euler[i].x = breathe * 0.012f;
        else if (r == entity::PartRole::Head)
            pose.euler[i].x = breathe * 0.02f;
        else if (r == entity::PartRole::Tail)
            pose.euler[i].y = std::sin(t * 0.9f) * 0.10f;
        else if (r == entity::PartRole::Ear)
            pose.euler[i].x = std::sin(t * 1.1f) * 0.05f;
    }
}

/// Всё, что анимации нужно знать о сущности.
///
/// Именно всё: ни мобов, ни NPC, ни игрока здесь нет. Раньше поза
/// строилась прямо из mobs::MobAI, и NPC с игроком воспользоваться
/// ею не могли — у них своя структура состояния. Отсюда и брались
/// самодельные `sin(walkPhase)` в рендере NPC, двигавшие ногу
/// параллельно себе.
struct AnimState {
    f32 phase     = 0.f;   ///< фаза шага, радианы; идёт ПУТЁМ
    f32 time      = 0.f;   ///< монотонное время, секунды; для дыхания
    f32 speedNorm = 0.f;   ///< скорость к максимальной, 0..1
    f32 attack    = 0.f;   ///< 0..1, замах
    f32 death     = 0.f;   ///< секунды с момента смерти, 0 — жив
};

/// Продвинуть фазу шага пройденным путём.
///
/// Раньше фазу двигало время: `walkPhase += dt * 9.f` в погоне,
/// `* 6.f` в ходьбе, `* 2.f` в покое. Скорость и длина шага при этом
/// не связаны ничем, поэтому ноги скользят — и «правильного»
/// множителя не существует: он верен ровно для одной скорости.
///
/// Здесь согласованность — следствие формулы: за длину шага модель
/// проходит ровно один цикл, какой бы ни была скорость.
///
/// При остановке фаза ЗАМИРАЕТ. Не сбрасывается: иначе существо
/// дёргает ногой всякий раз, как встанет.
template <class GaitT>
inline void advanceGait(GaitT& g, const glm::vec3& velocity, f32 dt) {
    constexpr f32 TAU = 6.28318530718f;
    const f32 speed = std::sqrt(velocity.x * velocity.x +
                                velocity.z * velocity.z);
    if (speed < 0.05f) return;

    const f32 stride = g.stride > 0.01f ? g.stride : 1.6f;
    g.phase += (speed * dt) / stride * TAU;
    // Держим фазу в разумных пределах: за час игры она иначе
    // вырастает настолько, что синус теряет точность.
    if (g.phase >= TAU) g.phase = std::fmod(g.phase, TAU);
}

/// Поза сущности: покой смешивается с шагом по нормированной скорости.
///
/// Смешивание, а не переключение: иначе при трогании поза прыгает.
inline void poseFor(const entity::Rig& rig, entity::Pose& pose,
                    const AnimState& st)
{
    entity::Pose walk, idle;
    walkPose(rig, walk, st.phase, st.speedNorm);
    // Дыхание идёт ВРЕМЕНЕМ: фаза шага у стоящего замерла, и покой,
    // построенный на ней, замер бы вместе с ней.
    idlePose(rig, idle, st.time);

    const f32 k = st.speedNorm < 0.f ? 0.f : (st.speedNorm > 1.f ? 1.f : st.speedNorm);
    for (u8 i = 0; i < rig.count && i < entity::MAX_PARTS; ++i) {
        // Поза покоя — постоянный наклон модели (висячее ухо, поднятый
        // хвост). Анимация кладётся ПОВЕРХ неё, а не вместо: иначе ухо
        // на первом же шаге встанет торчком.
        pose.euler[i] = rig.rest.euler[i]
                      + idle.euler[i] * (1.f - k) + walk.euler[i] * k;
    }

    if (st.attack > 0.01f) {
        for (u8 i = 0; i < rig.count && i < entity::MAX_PARTS; ++i)
            if (isArm(rig.parts[i].role))
                pose.euler[i].x -= st.attack * 1.1f;
    }
    if (st.death > 0.f) {
        // Заваливается набок, а не тонет в земле.
        const f32 t = st.death / 1.6f;
        const f32 fall = (1.f - (t < 0.f ? 0.f : (t > 1.f ? 1.f : t))) * 1.4f;
        for (u8 i = 0; i < rig.count && i < entity::MAX_PARTS; ++i)
            if (rig.parts[i].parent < 0) pose.euler[i].z += fall;
    }
}

} // namespace anim
