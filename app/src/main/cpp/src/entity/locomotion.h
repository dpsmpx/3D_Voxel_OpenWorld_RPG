/**
 * @file locomotion.h
 * @brief Поза сущности: качание суставов по фазе шага.
 */
#pragma once
#include "rig.h"
#include <algorithm>
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
/// Каким движением существо бьёт.
///
/// Десять классов оружия делали одно и то же движение — обе руки
/// одинаково вперёд, — и отличались только длительностью. В третьем
/// лице, где модель занимает середину экрана, это и есть главный
/// признак прототипа: по кинжалу и двуручному топору не видно, что
/// это разное оружие.
///
/// Форма — свойство ОРУЖИЯ и ВИДА, а не ветка в рендере: она лежит в
/// WeaponDef и MobDef, и одиннадцатое оружие не требует одиннадцатой
/// ветки.
enum class AttackShape : u8 {
    None = 0,
    Slash,    ///< меч: за плечо и наотмашь поперёк тела
    Chop,     ///< топор: над головой вниз, всем корпусом
    Thrust,   ///< копьё: локоть назад, выпад прямо вперёд
    Stab,     ///< кинжал: короткий тычок от бедра
    Draw,     ///< лук и арбалет: держать цевьё, тянуть тетиву
    Cast,     ///< посох и жезл: поднять ладонь, толчок вперёд
    Bite,     ///< зверь: осесть на задние, бросок пастью вперёд
};

struct AnimState {
    f32 phase     = 0.f;   ///< фаза шага, радианы; идёт ПУТЁМ
    f32 time      = 0.f;   ///< монотонное время, секунды; для дыхания
    f32 speedNorm = 0.f;   ///< скорость к максимальной, 0..1

    /// Где сейчас удар: −1 рука отведена в замахе, 0 покой,
    /// +1 рука прошла сквозь цель.
    ///
    /// ЗНАКОВОЕ, и это главное. Раньше здесь была доля 0..1, а весь
    /// удар был «рука вперёд и обратно»: замаха не существовало ни в
    /// позе, ни во времени. Замах — это ПРОТИВОХОД: сперва назад,
    /// потом сквозь. Одним числом, потому что это одна величина —
    /// положение руки на дуге удара.
    f32 attack    = 0.f;

    /// По какой дуге. Значимо только при attack != 0.
    AttackShape attackShape = AttackShape::None;
    f32 death     = 0.f;   ///< секунды с момента смерти, 0 — жив
    f32 air       = 0.f;   ///< доля воздушной позы, 0..1
    f32 land      = 0.f;   ///< доля посадочной позы, 0..1
    f32 rise      = 0.f;   ///< -1 падает, +1 взлетает; форма воздушной позы
};

/// Ниже этой доли максимальной скорости существо идёт, выше — бежит.
///
/// Бег отличается не только частотой шага: корпус наклоняется вперёд.
/// Без наклона шаг и бег выглядят одним движением, прокрученным с
/// разной скоростью.
constexpr f32 RUN_THRESHOLD = 0.6f;

/// Сколько длится поза приземления.
constexpr f32 LAND_TIME = 0.28f;

/// За сколько нарастает и гаснет воздушная поза.
constexpr f32 AIR_BLEND_TIME = 0.12f;

/// Продвинуть состояние передвижения.
///
/// Состояние выбирается по ФАКТИЧЕСКОЙ скорости и признаку опоры.
/// Переходы плавные: доли воздушной и посадочной поз нарастают и
/// гаснут за своё время, а не переключаются кадром.
template <class LocoT>
inline void advanceLocomotion(LocoT& lo, f32 speedNorm, f32 verticalSpeed,
                              bool grounded, f32 dt)
{
    using State = typename LocoT::State;

    const State was = lo.state;
    const bool wasAir = (was == State::Jump || was == State::Fall);

    if (!grounded) {
        lo.state = (verticalSpeed > 0.5f) ? State::Jump : State::Fall;
    } else if (wasAir) {
        // Коснулись земли — приседаем. Без этого падение с высоты
        // кончается тем, что существо просто продолжает идти.
        lo.state = State::Land;
    } else if (was == State::Land && lo.stateTime < LAND_TIME) {
        lo.state = State::Land;
    } else if (speedNorm < 0.05f) {
        lo.state = State::Idle;
    } else {
        lo.state = (speedNorm < RUN_THRESHOLD) ? State::Walk : State::Run;
    }

    lo.stateTime = (lo.state == was) ? lo.stateTime + dt : 0.f;
    lo.grounded  = grounded;

    const bool air = (lo.state == State::Jump || lo.state == State::Fall);
    const f32 step = dt / AIR_BLEND_TIME;
    lo.air  = air ? std::min(1.f, lo.air + step) : std::max(0.f, lo.air - step);

    if (lo.state == State::Land) {
        // Приседание глубже всего в момент касания и распрямляется.
        const f32 t = lo.stateTime / LAND_TIME;
        lo.land = (t >= 1.f) ? 0.f : (1.f - t) * (1.f - t);
    } else {
        lo.land = std::max(0.f, lo.land - dt / LAND_TIME);
    }

    lo.rise = grounded ? 0.f
                       : (verticalSpeed > 0.5f ? 1.f
                          : (verticalSpeed < -0.5f ? -1.f : 0.f));
}

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

/// Правая рука бьющая: к её предплечью крепится оружие (см.
/// humanoidRig). Левая работает противовесом.
inline bool isRightArm(entity::PartRole r) {
    return r == entity::PartRole::UpperArmR ||
           r == entity::PartRole::LowerArmR ||
           r == entity::PartRole::HandR;
}
inline bool isLeftArm(entity::PartRole r) {
    return r == entity::PartRole::UpperArmL ||
           r == entity::PartRole::LowerArmL ||
           r == entity::PartRole::HandL;
}
inline bool isFrontLeg(entity::PartRole r) {
    return r == entity::PartRole::UpperLegFL ||
           r == entity::PartRole::UpperLegFR;
}

/// Дуга удара, положенная поверх позы.
///
/// Оси проверены численно на humanoidRig и означают буквально
/// следующее (сустав плеча, рука висит вниз):
///
///   * `x > 0` — кисть уходит НАЗАД (−Z), `x < 0` — ВПЕРЁД (+Z);
///   * `z > 0` — кисть уходит НАРУЖУ (+X, вправо от тела),
///     `z < 0` — ПОПЕРЁК тела, влево;
///   * `y` у висящей руки не делает ничего: ось вращения совпадает
///     с самой рукой. Поэтому горизонтальный замах строится из X и
///     Z, а не из Y.
///
/// `a` — положение на дуге, −1 отведено, +1 прошло сквозь цель.
inline void attackPose(const entity::Rig& rig, entity::Pose& pose,
                       f32 a, AttackShape shape)
{
    if (shape == AttackShape::None) return;
    const f32 draw   = a < 0.f ? -a : 0.f;   // отведено под удар
    const f32 strike = a > 0.f ?  a : 0.f;   // прошло сквозь цель
    if (draw < 0.01f && strike < 0.01f) return;
    const f32 hold = draw + strike;          // «рука занята ударом»

    for (u8 i = 0; i < rig.count && i < entity::MAX_PARTS; ++i) {
        const entity::PartRole r = rig.parts[i].role;
        glm::vec3& e = pose.euler[i];
        const bool right = isRightArm(r);
        const bool left  = isLeftArm(r);
        const bool torso = (r == entity::PartRole::Torso);
        const bool head  = (r == entity::PartRole::Head);

        switch (shape) {

        // Меч: рука уходит за правое плечо и проносится поперёк тела.
        case AttackShape::Slash:
            if (right)      { e.x += 0.50f * draw - 0.70f * strike;
                              e.z += 1.15f * draw - 0.95f * strike; }
            else if (left)  { e.x -= 0.30f * draw - 0.20f * strike; }
            else if (torso) { e.y += 0.28f * draw - 0.42f * strike; }
            break;

        // Топор: над головой и вниз. Корпус отклоняется назад в
        // замахе и падает вперёд вместе с ударом — вес виден в
        // корпусе, а не в руке.
        case AttackShape::Chop:
            if (right)      { e.x += 1.55f * draw - 1.15f * strike; }
            else if (left)  { e.x += 0.70f * draw - 0.45f * strike; }
            else if (torso) { e.x -= 0.18f * draw - 0.34f * strike; }
            else if (head)  { e.x -= 0.10f * draw - 0.18f * strike; }
            break;

        // Копьё: локоть уходит далеко назад, корпус поворачивается
        // боком, выпад идёт по прямой и дальше всех.
        case AttackShape::Thrust:
            if (right)      { e.x += 0.90f * draw - 1.30f * strike; }
            else if (left)  { e.x -= 0.55f * hold; }
            else if (torso) { e.y += 0.34f * draw - 0.30f * strike;
                              e.x -= 0.22f * strike; }
            break;

        // Кинжал: коротко и близко. Замах едва заметен — это и есть
        // его характер: бьёт раньше, чем видно.
        case AttackShape::Stab:
            if (right)      { e.x += 0.32f * draw - 0.85f * strike; }
            else if (left)  { e.x -= 0.25f * hold; }
            else if (torso) { e.x -= 0.14f * strike; }
            break;

        // Лук: левая держит цевьё вытянутой ВСЁ время выстрела,
        // правая тянет тетиву к щеке и срывается с неё.
        case AttackShape::Draw:
            if (left)       { e.x -= 1.30f * hold; }
            else if (right) { e.x -= 0.90f * hold;
                              e.z -= 0.55f * draw - 0.15f * strike; }
            else if (torso) { e.y += 0.30f * hold; }
            break;

        // Посох: ладонь поднимается вверх-наружу и толкает вперёд.
        case AttackShape::Cast:
            if (right)      { e.x -= 0.35f * draw + 0.95f * strike;
                              e.z += 1.25f * draw - 0.60f * strike; }
            else if (left)  { e.x -= 0.30f * hold; }
            else if (torso) { e.x -= 0.12f * draw - 0.16f * strike; }
            break;

        // Зверь: приседает на передние, потом бросок всем телом.
        // Рук у него нет — работают голова, корпус и передние ноги.
        case AttackShape::Bite:
            if (head)            { e.x += 0.40f * draw - 0.60f * strike; }
            else if (torso)      { e.x -= 0.16f * draw - 0.30f * strike; }
            else if (isFrontLeg(r)) { e.x -= 0.55f * draw - 0.30f * strike; }
            else if (right || left) { e.x += 0.45f * draw - 0.80f * strike; }
            break;

        case AttackShape::None:
        default:
            break;
        }
    }
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

    // ---- Бег: корпус вперёд ----
    //
    // Иначе шаг и бег — одно движение с разной частотой. Наклон
    // растёт только выше порога бега, чтобы ходьба его не получала.
    if (st.speedNorm > RUN_THRESHOLD) {
        const f32 runK = (st.speedNorm - RUN_THRESHOLD) / (1.f - RUN_THRESHOLD);
        for (u8 i = 0; i < rig.count && i < entity::MAX_PARTS; ++i)
            if (rig.parts[i].role == entity::PartRole::Torso)
                pose.euler[i].x += runK * 0.22f;
    }

    // ---- Воздух ----
    //
    // Поджатые ноги и разведённые руки. Раньше существо падало с
    // обрыва, перебирая ногами по воздуху: воздушного положения не
    // было вовсе.
    if (st.air > 0.001f) {
        // Взлетая, ноги поджимаются вперёд; падая — вытягиваются
        // вниз-назад, навстречу земле.
        const f32 tuck = (st.rise >= 0.f) ? 0.7f : -0.35f;
        for (u8 i = 0; i < rig.count && i < entity::MAX_PARTS; ++i) {
            const entity::PartRole r = rig.parts[i].role;
            if (isUpperLimb(r))
                pose.euler[i].x = pose.euler[i].x * (1.f - st.air)
                                + tuck * st.air;
            else if (isLowerLimb(r))
                pose.euler[i].x = pose.euler[i].x * (1.f - st.air)
                                + kneeBendSign(r) * 0.8f * st.air;
            else if (isArm(r))
                pose.euler[i].x = pose.euler[i].x * (1.f - st.air)
                                - 0.9f * st.air;
        }
    }

    // ---- Приземление ----
    //
    // Присед в момент касания, распрямляющийся за LAND_TIME. Гасит
    // падение: без него удар о землю ничем не отмечен.
    if (st.land > 0.001f) {
        for (u8 i = 0; i < rig.count && i < entity::MAX_PARTS; ++i) {
            const entity::PartRole r = rig.parts[i].role;
            if (isUpperLimb(r))
                pose.euler[i].x += 0.45f * st.land;
            else if (isLowerLimb(r))
                pose.euler[i].x += kneeBendSign(r) * 0.75f * st.land;
            else if (r == entity::PartRole::Torso)
                pose.euler[i].x += 0.25f * st.land;
        }
    }

    attackPose(rig, pose, st.attack, st.attackShape);
    if (st.death > 0.f) {
        // Заваливается набок, а не тонет в земле.
        const f32 t = st.death / 1.6f;
        const f32 fall = (1.f - (t < 0.f ? 0.f : (t > 1.f ? 1.f : t))) * 1.4f;
        for (u8 i = 0; i < rig.count && i < entity::MAX_PARTS; ++i)
            if (rig.parts[i].parent < 0) pose.euler[i].z += fall;
    }
}

} // namespace anim
