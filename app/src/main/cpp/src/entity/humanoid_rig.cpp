/**
 * @file humanoid_rig.cpp
 * @brief Оснастка двуногого: одна для NPC, стражи и игрока.
 */
#include "humanoid_rig.h"

namespace entity {

namespace {

/// Добавить парную конечность из двух звеньев.
///
/// Верхнее звено вращается у своего сустава (плечо, бедро), нижнее —
/// у сгиба. Коробка висит НИЖЕ сустава: так поворот на угол даёт дугу,
/// а не параллельный сдвиг.
i8 addLimb(Rig& rig, i8 parent, PartRole upper, PartRole lower,
           const glm::vec3& joint, f32 upLen, f32 loLen,
           f32 thick, u32 color)
{
    Part up;
    up.parent    = parent;
    up.role      = upper;
    up.pivot     = joint;
    up.boxOffset = glm::vec3(0.f, -upLen * 0.5f, 0.f);
    up.size      = glm::vec3(thick, upLen, thick);
    up.color     = color;
    const u8 iUp = rig.add(up);

    Part lo;
    lo.parent    = (i8)iUp;
    lo.role      = lower;
    lo.pivot     = glm::vec3(0.f, -upLen, 0.f);
    lo.boxOffset = glm::vec3(0.f, -loLen * 0.5f, 0.f);
    // Голень чуть тоньше бедра: сгиб в колене иначе читается как
    // излом одной палки.
    lo.size      = glm::vec3(thick * 0.9f, loLen, thick * 0.9f);
    lo.color     = color;
    return (i8)rig.add(lo);
}

} // namespace

Rig humanoidRig(const HumanoidSpec& spec) {
    Rig rig;

    const f32 H = spec.height;
    const f32 legLen   = H * spec.legFrac;
    const f32 torsoLen = H * spec.torsoFrac;
    const f32 headLen  = H * spec.headFrac;

    const f32 shoulderW = H * spec.shoulderFrac;
    const f32 depth     = H * spec.depthFrac;
    const f32 limb      = H * spec.limbFrac;

    // Корень: невидимый сустав в точке опоры.
    Part root;
    root.parent  = -1;
    root.role    = PartRole::Root;
    root.visible = false;
    const u8 iRoot = rig.add(root);

    // Торс. Сустав — на высоте бедра; коробка уходит ВВЕРХ от него,
    // до плеч. Раньше коробка тела стояла центром в точке опоры, то
    // есть наполовину под землёй, а голова висела в воздухе выше — с
    // разрывом там, где полагалось быть груди.
    Part torso;
    torso.parent    = (i8)iRoot;
    torso.role      = PartRole::Torso;
    torso.pivot     = glm::vec3(0.f, legLen, 0.f);
    torso.boxOffset = glm::vec3(0.f, torsoLen * 0.5f, 0.f);
    torso.size      = glm::vec3(shoulderW, torsoLen, depth);
    torso.color     = spec.bodyColor;
    const i8 iTorso = (i8)rig.add(torso);

    // Голова: сустав на шее, коробка над ним.
    Part head;
    head.parent    = iTorso;
    head.role      = PartRole::Head;
    head.pivot     = glm::vec3(0.f, torsoLen, 0.f);
    head.boxOffset = glm::vec3(0.f, headLen * 0.5f, 0.f);
    head.size      = glm::vec3(headLen * 0.92f, headLen, headLen * 0.92f);
    head.color     = spec.headColor;
    rig.add(head);

    // Руки: плечи у верха торса, чуть внутрь от его края.
    const f32 armX = shoulderW * 0.5f + limb * 0.5f;
    const f32 armY = torsoLen - limb * 0.5f;
    // Звено руки в половину торса: вся рука выходит длиной с торс и
    // достаёт кончиками до бедра — так силуэт читается как человек.
    const f32 armHalf = torsoLen * 0.5f;
    const i8 iForearmR =
        addLimb(rig, iTorso, PartRole::UpperArmR, PartRole::LowerArmR,
                glm::vec3( armX, armY, 0.f), armHalf, armHalf, limb, spec.accentColor);
    addLimb(rig, iTorso, PartRole::UpperArmL, PartRole::LowerArmL,
            glm::vec3(-armX, armY, 0.f), armHalf, armHalf, limb, spec.accentColor);

    // Оружие в правой руке. Крепится к ПРЕДПЛЕЧЬЮ, а не к торсу:
    // иначе при замахе рука уходит, а клинок остаётся висеть.
    if (spec.weaponFrac > 0.f) {
        const f32 bladeLen = H * spec.weaponFrac;
        const f32 bladeTh  = H * spec.weaponThickFrac;
        Part w;
        w.parent    = iForearmR;
        w.role      = PartRole::Prop;
        // Сустав — в кисти, то есть у нижнего конца предплечья.
        w.pivot     = glm::vec3(0.f, -armHalf, 0.f);
        // Клинок смотрит вперёд и вниз от кисти: так он виден с
        // игровой камеры и не протыкает собственную ногу.
        w.boxOffset = glm::vec3(0.f, -bladeLen * 0.35f, bladeLen * 0.42f);
        w.size      = glm::vec3(bladeTh, bladeLen * 0.82f, bladeTh * 2.2f);
        w.color     = spec.weaponColor;
        rig.add(w);
    }

    // Ноги: бёдра у низа торса, то есть в его собственном нуле.
    const f32 legX = shoulderW * 0.25f;
    const f32 legHalf = legLen * 0.5f;
    addLimb(rig, iTorso, PartRole::UpperLegFR, PartRole::LowerLegFR,
            glm::vec3( legX, 0.f, 0.f), legHalf, legHalf, limb, spec.accentColor);
    addLimb(rig, iTorso, PartRole::UpperLegFL, PartRole::LowerLegFL,
            glm::vec3(-legX, 0.f, 0.f), legHalf, legHalf, limb, spec.accentColor);

    // Опора выводится из геометрии — тем же правилом, что у мобов.
    {
        rig.groundOffset = -lowestPoint(rig, rig.rest, 0.f);
        rig.strideLength = strideFromLegs(rig);
    }
    return rig;
}

} // namespace entity
