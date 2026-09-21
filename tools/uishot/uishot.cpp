/**
 * @file uishot.cpp
 * @brief Снимок интерфейса: треугольники UiSystem → PNG, без Vulkan.
 *
 * Зачем это есть. Интерфейс — единственная часть игры, которую
 * проверки щупают насквозь: раскладку, пересечения, обработчики,
 * касания, цвета. И при этом его ни разу никто не ВИДЕЛ. Три
 * итерации подряд кончались записью «глазами не проверено ничего»,
 * а последние две только интерфейс и трогали.
 *
 * Vulkan для этого не нужен вовсе. `UiSystem::buildFrame` собирает
 * кадр целиком на процессоре и отдаёт список треугольников; шрифт и
 * рамки лежат в атласе, который строится в памяти. Значит, всё, чего
 * не хватало, — это сотня строк растеризатора и уже написанный
 * кодировщик PNG.
 */
#include "ui/ui_system.h"
#include "ui/ui_atlas.h"
#include "render/iso_png.h"
#include "player/player.h"
#include "world/chunk_manager.h"
#include "world/block.h"
#include "items/item_def.h"
#include "mobs/mob_def.h"
#include "combat/weapon.h"
#include "quests/quest.h"
#include "quests/quest_def.h"
#include "ecs/registry.h"
#include "config/settings.h"

#include <cstdio>
#include <cstring>
#include <cmath>
#include <string>
#include <vector>
#include <algorithm>

namespace {

/// Холст RGBA8. Альфа нужна на время смешивания, наружу уходит RGB.
struct Canvas {
    u32 w = 0, h = 0;
    std::vector<u8> px;

    void reset(u32 W, u32 H, u32 rgb) {
        w = W; h = H;
        px.assign((usize)w * h * 4, 0);
        for (usize i = 0; i < (usize)w * h; ++i) {
            px[i * 4 + 0] = (u8)((rgb >> 16) & 0xFF);
            px[i * 4 + 1] = (u8)((rgb >>  8) & 0xFF);
            px[i * 4 + 2] = (u8)( rgb        & 0xFF);
            px[i * 4 + 3] = 255;
        }
    }

    void blend(u32 x, u32 y, f32 r, f32 g, f32 b, f32 a) {
        if (x >= w || y >= h || a <= 0.f) return;
        u8* p = &px[((usize)y * w + x) * 4];
        p[0] = (u8)std::min(255.f, r * a + (f32)p[0] * (1.f - a));
        p[1] = (u8)std::min(255.f, g * a + (f32)p[1] * (1.f - a));
        p[2] = (u8)std::min(255.f, b * a + (f32)p[2] * (1.f - a));
    }

    std::vector<u8> rgb() const {
        std::vector<u8> out((usize)w * h * 3);
        for (usize i = 0; i < (usize)w * h; ++i) {
            out[i * 3 + 0] = px[i * 4 + 0];
            out[i * 3 + 1] = px[i * 4 + 1];
            out[i * 3 + 2] = px[i * 4 + 2];
        }
        return out;
    }
};

/// Выборка из атласа — ближайший тексель, как и на видеокарте при
/// VK_FILTER_NEAREST.
///
/// Правило про отрицательную UV взято из `shaders/ui.frag` дословно:
/// «vUv.x < 0 значит без текстуры, сплошная заливка». Разойтись с
/// ним нельзя — снимок тогда показывал бы не то, что видит игрок.
/// Первый же прогон это и доказал: без этой ветки заливки исчезли
/// все до одной, а буквы остались.
struct Atlas {
    ui::UiAtlasData d = ui::buildUiAtlas();

    void sample(f32 u, f32 v, f32 out[4]) const {
        if (u < 0.f) {
            out[0] = out[1] = out[2] = 255.f;
            out[3] = 1.f;
            return;
        }
        const i32 x = std::clamp((i32)(u * (f32)d.width),  0, (i32)d.width  - 1);
        const i32 y = std::clamp((i32)(v * (f32)d.height), 0, (i32)d.height - 1);
        const u8* p = &d.pixels[((usize)y * d.width + x) * 4];
        out[0] = (f32)p[0]; out[1] = (f32)p[1];
        out[2] = (f32)p[2]; out[3] = (f32)p[3] / 255.f;
    }
};

/// Вершина интерфейса приходит в координатах ОТСЕЧЕНИЯ, а не в
/// точках экрана: -1..1 по обеим осям, Y вниз. Это ровно то, что
/// ждёт видеокарта, и ровно то, обо что спотыкается всякий, кто
/// решит, будто там пиксели.
struct Screen {
    f32 w = 0.f, h = 0.f;
    glm::vec2 px(glm::vec2 clip) const {
        return { (clip.x * 0.5f + 0.5f) * w, (clip.y * 0.5f + 0.5f) * h };
    }
};

/// Треугольник по барицентрическим координатам.
///
/// Общий растеризатор, а не «заливка прямоугольника»: интерфейс
/// отдаёт треугольники, и складывать их обратно в прямоугольники
/// значило бы проверять свою догадку о том, что он рисует, вместо
/// того, что он рисует на самом деле.
void triangle(Canvas& c, const Atlas& at, const Screen& sc,
              const ui::UiVertex& av, const ui::UiVertex& bv,
              const ui::UiVertex& cv)
{
    struct P { glm::vec2 pos; };
    const P a{ sc.px(av.pos) }, b{ sc.px(bv.pos) }, cc{ sc.px(cv.pos) };

    const f32 minX = std::floor(std::min({ a.pos.x, b.pos.x, cc.pos.x }));
    const f32 maxX = std::ceil (std::max({ a.pos.x, b.pos.x, cc.pos.x }));
    const f32 minY = std::floor(std::min({ a.pos.y, b.pos.y, cc.pos.y }));
    const f32 maxY = std::ceil (std::max({ a.pos.y, b.pos.y, cc.pos.y }));

    const f32 area = (b.pos.x - a.pos.x) * (cc.pos.y - a.pos.y) -
                     (b.pos.y - a.pos.y) * (cc.pos.x - a.pos.x);
    if (std::fabs(area) < 1e-6f) return;

    for (i32 y = (i32)minY; y <= (i32)maxY; ++y) {
        for (i32 x = (i32)minX; x <= (i32)maxX; ++x) {
            const f32 px = (f32)x + 0.5f, py = (f32)y + 0.5f;
            f32 w0 = ((b.pos.x - a.pos.x) * (py - a.pos.y) -
                      (b.pos.y - a.pos.y) * (px - a.pos.x)) / area;
            f32 w1 = ((cc.pos.x - b.pos.x) * (py - b.pos.y) -
                      (cc.pos.y - b.pos.y) * (px - b.pos.x)) / area;
            f32 w2 = 1.f - w0 - w1;
            // w0 — вес вершины cc, w1 — вершины a, w2 — вершины b.
            if (w0 < 0.f || w1 < 0.f || w2 < 0.f) continue;

            const f32 u = av.uv.x * w1 + bv.uv.x * w2 + cv.uv.x * w0;
            const f32 v = av.uv.y * w1 + bv.uv.y * w2 + cv.uv.y * w0;
            f32 tex[4];
            at.sample(u, v, tex);

            const f32 vr = (f32)av.r * w1 + (f32)bv.r * w2 + (f32)cv.r * w0;
            const f32 vg = (f32)av.g * w1 + (f32)bv.g * w2 + (f32)cv.g * w0;
            const f32 vb = (f32)av.b * w1 + (f32)bv.b * w2 + (f32)cv.b * w0;
            const f32 va = ((f32)av.a * w1 + (f32)bv.a * w2 + (f32)cv.a * w0) / 255.f;

            c.blend((u32)x, (u32)y,
                    vr * tex[0] / 255.f, vg * tex[1] / 255.f, vb * tex[2] / 255.f,
                    va * tex[3]);
        }
    }
}

struct Opts {
    i32 w = 1920, h = 1080, dpi = 420;
    bool mirror = false;
    /// Какой экран снимать. HUD — по умолчанию; журнал заданий —
    /// потому что он обещает игроку награду, и обещание надо
    /// прочитать глазами, а не поверить коду на слово.
    ui::Screen screen = ui::Screen::Hud;
    bool target = false;          ///< показать полосу цели
    f32  targetFill = 0.42f;
    f32  targetGhost = 0.63f;
    u8   targetPhases = 1;
    const char* targetName = "Stone Warden";
    u32  background = 0x5A6A78u;  ///< «серый день»: не белое и не чёрное
    std::string out = "build/uishot/hud.png";
};

} // namespace

int main(int argc, char** argv) {
    Opts o;
    for (int i = 1; i < argc; ++i) {
        const std::string k = argv[i];
        auto next = [&]() { return (i + 1 < argc) ? argv[++i] : ""; };
        if      (k == "--w")      o.w = std::atoi(next());
        else if (k == "--h")      o.h = std::atoi(next());
        else if (k == "--dpi")    o.dpi = std::atoi(next());
        else if (k == "--mirror") o.mirror = true;
        else if (k == "--screen") {
            const std::string v = next();
            if      (v == "hud")    o.screen = ui::Screen::Hud;
            else if (v == "quests") o.screen = ui::Screen::QuestLog;
            else { std::printf("uishot: неизвестный экран «%s»\n", v.c_str()); return 1; }
        }
        else if (k == "--target") o.target = true;
        else if (k == "--fill")   o.targetFill = (f32)std::atof(next());
        else if (k == "--ghost")  o.targetGhost = (f32)std::atof(next());
        else if (k == "--phases") o.targetPhases = (u8)std::atoi(next());
        else if (k == "--name")   o.targetName = next();
        else if (k == "--bg")     o.background = (u32)std::strtoul(next(), nullptr, 16);
        else if (k == "--out")    o.out = next();
    }

    world::blocks();
    items::items();
    mobs::mobRegistry();
    combat::weapons();

    ecs::Registry reg;
    player::Player pl;
    pl.init(reg, { 0.f, 40.f, 0.f });
    world::ChunkManager wd(0x5150Bull, 1);

    // Через настоящую настройку, а не через флаг мимо неё: снимок
    // обязан показывать то, что увидит игрок, переключивший тумблер.
    config::settings() = config::Settings{};
    config::settings().joystickLeftHanded = o.mirror;

    ui::UiSystem sys;
    sys.setDensityDpi(o.dpi);
    sys.setScreenSize(o.w, o.h);
    sys.screen = o.screen;

    // Журнал без заданий показывать нечего, а цель снимка —
    // прочитать строку награды. Задание собирается тем же
    // генератором, каким его получает игрок: снимок обязан показывать
    // настоящие тексты и настоящие числа, а не подставные.
    if (o.screen == ui::Screen::QuestLog) {
        quests::Quest q{};
        q.id = quests::nextQuestId();
        q.tmpl.type = quests::QuestType::Collect;
        q.tmpl.targetBlockId = world::WOOD;
        q.tmpl.requiredCount = 8;
        q.tmpl.difficulty = quests::QuestDifficulty::Hard;
        q.progress = 5;
        q.state = quests::QuestState::Active;
        q.ownerEntity = (u32)pl.entity();
        q.rewards.xp = 240;
        q.rewards.gold = 120;
        q.rewards.itemBlockId = world::IRON_ORE;
        q.rewards.itemCount = 3;
        std::snprintf(q.title, sizeof(q.title), "Wood for the Palisade");
        std::snprintf(q.description, sizeof(q.description),
                      "The village needs 8 wood to close the north gap "
                      "before nightfall.");
        const ecs::Entity qe = reg.create();
        reg.add(qe, q);
        if (auto* log = pl.questLog()) log->addActive(qe);
        sys.selectedQuest = 0;
    }

    if (o.target) {
        sys.target.name   = o.targetName;
        sys.target.fill   = o.targetFill;
        sys.target.ghost  = o.targetGhost;
        sys.target.alpha  = 1.f;
        sys.target.phases = o.targetPhases;
        sys.target.phase  = 0;
    }

    sys.tickUi(1.f / 60.f);
    sys.buildFrame(pl, wd, 60.f);

    const std::vector<ui::UiVertex>& v = sys.frameVertices();
    if (v.size() < 3) {
        std::printf("uishot: кадр пуст — рисовать нечего\n");
        return 1;
    }

    Canvas c;
    c.reset((u32)o.w, (u32)o.h, o.background);
    Atlas at;
    const Screen sc{ (f32)o.w, (f32)o.h };
    for (usize i = 0; i + 2 < v.size(); i += 3)
        triangle(c, at, sc, v[i], v[i + 1], v[i + 2]);

    const std::vector<u8> rgb = c.rgb();
    if (!render::writePngFile(o.out.c_str(), rgb.data(), (u32)o.w, (u32)o.h)) {
        std::printf("uishot: не записался %s\n", o.out.c_str());
        return 1;
    }
    std::printf("uishot: %dx%d @%d%s%s%s -> %s (треугольников %u)\n",
                o.w, o.h, o.dpi, o.mirror ? ", левша" : "",
                o.target ? ", с полосой цели" : "",
                o.screen == ui::Screen::QuestLog ? ", журнал заданий" : "",
                o.out.c_str(), (u32)(v.size() / 3));
    return 0;
}
