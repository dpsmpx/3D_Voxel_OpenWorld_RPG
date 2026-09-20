/**
 * @file lights.h
 * @brief Точечные источники света: факелы, фонари, лава, огонь в руке.
 */
#pragma once
#include "../core/types.h"
#include "../world/chunk_manager.h"
#include "../world/block.h"
#include <glm/glm.hpp>
#include <vector>

namespace render {

/// Точечный источник света.
struct PointLight {
    glm::vec3 pos{0.f};
    /// Цвет в sRGB, как задан у блока. В линейное переводит камера —
    /// там же, где и остальной свет, одним правилом на всех.
    glm::vec3 color{1.f};
    /// Где свет гаснет полностью, в блоках.
    f32 radius = 8.f;
    /// Яркость в центре.
    f32 power = 1.f;
};

/// Сколько источников уходит на GPU за кадр.
///
/// Каждый — это ветка в фрагментном шейдере террейна, самом дорогом
/// в кадре. Восьми хватает на освещённую комнату и на коридор с
/// факелами по стенам; дальние всё равно не видно, а ближние
/// выбираются по расстоянию.
constexpr u32 MAX_GPU_LIGHTS = 8;

/// Источники света вокруг игрока.
///
/// Блоки-светильники уже были: у BlockDef есть isEmissive и
/// lightLevel, LANTERN объявлен с уровнем 13, а деревни его ставят в
/// дома. Только читать эти поля было некому — они заполнялись и не
/// спрашивались НИ РАЗУ. Ночью и в пещере фонарь светил ровно
/// столько же, сколько булыжник.
///
/// Здесь они собираются в список, который каждый кадр уезжает в
/// камеру. Мир перебирается не каждый кадр: светильники не
/// двигаются, и пересобирать список чаще, чем игрок проходит пару
/// блоков, незачем.
class LightField {
public:
    /// Радиус поиска светильников вокруг игрока, блоков.
    static constexpr i32 SCAN_RADIUS_XZ = 14;
    static constexpr i32 SCAN_RADIUS_Y  = 10;
    /// Как часто перебирается мир, даже если игрок стоит на месте.
    static constexpr f32 RESCAN_SEC = 1.0f;
    /// Сдвинулся на столько блоков — пересобрать сразу.
    static constexpr f32 RESCAN_MOVE = 4.f;

    /// Пересобрать список блоков-светильников, если пора.
    void scan(world::ChunkManager& world, const glm::vec3& around, f32 dt);

    /// Подвижный огонь на ЭТОТ кадр: факел в руке, горящий снаряд.
    /// Живёт до следующего beginFrame.
    void addTransient(const PointLight& l);

    /// Начало кадра: подвижные огни прошлого кадра забываются.
    void beginFrame() { transient_.clear(); }

    /// Ближайшие к точке источники. Возвращает, сколько записал.
    u32 nearest(const glm::vec3& to, PointLight* out, u32 maxOut) const;

    usize blockLightCount() const { return blocks_.size(); }
    usize transientCount()  const { return transient_.size(); }
    void  clear() { blocks_.clear(); transient_.clear(); scanned_ = false; }

    /// Источник, которым светит этот блок. power = 0 — не светит.
    ///
    /// В заголовке, а не в .cpp, и это не про скорость: тем же
    /// правилом пользуется vkcheck, а он собирается из горстки файлов
    /// и обхода чанков не линкует вовсе. Выписать там «факел светит
    /// на пятнадцать блоков» числами значило бы завести вторую
    /// правду о свете.
    static PointLight lightOfBlock(u16 block, const glm::ivec3& at) {
        PointLight l;
        l.power = 0.f;
        if (block == world::AIR || block == world::UNKNOWN) return l;

        const world::BlockDef& d = world::blocks().get(block);
        if (!d.emitsLight || d.lightLevel == 0) return l;

        // Цвет своего же огня. Отдельного поля «цвет свечения»
        // заводить незачем: факел светит цветом пламени, лава —
        // цветом лавы, и если их развести, они разойдутся.
        const world::BlockColor c = d.colorTop;
        l.color = glm::vec3((f32)((c >> 24) & 0xFFu),
                            (f32)((c >> 16) & 0xFFu),
                            (f32)((c >>  8) & 0xFFu)) / 255.f;

        // Центр блока: свет идёт из него, а не из угла.
        l.pos = glm::vec3((f32)at.x + 0.5f, (f32)at.y + 0.5f, (f32)at.z + 0.5f);

        // Уровень 0..15 — это и дальность, и яркость. Пятнадцатый
        // светит на шестнадцать блоков, как и положено полному.
        const f32 lvl = (f32)d.lightLevel;
        l.radius = 1.f + lvl;
        l.power  = lvl / 15.f;
        return l;
    }

    /// Огонь в руке: тот же блок, но несомый, а не вкопанный.
    ///
    /// `feet` — точка опоры того, кто несёт. Светит из кулака, а не
    /// из ступней: иначе пол под ногами выжжен добела, а стена перед
    /// носом черна. И чуть слабее вкопанного — пламя прикрыто
    /// ладонью; это единственное, чем «нести» отличается от
    /// «поставить».
    static PointLight heldLight(u16 block, const glm::vec3& feet) {
        PointLight l = lightOfBlock(block, glm::ivec3(0));
        if (l.power <= 0.f) return l;
        l.pos = feet + glm::vec3(0.f, HAND_HEIGHT, 0.f);
        l.power *= HAND_DIM;
        return l;
    }

    /// На какой высоте от ступней горит несомый факел.
    static constexpr f32 HAND_HEIGHT = 1.25f;
    /// Во сколько раз несомый слабее вкопанного.
    static constexpr f32 HAND_DIM    = 0.85f;

private:
    std::vector<PointLight> blocks_;
    std::vector<PointLight> transient_;
    glm::vec3 lastScan_{0.f};
    f32  timer_   = 0.f;
    bool scanned_ = false;
};

} // namespace render
