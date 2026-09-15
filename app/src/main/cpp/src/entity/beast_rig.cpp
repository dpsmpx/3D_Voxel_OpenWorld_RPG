/**
 * @file beast_rig.cpp
 * @brief Оснастка зверя: четвероногие и птицы.
 */
#include "beast_rig.h"

namespace entity {

namespace {

/// Нога из двух звеньев. Бедро вращается у верха, голень у колена:
/// поворот даёт дугу, а не параллельный сдвиг коробки.
void addLeg(Rig& rig, i8 parent, PartRole upper, PartRole lower,
            const glm::vec3& hip, f32 len, f32 thick, u32 color)
{
    const f32 half = len * 0.5f;

    Part up;
    up.parent    = parent;
    up.role      = upper;
    up.pivot     = hip;
    up.boxOffset = glm::vec3(0.f, -half * 0.5f, 0.f);
    up.size      = glm::vec3(thick, half, thick);
    up.color     = color;
    const u8 iUp = rig.add(up);

    Part lo;
    lo.parent    = (i8)iUp;
    lo.role      = lower;
    lo.pivot     = glm::vec3(0.f, -half, 0.f);
    lo.boxOffset = glm::vec3(0.f, -half * 0.5f, 0.f);
    lo.size      = glm::vec3(thick * 0.9f, half, thick * 0.9f);
    lo.color     = color;
    rig.add(lo);
}

} // namespace

Rig beastRig(const BeastSpec& spec) {
    Rig rig;

    // Корень: невидимый сустав в точке опоры.
    Part root;
    root.parent  = -1;
    root.role    = PartRole::Root;
    root.visible = false;
    const u8 iRoot = rig.add(root);

    // ---- Торс ----
    //
    // Сустав торса — на линии бёдер, чтобы качание корпуса в такт
    // шагу шло вокруг таза, а не вокруг центра коробки.
    const f32 hipY = spec.legLength;

    Part torso;
    torso.parent    = (i8)iRoot;
    torso.role      = PartRole::Torso;
    torso.pivot     = glm::vec3(0.f, hipY, 0.f);
    torso.boxOffset = glm::vec3(0.f, spec.bodyDepth * 0.5f, 0.f);
    torso.size      = glm::vec3(spec.bodyWidth, spec.bodyDepth, spec.bodyLength);
    torso.color     = spec.bodyColor;
    const i8 iTorso = (i8)rig.add(torso);

    // ---- Голова ----
    //
    // Ставится так, чтобы МАКУШКА пришлась ровно на заданный рост:
    // тогда модель и коллайдер одного размера по построению.
    const f32 headCenterY = spec.height - spec.headSize * 0.5f;
    const f32 headZ = spec.bodyLength * 0.5f + spec.headSize * 0.35f;

    Part head;
    head.parent    = iTorso;
    head.role      = PartRole::Head;
    head.pivot     = glm::vec3(0.f, headCenterY - hipY, headZ);
    head.boxOffset = glm::vec3(0.f);
    head.size      = glm::vec3(spec.headWidth, spec.headSize, spec.headSize);
    head.color     = spec.headColor;
    const i8 iHead = (i8)rig.add(head);

    // ---- Морда ----
    if (spec.snoutLen > 0.001f) {
        Part snout;
        snout.parent    = iHead;
        snout.role      = PartRole::Snout;
        snout.pivot     = glm::vec3(0.f, -spec.headSize * 0.18f,
                                    spec.headSize * 0.5f);
        snout.boxOffset = glm::vec3(0.f, 0.f, spec.snoutLen * 0.5f);
        snout.size      = glm::vec3(spec.headWidth * 0.55f,
                                    spec.headSize * 0.45f, spec.snoutLen);
        snout.color     = spec.accentColor;
        rig.add(snout);
    }

    // ---- Уши ----
    //
    // Торчком или висячие — по знаку earLean. Именно уши и делают
    // силуэт узнаваемым: волк с торчащими и овца с висячими
    // читаются по-разному даже издали.
    if (spec.earSize > 0.001f) {
        for (int side = -1; side <= 1; side += 2) {
            Part ear;
            ear.parent    = iHead;
            ear.role      = PartRole::Ear;
            ear.pivot     = glm::vec3(spec.headWidth * 0.35f * (f32)side,
                                      spec.headSize * 0.45f,
                                      -spec.headSize * 0.1f);
            ear.boxOffset = glm::vec3(0.f, spec.earSize * 0.5f, 0.f);
            ear.size      = glm::vec3(spec.earSize * 0.5f, spec.earSize,
                                      spec.earSize * 0.45f);
            ear.color     = spec.headColor;
            rig.add(ear);
        }
    }

    // ---- Рога ----
    if (spec.hornLen > 0.001f) {
        for (int side = -1; side <= 1; side += 2) {
            Part horn;
            horn.parent    = iHead;
            horn.role      = PartRole::Horn;
            horn.pivot     = glm::vec3(spec.headWidth * 0.45f * (f32)side,
                                       spec.headSize * 0.35f, 0.f);
            horn.boxOffset = glm::vec3(spec.hornLen * 0.5f * (f32)side, 0.f, 0.f);
            horn.size      = glm::vec3(spec.hornLen, spec.hornLen * 0.35f,
                                       spec.hornLen * 0.35f);
            horn.color     = spec.accentColor;
            rig.add(horn);
        }
    }

    // ---- Хвост ----
    if (spec.tailLength > 0.001f) {
        Part tail;
        tail.parent    = iTorso;
        tail.role      = PartRole::Tail;
        tail.pivot     = glm::vec3(0.f, spec.bodyDepth * 0.75f,
                                   -spec.bodyLength * 0.5f);
        tail.boxOffset = glm::vec3(0.f, 0.f, -spec.tailLength * 0.5f);
        tail.size      = glm::vec3(spec.tailThick, spec.tailThick,
                                   spec.tailLength);
        tail.color     = spec.bodyColor;
        rig.add(tail);
    }

    // ---- Ноги ----
    const f32 legX = spec.bodyWidth * 0.5f - spec.legThick * 0.5f;
    const f32 legZ = spec.bodyLength * 0.5f - spec.legThick;

    if (spec.legPairs >= 2) {
        addLeg(rig, iTorso, PartRole::UpperLegFR, PartRole::LowerLegFR,
               glm::vec3( legX, 0.f,  legZ), spec.legLength, spec.legThick,
               spec.legColor);
        addLeg(rig, iTorso, PartRole::UpperLegFL, PartRole::LowerLegFL,
               glm::vec3(-legX, 0.f,  legZ), spec.legLength, spec.legThick,
               spec.legColor);
        addLeg(rig, iTorso, PartRole::UpperLegBR, PartRole::LowerLegBR,
               glm::vec3( legX, 0.f, -legZ), spec.legLength, spec.legThick,
               spec.legColor);
        addLeg(rig, iTorso, PartRole::UpperLegBL, PartRole::LowerLegBL,
               glm::vec3(-legX, 0.f, -legZ), spec.legLength, spec.legThick,
               spec.legColor);
    } else {
        // Птица: одна пара, и стоит она под центром тяжести, а не
        // спереди — иначе курица падает носом вперёд.
        addLeg(rig, iTorso, PartRole::UpperLegFR, PartRole::LowerLegFR,
               glm::vec3( legX, 0.f, 0.f), spec.legLength, spec.legThick,
               spec.legColor);
        addLeg(rig, iTorso, PartRole::UpperLegFL, PartRole::LowerLegFL,
               glm::vec3(-legX, 0.f, 0.f), spec.legLength, spec.legThick,
               spec.legColor);
    }

    // Поза покоя задаёт постоянные углы: висячее ухо и поднятый хвост
    // — это НАКЛОН СУСТАВА, а не другая коробка. Поэтому они и
    // качаются потом вместе со всем остальным.
    for (u8 i = 0; i < rig.count; ++i) {
        if (rig.parts[i].role == PartRole::Ear)
            rig.rest.euler[i].x = spec.earLean;
        else if (rig.parts[i].role == PartRole::Tail)
            rig.rest.euler[i].x = spec.tailLift;
    }

    // ---- Рост: по макушке, а не по темени ----
    //
    // Голову ставили так, чтобы на заданную высоту пришёлся её верх.
    // Но силуэт — это то, что видно, а уши и рога торчат выше: овца
    // выходила 1.38 при коллайдере 1.20. Опускаем голову вместе со
    // всем, что на ней висит, на величину превышения. Голова у зверя
    // вынесена ВПЕРЁД от груди, поэтому в торс она при этом не
    // уходит.
    for (u8 i = 0; i < rig.count; ++i) {
        if (rig.parts[i].role != PartRole::Head) continue;
        const f32 excess = highestPoint(rig, rig.rest, 0.f) - spec.height;
        if (excess > 0.f) rig.parts[i].pivot.y -= excess;
        break;
    }

    rig.groundOffset = -lowestPoint(rig, rig.rest, 0.f);
    rig.strideLength = strideFromLegs(rig);
    return rig;
}

} // namespace entity
