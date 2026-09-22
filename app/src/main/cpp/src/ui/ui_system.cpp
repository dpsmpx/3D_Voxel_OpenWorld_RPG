/**
 * @file ui_system.cpp
 * @brief Интерфейс: immediate-mode UI поверх Vulkan, HUD, меню.
 */
#include "ui_system.h"
#include "hud_layout.h"
#include "slider.h"
#include "hud_resources.h"
#include "font_data.h"
#include "../combat/weapon.h"
#include "../combat/resonance.h"
#include "../combat/enchantment.h"
#include "../combat/components.h"
#include "../combat/hurt_marks.h"
#include "../progression/progression.h"
#include "../progression/skill_tree.h"
#include "../quests/quest.h"
#include "../quests/quest_def.h"
#include "../npc/dialogue.h"
#include "../npc/npc_def.h"
#include "../factions/faction.h"
#include "../items/item_def.h"
#include "../items/item_use.h"
#include "../items/item_pickup.h"
#include "../trade/trade.h"
#include "../world/enchant_altar.h"
#include "../world/world_spec.h"
#include "../config/settings.h"
#include "../config/localization.h"
#include "../audio/audio_events.h"
#include "../ecs/components.h"
#include "../core/log.h"
#include <cstdio>
#include <cstring>
#include <ctime>
#include <algorithm>
#include <cmath>
#include <glm/glm.hpp>
#include <functional>
#include <string>
#include <vector>

namespace ui {

using namespace progression;
namespace cfg = config;
using cfg::StrKey;
using cfg::T;

static const char* gMonthNames[12] = {
    "Jan","Feb","Mar","Apr","May","Jun",
    "Jul","Aug","Sep","Oct","Nov","Dec"
};

// ============================================================
// Init
// ============================================================
bool UiSystem::init(vk::Context& ctx, AAssetManager* mgr) {
    dev_ = ctx.device();
    if (!renderer_.init(ctx, mgr)) {
        LOGE("UiRenderer init failed");
        return false;
    }
    ui_.init(&renderer_, screenW_, screenH_);
    LOGI("UiSystem готов (Phase 13)");
    return true;
}

void UiSystem::setScreenSize(i32 w, i32 h) {
    screenW_ = w;
    screenH_ = h;
    // Привязка к сборщику вершин — ЗДЕСЬ, а не в init(): тот требует
    // устройство.
    //
    // buildFrame() обещает в своём же комментарии, что Vulkan в
    // построении кадра не участвует. Обещание было половинчатым: без
    // init() у контекста не было сборщика, и каждый примитив молча
    // уходил в никуда. Кадр строился — пустой. Заметить это было
    // нечем: проверки, зовущие buildFrame, щупали области касания, а
    // они собираются другим списком и работали.
    ui_.init(&renderer_, w, h);
    rebuildLayout();
}

void UiSystem::setDensityDpi(i32 dpi) {
    densityDpi_ = dpi;
    rebuildLayout();
}

/// Раскладка пересобирается только здесь: и отрисовка, и касание
/// обязаны видеть одни и те же числа. Раньше каждая сторона считала
/// свои, и на 1280x720 они расходились на 191 точку.
void UiSystem::rebuildLayout() {
    const auto m = theme::Metrics::fromDensityDpi(
        densityDpi_, cfg::settingsConst().uiScale);
    // Безопасная зона пока нулевая: система сама держит окно вне
    // выреза, пока манифест не просит SHORT_EDGES. Место для неё
    // заведено, чтобы правка была в одном месте, а не в четырнадцати.
    // Зеркало — из настройки, а не «всегда нет».
    //
    // HudLayout умеет отражать весь экран, у этого есть своя ветка в
    // проверке раскладки и подробное объяснение, почему иначе «PUT»
    // приезжает под столбец навигации. Настройка `joystickLeftHanded`
    // есть, сохраняется и переключается тумблером — а сюда не
    // доходила: до сих пор она двигала один джойстик. Левша
    // переключал тумблер, палка переезжала, а кнопки оставались на
    // месте.
    layout_ = HudLayout((f32)screenW_, (f32)screenH_, m, SafeInsets{},
                        cfg::settingsConst().joystickLeftHanded);
}

bool UiSystem::routeTouch(i32 id, float px, float py, int phase) {
    // Передаём в ui-context (для активного указателя и hit-тестов).
    return ui_.handleTouch(id, px, py, phase);
}

void UiSystem::refreshSlotMeta(save::SaveSlotManager& mgr) {
    save::SlotMeta meta[save::SaveSlotManager::NUM_PROFILES][save::SaveSlotManager::NUM_SLOTS];
    mgr.scanAll(meta);
    for (u32 p = 0; p < save::SaveSlotManager::NUM_PROFILES; ++p)
        for (u32 s = 0; s < save::SaveSlotManager::NUM_SLOTS; ++s)
            slotMeta[p][s] = meta[p][s];
}

// ============================================================
// Уведомления
// ============================================================
//
// Слот был один, и новое сообщение затирало предыдущее: «предмет
// получен» стирало «задание выполнено». Теперь очередь с приоритетом.
void UiSystem::notify(const std::string& text, theme::NotifyPriority p) {
    if (text.empty()) return;

    // Повтор того же текста не множится, а продлевается: подбор
    // десяти предметов подряд не должен занимать весь экран.
    for (auto& n : notices_) {
        if (n.text != text) continue;
        n.timeLeft = theme::notifyDuration(p);
        n.priority = p;
        return;
    }

    notices_.push_back({ text, p, theme::notifyDuration(p), 0.f });

    // Важное вперёд. Порядок устойчивый: при равном приоритете
    // остаётся тот, что пришёл раньше.
    std::stable_sort(notices_.begin(), notices_.end(),
                     [](const Notice& a, const Notice& b) {
                         return (u8)a.priority > (u8)b.priority;
                     });

    // Очередь не растёт без предела: лишнее рядовое отбрасывается,
    // а важное — нет.
    constexpr usize CAP = 8;
    if (notices_.size() > CAP) notices_.resize(CAP);
}

void UiSystem::tickUi(f32 dt) {
    for (auto& n : notices_) { n.timeLeft -= dt; n.age += dt; }
    notices_.erase(std::remove_if(notices_.begin(), notices_.end(),
                                  [](const Notice& n) { return n.timeLeft <= 0.f; }),
                   notices_.end());

    craftScroll.tick(dt);
    enchantScroll.tick(dt);
    saveScroll.tick(dt);
    skillScroll.tick(dt);
    pauseScroll.tick(dt);
    questScroll.tick(dt);
    tradeScroll.tick(dt);
    invScroll.tick(dt);

    uiDt_ = dt;
    // Пока палец на экране. После отпускания pointerX/Y остаются
    // последними известными, и обновлять по ним нечего — а
    // отпускание разбирает экран инвентаря, ему нужна именно
    // последняя позиция.
    if (ui_.hasActivePointer())
        drag.update({ ui_.pointerX(), ui_.pointerY() }, dt);
}

// ============================================================
// Render dispatch
// ============================================================
void UiSystem::render(vk::Context& ctx,
                      player::Player& player,
                      world::ChunkManager& world,
                      f32 fps)
{
    buildFrame(player, world, fps);
    renderer_.flush(ctx);
}

void UiSystem::buildFrame(player::Player& player,
                          world::ChunkManager& world,
                          f32 fps)
{
    ui_.beginFrame();

    // Обновляем кэш ресурсов
    {
        HudResources hr = readHudResources(*player.registryHandle(), player.entity());
        cachedHpPct = hr.hpPct();
        cachedMpPct = hr.mpPct();
        cachedSpPct = hr.spPct();
        cachedSpCeil = hr.spCeilPct;
        cachedAirPct = hr.airPct;
    }

    // Раскладка обходит столбец ресурсов ровно там, где он есть, —
    // и узнаёт об этом из того же списка, по которому HUD рисуется.
    layout_.setHudBehind(hudVisibleUnder(screen));
    // Под меню столбец сжимается: там нужен факт «сколько
    // осталось», а не половина экрана под полосами с подписями.
    layout_.setHudCompact(screen != Screen::Hud);
    // Пояс уступает место только паузе: там игра стоит.
    layout_.setMenuOverHotbar(screen == Screen::PauseMenu);

    switch (screen) {
        case Screen::Hud:
            drawHud(player, world, fps);
            drawTouchControls();
            if (loading()) drawLoadingOverlay();
            break;
        case Screen::PauseMenu:
            drawHud(player, world, fps);
            drawPauseMenu(player);
            break;
        case Screen::Inventory:
            drawHud(player, world, fps);
            drawInventory(player);
            break;
        case Screen::Settings:
            drawSettingsScreen(player);
            break;
        case Screen::SkillTree:
            drawHud(player, world, fps);
            drawSkillTreeScreen(player);
            break;
        case Screen::Attributes:
            drawHud(player, world, fps);
            drawAttributesScreen(player);
            break;
        case Screen::Dialogue:
            drawHud(player, world, fps);
            drawDialogueScreen(player, world);
            break;
        case Screen::QuestLog:
            drawHud(player, world, fps);
            drawQuestLogScreen(player);
            break;
        case Screen::Reputation:
            drawHud(player, world, fps);
            drawReputationScreen(player);
            break;
        case Screen::SaveLoad:
            drawHud(player, world, fps);
            drawSaveLoadScreen(player);
            break;
        case Screen::Crafting:
            drawHud(player, world, fps);
            drawCraftingScreen(player);
            break;
        case Screen::Trade:
            drawHud(player, world, fps);
            drawTradeScreen(player);
            break;
        case Screen::Enchant:
            drawHud(player, world, fps);
            drawEnchantScreen(player);
            break;
        case Screen::Worlds:
            drawWorldsScreen();
            break;
        case Screen::NewWorld:
            drawNewWorldScreen();
            break;
        case Screen::IsoSnapshot:
            drawIsoSnapshotScreen();
            break;
    }

    drawDragOverlay();
    drawStatusToast();
    // Подтверждение — поверх всего: оно модальное.
    drawConfirm();

    ui_.endFrame();
}

// ============================================================
// Иконки предметов
// ============================================================
void UiSystem::drawItemIcon(items::ItemStack& stack, float x, float y,
                            float size, bool selected)
{
    if (stack.empty()) return;

    const auto& def = items::items().get(stack.itemId);
    u32 rarity = items::rarityColor(def.rarity);

    u8 rr = (rarity >> 24) & 0xFF;
    u8 gg = (rarity >> 16) & 0xFF;
    u8 bb = (rarity >>  8) & 0xFF;

    UiColor bg = selected
        ? rgba(255, 240, 160, 240)
        : rgba(rr / 3, gg / 3, bb / 3, 240);
    ui_.rect(x, y, size, size, bg);
    ui_.rectOutline(x, y, size, size, 2.f, rgba(rr, gg, bb, 255));

    float pad = size * 0.18f;
    ui_.rect(x + pad, y + pad, size - pad * 2.f, size - pad * 2.f,
             rgba(rr, gg, bb, 255));

    if (stack.count > 1) {
        char buf[8];
        std::snprintf(buf, sizeof(buf), "%u", (unsigned)stack.count);
        float tw = ui_.textWidth(buf, 1.6f);
        ui_.text(buf, x + size - tw - 4.f, y + size - 20.f, 1.6f, COL_WHITE);
    }

    if (stack.enchant.id != combat::EnchantmentId::None) {
        ui_.rect(x + 2.f, y + 2.f, 12.f, 12.f, rgba(180, 100, 240, 255));
    }
}

void UiSystem::drawDragOverlay() {
    if (!drag.active || drag.stack.empty()) return;
    drawItemIcon(drag.stack, drag.pos.x - 30.f, drag.pos.y - 30.f, 60.f, true);
}

// ============================================================
// HUD
// ============================================================
// ============================================================
// Подгрузка мира: плашка, а не занавес.
//
// Здесь стояла заливка во весь экран поверх сцены. Мир к этому моменту
// уже нарисован и уже играбелен: чанки грузятся вокруг игрока, ближние
// готовы первыми, и дальше круг просто растёт. Гасить ради полосы
// прогресса всю картинку — значит прятать от игрока ровно то, ради
// чего он ждёт, да ещё и создавать впечатление, что игра не началась.
//
// Осталась плашка сверху по центру: подпись, полоса, проценты.
// ============================================================
void UiSystem::drawLoadingOverlay() {
    const Rect r = layout_.loadingPanel();
    const f32 pad = layout_.dp(theme::SPACE_S_DP);

    ui_.rect(r.x, r.y, r.w, r.h, hudTint(theme::Panel));
    ui_.rectOutline(r.x, r.y, r.w, r.h, layout_.dp(theme::STROKE_DP),
                    hudTint(theme::Stroke));

    const char* label = loadLabel ? loadLabel : T(StrKey::Notif_Loading);
    ui_.text(label, r.x + pad, r.y + pad, theme::TEXT_LABEL,
             theme::TextPrimary);

    const f32 p = loadProgress < 0.f ? 0.f
                : (loadProgress > 1.f ? 1.f : loadProgress);

    char pct[8];
    std::snprintf(pct, sizeof(pct), "%d%%", (int)(p * 100.f + 0.5f));
    const f32 pw = ui_.textWidth(pct, theme::TEXT_LABEL);
    ui_.text(pct, r.x + r.w - pad - pw, r.y + pad, theme::TEXT_LABEL,
             theme::TextSecondary);

    // Полоса под подписью, во всю ширину плашки за вычетом полей.
    const f32 barH = layout_.dp(theme::SPACE_S_DP);
    const f32 barY = r.y + r.h - pad - barH;
    const f32 barW = r.w - pad * 2.f;
    ui_.rect(r.x + pad, barY, barW, barH, hudTint(theme::Ink));
    if (p > 0.f)
        ui_.rect(r.x + pad, barY, barW * p, barH, hudTint(theme::Success));
}

// Служебная строка: от левого края отведённого раскладкой места.
//
// Не по центру: строки разной длины («FPS 60» и «POS -1234.5 …»)
// прыгали бы друг относительно друга, а читаются они столбиком.
void UiSystem::drawDebugLine(u32 line, const char* text, f32 scale, UiColor c) {
    const Rect r = layout_.debugLine(line);
    ui_.text(text, r.x, r.y + (r.h - ui_.textHeight(scale)) * 0.5f, scale, c);
}

void UiSystem::drawHud(player::Player& player,
                       world::ChunkManager& /*world*/,
                       f32 fps)
{
    const auto& st = player.controller.state();

    auto drawMenuButton = [&](Rect r, const char* label,
                              std::function<void()> onClick)
    {
        int idx = ui_.pushInteractiveRect(r, std::move(onClick));
        bool pressed = ui_.isInteractivePressed(idx);
        ui_.rect(r.x, r.y, r.w, r.h,
                 hudTint(pressed ? theme::Accent : theme::Panel));
        ui_.rectOutline(r.x, r.y, r.w, r.h, layout_.dp(theme::STROKE_DP),
                        hudTint(pressed ? theme::AccentPressed : theme::Stroke));
        const f32 tw = ui_.textWidth(label, theme::TEXT_BODY);
        const f32 th = ui_.textHeight(theme::TEXT_BODY);
        ui_.text(label, r.x + (r.w - tw) * 0.5f, r.y + (r.h - th) * 0.5f,
                 theme::TEXT_BODY, pressed ? theme::Ink : theme::TextPrimary);
    };

    // Кнопок было семь. При нормальном размере касания столбец из
    // семи занял бы 78 % высоты экрана: раскладка была несовместима
    // с размером пальца. Осталось две — пауза и инвентарь, самый
    // частый экран. Всё прочее живёт в паузе, куда ведёт первая.
    drawMenuButton(layout_.navButton(0), "|||",
                   [this]() { screen = Screen::PauseMenu; });
    drawMenuButton(layout_.navButton(1), cfg::tr("INV"),
                   [this]() { screen = Screen::Inventory; drag.clear(); });

    drawXpBar(player);
    drawTargetBar();
    drawHudResources(player);
    drawResonanceBar(player);
    drawStatusIcons(player);
    drawHotbar(player);

    // Счётчик кадров и координаты — по месту из раскладки, а не по
    // постоянным 180 и 204 точкам от верха: полосы ресурсов считаются
    // от плотности экрана и кончаются кто где, а эти два числа не
    // считались ни от чего и лежали прямо на полосе выносливости.
    if (showFps) {
        char buf[64];
        std::snprintf(buf, sizeof(buf), "FPS %d", (int)fps);
        drawDebugLine(0, buf, theme::TEXT_BODY, COL_YELLOW);
    }

    if (player.cameraMode == player::CameraMode::FirstPerson) {
        const float cx = screenW_ * 0.5f;
        const float cy = screenH_ * 0.5f;
        ui_.rect(cx - 10.f, cy - 1.f, 20.f, 2.f, COL_WHITE);
        ui_.rect(cx - 1.f, cy - 10.f, 2.f, 20.f, COL_WHITE);
    }

    drawHurtMarks(player);
    drawLostGoldMark(player);

    // Строка задания — только на чистом HUD: под меню её место
    // отдано самому меню, а цель задания там и так открыта в журнале.
    if (!layout_.hudCompact()) drawQuestTracker(player);
    drawInteractPrompt();

    drawLevelUpNotification(player);
    drawReputationNotification(player);

    if (cfg::settingsConst().showDebugPos) {
        char dbg[96];
        std::snprintf(dbg, sizeof(dbg), "POS %.1f %.1f %.1f",
                      st.position.x, st.position.y, st.position.z);
        drawDebugLine(1, dbg, theme::TEXT_LABEL, COL_WHITE);
    }
}

// ============================================================
// Экранное управление
// ============================================================
void UiSystem::drawTouchControls() {
    if (!touch_) return;

    const auto& s = cfg::settingsConst();
    const u8 alpha = (u8)std::clamp(s.buttonOpacity * 255.f, 0.f, 255.f);

    // --- кнопки ---
    for (const auto& b : touch_->buttons()) {
        if (!b.visible) continue;
        const glm::vec2 c = touch_->buttonCenterPx(b.id);
        const float     r = touch_->buttonRadiusPx(b.id);
        if (r <= 0.f) continue;

        const UiColor fill = b.pressed ? rgba(230, 230, 230, alpha)
                                       : rgba( 20,  20,  26, (u8)(alpha * 0.7f));
        ui_.circle(c.x, c.y, r, fill);
        ui_.ring(c.x, c.y, r - 3.f, r,
                 b.pressed ? rgba(255, 235, 140, alpha)
                           : rgba(210, 210, 220, alpha));

        if (b.label) {
            const float tw = ui_.textWidth(b.label, 2.f);
            const float th = ui_.textHeight(2.f);
            ui_.text(b.label, c.x - tw * 0.5f, c.y - th * 0.5f, 2.f,
                     b.pressed ? COL_BLACK : rgba(235, 235, 245, alpha));
        }
    }

    // --- джойстик ---
    // Он появляется под пальцем, поэтому рисуется только пока активен.
    const auto& j = touch_->joystick();
    if (j.active) {
        const u8 ja = (u8)std::clamp(j.opacity * 255.f, 0.f, 255.f);
        ui_.ring(j.center.x, j.center.y, j.radius - 4.f, j.radius,
                 rgba(220, 220, 230, ja));
        ui_.circle(j.center.x, j.center.y, j.radius * 0.12f,
                   rgba(220, 220, 230, (u8)(ja * 0.5f)));

        // Ручку держим внутри круга: палец уходит дальше, чем радиус.
        glm::vec2 d = j.current - j.center;
        const float len = glm::length(d);
        if (len > j.radius) d *= j.radius / len;
        ui_.circle(j.center.x + d.x, j.center.y + d.y,
                   j.radius * 0.34f, rgba(245, 245, 250, ja));
    }
}

// ============================================================
// Подсказка взаимодействия
// ============================================================
//
// Игра каждый кадр считает, стоит ли игрок у станка или алтаря
// (main.cpp: detectNearbyStation, EnchantAltar). По этому признаку
// даже переключалась музыка. А показать было нечем: подсказки не
// существовало, openEnchant не вызывался ниоткуда, а ремесло и
// торговля висели на диалоге, который не проходится. Три готовых
// экрана были недостижимы из-за отсутствия одной кнопки.
void UiSystem::drawInteractPrompt() {
    const bool station = nearbyStation != crafting::StationType::None;
    const bool altar   = nearbyAltar != 0;
    if (!station && !altar) return;

    // Станок ближе по смыслу: если рядом и то и другое, предлагаем его.
    const char* what = station ? crafting::stationName(nearbyStation)
                               : T(StrKey::Ench_Altar);

    const Rect r = layout_.interactPrompt();
    const int idx = ui_.pushInteractiveRect(r, [this, station]() {
        if (station) openCrafting(nearbyStation);
        else if (nearbyAltar) openEnchant(nearbyAltar);
    });
    const bool pressed = ui_.isInteractivePressed(idx);

    ui_.rect(r.x, r.y, r.w, r.h,
             pressed ? theme::Accent : theme::Panel);
    ui_.rectOutline(r.x, r.y, r.w, r.h,
                    layout_.dp(theme::STROKE_SELECTED_DP),
                    pressed ? theme::AccentPressed : theme::Accent);

    const UiColor fg = pressed ? theme::Ink : theme::TextPrimary;
    const f32 lw = ui_.textWidth(T(StrKey::Hud_Use), theme::TEXT_LABEL);
    ui_.text(T(StrKey::Hud_Use), r.x + (r.w - lw) * 0.5f,
             r.y + layout_.dp(theme::SPACE_S_DP),
             theme::TEXT_LABEL, pressed ? theme::Ink : theme::Accent);

    const f32 nw = ui_.textWidth(what, theme::TEXT_BODY);
    ui_.text(what, r.x + (r.w - nw) * 0.5f,
             r.y + r.h - layout_.dp(theme::SPACE_S_DP)
                 - ui_.textHeight(theme::TEXT_BODY),
             theme::TEXT_BODY, fg);
}

void UiSystem::drawXpBar(player::Player& player) {
    auto* prog = player.progression();
    if (!prog) return;

    const Rect r = layout_.xpBar();
    ui_.rect(r.x, r.y - layout_.dp(2.f), r.w, r.h + layout_.dp(4.f),
             hudTint(theme::Ink));
    ui_.rect(r.x, r.y, r.w, r.h, hudTint(theme::XpBed));
    ui_.rect(r.x, r.y, r.w * prog->levelProgress(), r.h, hudTint(theme::Xp));

    char buf[64];
    std::snprintf(buf, sizeof(buf), cfg::tr("LV %u"), prog->level);
    ui_.text(buf, r.x + layout_.dp(theme::SPACE_S_DP),
             r.y + r.h + layout_.dp(theme::SPACE_XS_DP),
             theme::TEXT_LABEL, theme::TextPrimary);

    char xpText[64];
    std::snprintf(xpText, sizeof(xpText), "%llu / %llu",
                  (unsigned long long)prog->xpWithinLevel(),
                  (unsigned long long)prog->xpForNextLevel());
    const f32 tw = ui_.textWidth(xpText, theme::TEXT_CAPTION);
    ui_.text(xpText, r.x + r.w - tw - layout_.dp(theme::SPACE_S_DP),
             r.y + r.h + layout_.dp(theme::SPACE_XS_DP),
             theme::TEXT_CAPTION, theme::TextSecondary);
}

void UiSystem::drawTargetBar() {
    if (!target.dead && target.fill <= 0.f) return;
    if (target.alpha <= 0.f) return;

    const Rect r = layout_.targetBar();
    const f32 a = target.alpha < 0.f ? 0.f : (target.alpha > 1.f ? 1.f : target.alpha);

    // Затухание полосы — поверх настройки прозрачности HUD, а не
    // вместо неё: уходящая полоса обязана уходить и у того, кто
    // выкрутил прозрачность.
    auto tint = [&](UiColor c) {
        const UiColor h = hudTint(c);
        return withAlpha(h, (u8)((f32)(h & 0xFFu) * a + 0.5f));
    };

    ui_.rect(r.x - layout_.dp(2.f), r.y - layout_.dp(2.f),
             r.w + layout_.dp(4.f), r.h + layout_.dp(4.f), tint(theme::Ink));
    ui_.rect(r.x, r.y, r.w, r.h, tint(theme::HpBed));

    // След последнего урона — ПОД основной заливкой и шире её. Он и
    // отвечает на вопрос «сколько снял этот удар»: разница между
    // двумя краями и есть размер удара.
    if (target.ghost > target.fill)
        ui_.rect(r.x, r.y, r.w * target.ghost, r.h, tint(theme::Accent));

    ui_.rect(r.x, r.y, r.w * target.fill, r.h,
             tint(target.fill < 0.3f ? theme::Danger : theme::Hp));

    // Деления фаз — на настоящих порогах, тех же, по которым босс
    // меняет поведение. Считать их «на глаз равными долями» нельзя:
    // разойдясь с bossPhaseFor, они начнут врать ровно в тот момент,
    // когда игрок на них и смотрит.
    for (u8 i = 1; i < target.phases; ++i) {
        const f32 at = (f32)(target.phases - i) / (f32)target.phases;
        ui_.rect(r.x + r.w * at - layout_.dp(1.f), r.y,
                 layout_.dp(2.f), r.h, tint(theme::Ink));
    }

    ui_.rectOutline(r.x, r.y, r.w, r.h, layout_.dp(theme::STROKE_DP),
                    tint(theme::Stroke));

    // Имя — внутри полосы, но ПРИЖАТО К ЛЕВОМУ КРАЮ, а не по центру.
    //
    // По центру оно и стояло, пока на HUD не посмотрели. Середина
    // полосы — самое содержательное её место: там граница заливки и
    // следа, то есть ровно размер последнего удара, и там же стоят
    // деления фаз босса. Подложка под именем накрывала и то и
    // другое.
    //
    // Слева она не накрывает ничего: левый край полосы полон, пока
    // цель жива, и имя ложится на ровную заливку.
    if (target.name && target.name[0]) {
        const f32 th = ui_.textHeight(theme::TEXT_LABEL);
        const f32 pad = layout_.dp(theme::SPACE_XS_DP);
        const f32 tx = r.x + pad * 2.f;
        const f32 ty = r.y + (r.h - th) * 0.5f;

        // Тень, а не подложка. Подложка — это непрозрачный
        // прямоугольник, и при низком здоровье цели она снова
        // накрыла бы границу заливки и следа: имя-то не короче, а
        // заливка ужалась. Тень в один пиксель читается так же, но
        // не закрывает собой ничего.
        const f32 off = layout_.dp(1.f);
        ui_.text(target.name, tx + off, ty + off, theme::TEXT_LABEL,
                 tint(withAlpha(theme::Ink, 220)));
        ui_.text(target.name, tx, ty, theme::TEXT_LABEL,
                 tint(theme::TextPrimary));
    }
}

void UiSystem::drawHudResources(player::Player& player) {
    struct Bar { f32 pct; UiColor fill; UiColor bed; StrKey label; };
    const Bar bars[3] = {
        { cachedHpPct, cachedHpPct < 0.3f ? theme::Danger : theme::Hp,
          theme::HpBed, StrKey::Hud_Health },
        { cachedMpPct, theme::Mp, theme::MpBed, StrKey::Hud_Mana },
        { cachedSpPct, theme::Sp, theme::SpBed, StrKey::Hud_Stamina },
    };

    const bool compact = layout_.hudCompact();

    for (u32 i = 0; i < 3; ++i) {
        const Rect r = layout_.resourceBar(i);
        const f32 o = layout_.dp(2.f);
        ui_.rect(r.x - o, r.y - o, r.w + o * 2.f, r.h + o * 2.f,
                 hudTint(theme::Ink));
        ui_.rect(r.x, r.y, r.w, r.h, hudTint(bars[i].bed));
        ui_.rect(r.x, r.y, r.w * bars[i].pct, r.h, hudTint(bars[i].fill));
        // Утомление: отрезанный хвост полоски выносливости. Без него
        // полоска просто переставала бы наполняться доверху, и игрок
        // читал бы это как поломку, а не как «ты вымотан».
        if (i == 2 && cachedSpCeil < 0.999f) {
            const f32 cx = r.x + r.w * cachedSpCeil;
            ui_.rect(cx, r.y, r.x + r.w - cx, r.h, hudTint(theme::Ink));
        }
        // Подпись — только в полном столбце: в полоску высотой в
        // восемь точек она не поместится, а цвет и длина говорят то
        // же самое. Читать «HP» игрок, открывший инвентарь, не
        // нанимался.
        if (!compact)
            ui_.text(T(bars[i].label), r.x + layout_.dp(theme::SPACE_XS_DP),
                     r.y + (r.h - ui_.textHeight(theme::TEXT_CAPTION)) * 0.5f,
                     theme::TEXT_CAPTION, theme::TextPrimary);
    }

    // ---- Воздух ----
    //
    // Показывается только под водой: полной полоске на экране делать
    // нечего, а пустеющая — единственное, что скажет игроку, зачем
    // всплывать.
    if (cachedAirPct < 0.999f && !compact) {
        const Rect r = layout_.airBar();
        const f32 o = layout_.dp(2.f);
        ui_.rect(r.x - o, r.y - o, r.w + o * 2.f, r.h + o * 2.f,
                 hudTint(theme::Ink));
        ui_.rect(r.x, r.y, r.w, r.h, hudTint(theme::MpBed));
        ui_.rect(r.x, r.y, r.w * cachedAirPct, r.h,
                 hudTint(cachedAirPct < 0.25f ? theme::Danger : theme::Mp));
        ui_.text(T(StrKey::Hud_Air), r.x + layout_.dp(theme::SPACE_XS_DP),
                 r.y + (r.h - ui_.textHeight(theme::TEXT_CAPTION)) * 0.5f,
                 theme::TEXT_CAPTION, theme::TextPrimary);
    }

    // Золото под меню не печатается: инвентарь и торговля показывают
    // его сами, и две одинаковые цифры в разных углах — лишний
    // вопрос «почему их две».
    if (auto* wal = compact ? nullptr : player.wallet()) {
        char goldBuf[32];
        wal->format(goldBuf, sizeof(goldBuf));
        const Rect g = layout_.goldLine();
        ui_.text(goldBuf, g.x,
                 g.y + (g.h - ui_.textHeight(theme::TEXT_LABEL)) * 0.5f,
                 theme::TEXT_LABEL, theme::Accent);
    }
}

void UiSystem::drawResonanceBar(player::Player& player) {
    const auto& res = player.resonance;
    const Rect r = layout_.resonanceBar();
    const float bw = r.w;
    const float bh = r.h;
    const float bx = r.x;
    const float by = r.y;
    const float pad = layout_.dp(2.f);

    ui_.rect(bx - pad, by - pad, bw + pad * 2.f, bh + pad * 2.f,
             hudTint(COL_BLACK));
    ui_.rect(bx, by, bw, bh, hudTint(rgba(40,20,60,255)));

    UiColor fillColor = rgba(140, 90, 220, 255);
    if (res.stack >= combat::RESONANCE_MAX_STACKS)
        fillColor = rgba(255, 200, 60, 255);
    else if (res.stack >= 3)
        fillColor = rgba(200, 100, 240, 255);

    ui_.rect(bx, by, bw * res.fill(), bh, hudTint(fillColor));

    // Деления — на АБСОЛЮТНЫХ отметках ступеней, а не на равных долях
    // полосы. У игрока с «Resonance Master» потолок выше, ступени
    // остаются там же, и хвост за пятым делением — это запас: он
    // утекает первым и держит ступень дольше.
    const f32 perStack = combat::RESONANCE_MAX /
                         (f32)combat::RESONANCE_MAX_STACKS;
    const f32 span = std::max(1.f, res.maxValue);
    for (i32 i = 1; i <= combat::RESONANCE_MAX_STACKS; ++i) {
        const f32 at = perStack * (f32)i / span;
        if (at >= 0.999f) continue;
        ui_.rect(bx + bw * at - layout_.dp(1.f), by, layout_.dp(2.f), bh,
                 hudTint(COL_BLACK));
    }
    // Подпись — как и у остальных полос, только в полном столбце: в
    // полоску высотой в восемь точек она не помещается и лезет
    // наружу, а делений Резонанса и так достаточно, чтобы прочитать
    // его одним взглядом.
    if (!layout_.hudCompact())
        ui_.text(T(StrKey::Hud_Resonance), bx + layout_.dp(theme::SPACE_XS_DP),
                 by + layout_.dp(1.f), theme::TEXT_CAPTION, hudTint(COL_WHITE));

    if (res.finisherReady) {
        const char* txt = T(StrKey::Hud_FinisherReady);
        const float tw = ui_.textWidth(txt, theme::TEXT_LABEL);
        ui_.text(txt, bx + bw - tw, by - layout_.dp(theme::SPACE_L_DP),
                 theme::TEXT_LABEL, hudTint(rgba(255, 220, 60, 255)));
    }
}

void UiSystem::drawStatusIcons(player::Player& player) {
    const auto& se = player.statuses;
    u32 slot = 0;

    auto icon = [&](UiColor c, const char* label) {
        if (slot >= HudLayout::STATUS_ICONS) return;
        const Rect r = layout_.statusIcon(slot++);
        ui_.rect(r.x, r.y, r.w, r.h, hudTint(c));
        ui_.rectOutline(r.x, r.y, r.w, r.h, layout_.dp(theme::STROKE_DP),
                        hudTint(COL_BLACK));
        const float tw = ui_.textWidth(label, theme::TEXT_CAPTION);
        const float th = ui_.textHeight(theme::TEXT_CAPTION);
        ui_.text(label, r.x + (r.w - tw) * 0.5f, r.y + (r.h - th) * 0.5f,
                 theme::TEXT_CAPTION, hudTint(COL_WHITE));
    };

    if (se.burnTime > 0.f)   icon(rgba(220, 100, 40, 220), cfg::tr("FIRE"));
    if (se.slowTime > 0.f)   icon(rgba(80, 160, 240, 220), cfg::tr("FROST"));
    if (se.stunTime > 0.f)   icon(rgba(240, 220, 80, 220), cfg::tr("STUN"));
    if (se.poisonTime > 0.f) icon(rgba(120, 220, 80, 220), cfg::tr("POIS"));
}

void UiSystem::drawLevelUpNotification(player::Player& player) {
    if (!player.pendingLevelUpNotification) return;
    const float t = player.levelUpFlashTimer / 2.0f;
    u8 alpha = (u8)(255.f * std::min(1.f, t * 2.f));
    const float cw = 400.f, ch = 90.f;
    const float cx = ((float)screenW_ - cw) * 0.5f;
    const float cy = (float)screenH_ * 0.30f;

    ui_.rect(cx, cy, cw, ch, rgba(60, 20, 90, (u8)(alpha * 0.85f)));
    ui_.rectOutline(cx, cy, cw, ch, 3.f, rgba(255, 220, 80, alpha));
    const char* txt = T(StrKey::Notif_LevelUp);
    float tw = ui_.textWidth(txt, 3.f);
    ui_.text(txt, cx + (cw - tw) * 0.5f, cy + 12.f, 3.f,
             rgba(255, 230, 120, alpha));
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%s %u", T(StrKey::Hud_Level),
                  player.lastLevelGained);
    float bw = ui_.textWidth(buf, 2.f);
    ui_.text(buf, cx + (cw - bw) * 0.5f, cy + 56.f, 2.f,
             rgba(255, 255, 255, alpha));
}

void UiSystem::drawReputationNotification(player::Player& player) {
    if (!player.reputationFlashActive) return;
    const float t = player.reputationFlashTimer / 2.0f;
    u8 alpha = (u8)(255.f * std::min(1.f, t * 2.f));
    const float cw = 480.f, ch = 60.f;
    const float cx = ((float)screenW_ - cw) * 0.5f;
    const float cy = (float)screenH_ * 0.45f;

    ui_.rect(cx, cy, cw, ch, rgba(40, 30, 20, (u8)(alpha * 0.85f)));
    ui_.rectOutline(cx, cy, cw, ch, 2.f, rgba(200, 180, 60, alpha));
    char buf[128];
    std::snprintf(buf, sizeof(buf), "%s: %s",
                  factions::factionName(player.lastRepFaction),
                  factions::tierName(player.lastRepTier));
    float tw = ui_.textWidth(buf, 2.f);
    ui_.text(buf, cx + (cw - tw) * 0.5f, cy + 20.f, 2.f,
             rgba(255, 240, 180, alpha));
}

void UiSystem::drawHotbar(player::Player& player) {
    // Ячеек В ДАННЫХ девять — это формат сохранения. На экране их
    // столько, сколько помещается при размере касания не ниже 48 dp:
    // на телефоне пять, на планшете все девять. Остальные достаются
    // из инвентаря.
    const u32 shown = layout_.hotbarVisibleSlots();

    auto* inv = player.inventory();
    const u8 active = inv ? inv->activeHotbar : 0;

    for (u32 i = 0; i < shown; ++i) {
        const Rect r = layout_.hotbarSlot(i);
        const bool isActive = (i == (u32)active);

        ui_.rect(r.x, r.y, r.w, r.h,
                 hudTint(isActive ? theme::PanelRaised : theme::Panel));
        // Выбранная ячейка отличается не только цветом: рамка толще.
        ui_.rectOutline(r.x, r.y, r.w, r.h,
                        layout_.dp(isActive ? theme::STROKE_SELECTED_DP
                                            : theme::STROKE_DP),
                        hudTint(isActive ? theme::Accent : theme::Stroke));

        if (inv) {
            auto& st = inv->at(items::INV_HOTBAR_OFFSET + i);
            if (!st.empty()) {
                const f32 pad = layout_.dp(theme::SPACE_XS_DP);
                drawItemIcon(st, r.x + pad, r.y + pad, r.w - pad * 2.f, false);
            }
        }

        char num[4];
        std::snprintf(num, sizeof(num), "%u", (unsigned)(i + 1));
        ui_.text(num, r.x + layout_.dp(theme::SPACE_XS_DP),
                 r.y + layout_.dp(theme::SPACE_XS_DP),
                 theme::TEXT_CAPTION, theme::TextSecondary);
    }

    const Rect hb = layout_.hotbar();
    const auto& wdef = combat::weapons().get(player.equipped.weaponId);
    if (wdef.name) {
        const char* wname = cfg::tr(wdef.name);
        const f32 tw = ui_.textWidth(wname, theme::TEXT_LABEL);
        ui_.text(wname, hb.x + hb.w * 0.5f - tw * 0.5f,
                 hb.y - layout_.dp(theme::SPACE_L_DP),
                 theme::TEXT_LABEL, theme::TextPrimary);
    }
}

// ============================================================
// Pause Menu
// ============================================================
// Разделы меню паузы.
//
// Крафт здесь появился не для симметрии: экран ремесла открывался
// ровно одной кнопкой — подсказкой «использовать», которая выходит
// рядом со станком. В поле станка нет, кнопки нет, и весь раздел
// StationType::None был недостижим — рецепты «на ходу» существовали
// только в перечислении.
const std::vector<MenuEntry>& menuEntries() {
    static const std::vector<MenuEntry> items = {
        { config::StrKey::Menu_Inventory,  Screen::Inventory  },
        { config::StrKey::Menu_Crafting,   Screen::Crafting   },
        { config::StrKey::Menu_Attributes, Screen::Attributes },
        { config::StrKey::Menu_Skills,     Screen::SkillTree  },
        { config::StrKey::Menu_Quests,     Screen::QuestLog   },
        { config::StrKey::Menu_Reputation, Screen::Reputation },
        { config::StrKey::Menu_SaveLoad,   Screen::SaveLoad   },
        { config::StrKey::Menu_Worlds,     Screen::Worlds     },
        { config::StrKey::Menu_Settings,   Screen::Settings   },
    };
    return items;
}

// ============================================================
// Фон и заголовок полноэкранного экрана
// ============================================================
//
// HUD под таким экраном рисуется намеренно: `paused()` гасит только
// ввод и музыку, мир продолжает жить, и полоса здоровья игроку
// нужна ровно тогда, когда он копается в сумке. Затемнение гасит
// игру за спиной, но не HUD — а заголовок отмеряется от раскладки,
// которая про столбец HUD знает и обходит его.
void UiSystem::drawMenuBackdrop(const char* title) {
    ui_.rect(0, 0, (f32)screenW_, (f32)screenH_,
             withAlpha(theme::Ink, theme::ALPHA_SCRIM));
    if (!title || !title[0]) return;
    const Rect t = layout_.menuTitle();
    ui_.text(title, t.x,
             t.y + (t.h - ui_.textHeight(theme::TEXT_TITLE)) * 0.5f,
             theme::TEXT_TITLE, theme::TextPrimary);
}

void UiSystem::drawPauseMenu(player::Player& player) {
    // Первое, что должен сообщать экран, — что игра остановлена.
    drawMenuBackdrop(T(StrKey::Menu_Pause));

    // Пунктов было восемь в столбик, каждый своего цвета: зелёный,
    // синий, фиолетовый, оранжевый, жёлтый, голубой, оливковый,
    // красный. Ни группировки, ни иерархии — и инвентаря в списке не
    // было вовсе, хотя выход ИЗ инвентаря вёл сюда.
    //
    // Теперь: продолжить отдельно и крупно, остальное — сеткой по
    // смыслу, выход отдельно и в опасном виде.
    auto* tree = player.skillTree();
    auto* prog = player.progression();

    // Значок с нераспределёнными очками — то, ради чего в раздел
    // заходят. Считается здесь, а не в списке разделов: список от
    // игрока не зависит, а значок только от него и зависит.
    auto badgeFor = [&](Screen sc) -> i32 {
        if (sc == Screen::Attributes) return prog ? prog->availableAttrPoints : -1;
        if (sc == Screen::SkillTree)  return tree ? tree->unspentPoints : -1;
        return -1;
    };

    const auto& items = menuEntries();
    const u32 ITEM_COUNT = (u32)items.size();
    // Первый ряд занят «Продолжить», последний — выходом.
    //
    // Столбцов не три постоянных, а столько, чтобы всё поместилось:
    // на 1280x720 пять рядов не влезали, и нижние кнопки вместе с
    // «выходом» уезжали за край экрана.
    const u32 COLS = layout_.menuColsFor(ITEM_COUNT, 2);

    // Одна сетка на всё: «продолжить» первым рядом во всю ширину,
    // разделы следом, «выход» последним рядом. Прокручивается
    // целиком.
    //
    // Раньше рядов было столько, сколько выходило, и когда они не
    // помещались по высоте, ряд поднимали до цели касания — а сетка
    // уезжала за нижний край экрана. На 1280x720 за краем
    // оказывались нижние разделы вместе с «выходом»: из паузы
    // нельзя было выйти.
    //
    // Закрепить «продолжить» и «выход» по краям не вышло: на
    // 1280x720 они вдвоём съедают 256 точек из 328, и разделам не
    // остаётся ни одного ряда. Поэтому едут все вместе.
    const Rect area = layout_.menuArea();
    const auto grid = layout_.cardGrid(area, COLS, theme::TOUCH_REGULAR_DP);
    const u32 sectionRows = (ITEM_COUNT + COLS - 1) / COLS;
    // «Продолжить» и «выход» делят первый ряд, разделы идут следом.
    //
    // «Выход» стоял последним рядом — и на 1280x720 оказывался за
    // прокруткой: из трёх видимых рядов два занимали разделы. В
    // паузе выход ищут первым делом, и заставлять его листать — это
    // ровно тот же дефект, что и уехавший за край.
    const u32 totalRows = 1 + sectionRows;

    pauseScroll.setContentHeight((f32)totalRows * grid.rowStride(),
                                 (f32)grid.rowsVisible() * grid.rowStride());
    feedScrollDrag(pauseScroll, area);

    // Ряд виден, только если помещается целиком: половина кнопки
    // поверх пояса хуже, чем её отсутствие.
    auto rowRect = [&](u32 row, u32 col, bool full) -> Rect {
        Rect r = grid.at(col, row);
        if (full) r.w = area.w;
        r.y -= pauseScroll.offset;
        return r;
    };
    auto visible = [&](const Rect& r) {
        return r.y >= area.y - 0.5f && r.y + r.h <= area.y + area.h + 0.5f;
    };

    // ---- Продолжить: почти во всю ширину, главное действие ----
    const Rect topRow = rowRect(0, 0, true);
    const f32 exitW = (topRow.w - layout_.dp(theme::SPACE_M_DP)
                       * (f32)(COLS - 1)) / (f32)COLS;
    if (Rect r = topRow; visible(r)) {
        r.w -= exitW + layout_.dp(theme::SPACE_M_DP);
        const int idx = ui_.pushInteractiveRect(r, [this]() {
            screen = Screen::Hud;
            returnTo = Screen::Hud;
        });
        const bool pressed = ui_.isInteractivePressed(idx);
        ui_.rect(r.x, r.y, r.w, r.h, pressed ? theme::Accent : theme::PanelRaised);
        ui_.rectOutline(r.x, r.y, r.w, r.h,
                        layout_.dp(theme::STROKE_SELECTED_DP),
                        pressed ? theme::AccentPressed : theme::Accent);
        const char* lbl = T(StrKey::Menu_Resume);
        const f32 tw = ui_.textWidth(lbl, theme::TEXT_BODY);
        ui_.text(lbl, r.x + (r.w - tw) * 0.5f,
                 r.y + (r.h - ui_.textHeight(theme::TEXT_BODY)) * 0.5f,
                 theme::TEXT_BODY, pressed ? theme::Ink : theme::TextPrimary);
    }

    // ---- Разделы: одинаковые кнопки, один вид на всю игру ----
    for (u32 i = 0; i < ITEM_COUNT; ++i) {
        const Rect r = rowRect(1 + i / COLS, i % COLS, false);
        if (!visible(r)) continue;

        const Screen target = items[i].target;
        const int idx = ui_.pushInteractiveRect(r, [this, target]() {
            // Крафт открывается с тем станком, который рядом СЕЙЧАС:
            // nearbyStation обновляется каждый кадр, и None в нём
            // значит «станка рядом нет», то есть крафт на ходу.
            if (target == Screen::Crafting) { openCrafting(nearbyStation); return; }
            openScreen(target);
            if (target == Screen::Inventory) drag.clear();
        });
        const bool pressed = ui_.isInteractivePressed(idx);

        ui_.rect(r.x, r.y, r.w, r.h, pressed ? theme::Accent : theme::PanelRaised);
        ui_.rectOutline(r.x, r.y, r.w, r.h, layout_.dp(theme::STROKE_DP),
                        pressed ? theme::AccentPressed : theme::Stroke);

        const char* label = T(items[i].label);
        const f32 tw = ui_.textWidth(label, theme::TEXT_BODY);
        ui_.text(label, r.x + (r.w - tw) * 0.5f,
                 r.y + (r.h - ui_.textHeight(theme::TEXT_BODY)) * 0.5f,
                 theme::TEXT_BODY, pressed ? theme::Ink : theme::TextPrimary);

        const i32 badge = badgeFor(target);
        if (badge > 0) {
            char b[16];
            std::snprintf(b, sizeof(b), "+%d", badge);
            const f32 bw = ui_.textWidth(b, theme::TEXT_CAPTION);
            ui_.text(b, r.x + r.w - bw - layout_.dp(theme::SPACE_S_DP),
                     r.y + layout_.dp(theme::SPACE_S_DP),
                     theme::TEXT_CAPTION, pressed ? theme::Ink : theme::Accent);
        }
    }

    // ---- Выход: необратимо, поэтому в опасном виде и с вопросом ----
    if (const Rect r{ topRow.x + topRow.w - exitW, topRow.y,
                      exitW, topRow.h }; visible(r)) {
        const int idx = ui_.pushInteractiveRect(r, [this]() {
            askConfirm(T(StrKey::Menu_Quit), T(StrKey::Menu_Quit),
                       [this]() { if (onQuit) onQuit(); });
        });
        const bool pressed = ui_.isInteractivePressed(idx);
        // Опасное показано рамкой, а не заливкой: цвет тут не
        // единственный признак.
        ui_.rect(r.x, r.y, r.w, r.h,
                 pressed ? theme::Danger : theme::Panel);
        ui_.rectOutline(r.x, r.y, r.w, r.h,
                        layout_.dp(theme::STROKE_SELECTED_DP), theme::Danger);
        const char* lbl = T(StrKey::Menu_Quit);
        const f32 tw = ui_.textWidth(lbl, theme::TEXT_BODY);
        ui_.text(lbl, r.x + (r.w - tw) * 0.5f,
                 r.y + (r.h - ui_.textHeight(theme::TEXT_BODY)) * 0.5f,
                 theme::TEXT_BODY, pressed ? theme::TextPrimary : theme::Danger);
    }
}

// ============================================================
// Inventory
// ============================================================
void UiSystem::drawInventory(player::Player& player) {
    drawMenuBackdrop(T(StrKey::Inv_Title));

    auto* inv = player.inventory();
    if (!inv) return;

    const Rect title = layout_.menuTitle();

    if (auto* wal = player.wallet()) {
        char gold[32];
        wal->format(gold, sizeof(gold));
        const f32 tw = ui_.textWidth(gold, theme::TEXT_BODY);
        ui_.text(gold, title.x + title.w - tw,
                 title.y + (title.h - ui_.textHeight(theme::TEXT_BODY)) * 0.5f,
                 theme::TEXT_BODY, theme::Accent);
    }

    // Содержимое едет целиком: сумка, экипировка и пояс — один
    // столбец, и прокручивается он как один.
    //
    // 27 + 6 + 9 ячеек требуют около 320 точек, а на 640x360 под
    // содержимое остаётся 232 даже со сжатым столбцом HUD. Это не
    // лечится раскладкой — пробовал: отнятая у сумки высота делала
    // недоступными все двадцать семь ячеек вместо тринадцати.
    const Rect view = layout_.invLeft();
    const Rect left{ view.x, view.y - invScroll.offset, view.w, view.h };

    // ---- Сумка: ВСЕ 27 ячеек ----
    //
    // Рисовалось 6x4 = 24 из 27. Три ячейки не были видны вовсе, а
    // sortMain() вправе положить предмет в любую — в том числе в
    // невидимую, откуда его не достать. Брони и аксессуаров не было
    // в интерфейсе тоже: игрок видел 33 ячейки из 42.
    const HudLayout::CellGrid main = layout_.cellGrid(left, items::INV_MAIN_SLOTS);
    drawSlotGrid(player, main, items::INV_MAIN_OFFSET,
                 items::INV_MAIN_SLOTS, view);

    // ---- Экипировка: броня и аксессуары ----
    const Rect mb = main.bounds();
    const Rect eqArea{ left.x, mb.y + mb.h + layout_.dp(theme::SPACE_L_DP),
                       left.w, layout_.dp(theme::TOUCH_REGULAR_DP) };
    // Подпись едет вместе со своей сеткой и пропадает вместе с ней:
    // надпись «EQUIPPED» над пустым местом сообщала бы о ячейках,
    // которых на экране нет.
    const auto stripLabel = [&](StrKey key, const Rect& area) {
        const f32 ty = area.y - layout_.dp(theme::SPACE_M_DP)
                     - ui_.textHeight(theme::TEXT_LABEL);
        if (ty < view.y || ty > view.y + view.h) return;
        ui_.text(T(key), area.x, ty, theme::TEXT_LABEL, theme::TextSecondary);
    };
    stripLabel(StrKey::Inv_Equipped, eqArea);

    const u32 eqCount = items::INV_ARMOR_SLOTS + items::INV_ACC_SLOTS;
    const HudLayout::CellGrid eq = layout_.cellGrid(eqArea, eqCount);
    drawSlotGrid(player, eq, items::INV_ARMOR_OFFSET, eqCount, view);

    // ---- Пояс: здесь видны все девять ----
    //
    // Он есть и на HUD, но убрать его отсюда нельзя: перенос
    // предмета пальцем работает по сеткам ЭТОГО экрана, и без сетки
    // пояса положить в пояс стало бы нечем. Проверка на это есть, и
    // она поймала попытку.
    const Rect eb = eq.bounds();
    const Rect hbArea{ left.x, eb.y + eb.h + layout_.dp(theme::SPACE_L_DP),
                       left.w, layout_.dp(theme::TOUCH_REGULAR_DP) };
    stripLabel(StrKey::Inv_Hotbar, hbArea);

    const HudLayout::CellGrid hb =
        layout_.cellGrid(hbArea, items::INV_HOTBAR_SLOTS);
    drawSlotGrid(player, hb, items::INV_HOTBAR_OFFSET,
                 items::INV_HOTBAR_SLOTS, view);

    // Сколько всего вышло содержимого — столько и прокручивается.
    // Считается по факту, а не по формуле: сетки сами решают, во
    // сколько рядов лечь, и повторять это решение здесь значило бы
    // держать те же правила в двух местах.
    {
        const Rect hbb = hb.bounds();
        const f32 content = (hbb.y + hbb.h) - left.y;
        invScroll.setContentHeight(content, view.h);
    }

    drawItemDetails(player);

    // Сетки прошли — значит цель переноса уже найдена, если палец
    // отпустили над ячейкой. Разбираем здесь, а не внутри сетки:
    // сеток три, а перенос один.
    updateSlotDrag(*inv);

    // Листать и нести — один и тот же жест, и развести их надо
    // однозначно. Палец, опущенный на ЗАНЯТУЮ ячейку, несёт предмет:
    // перенос возможен только оттуда. Всё остальное — пустая ячейка,
    // зазор, поле, подпись — листает.
    const bool carrying = drag.active ||
        (dragPressSlot_ >= 0 && !inv->at((u32)dragPressSlot_).empty());
    feedScrollDrag(invScroll, view, carrying);
}

// ============================================================
// Сетка ячеек инвентаря
// ============================================================
//
// Один вид ячейки на всю игру: сумка, экипировка и пояс рисуются
// этим же кодом. Тап ВЫБИРАЕТ — и только: раньше он одновременно
// использовал предмет и начинал его перенос.
void UiSystem::drawSlotGrid(player::Player& player,
                            const HudLayout::CellGrid& g,
                            u32 firstSlot, u32 count, Rect clip)
{
    auto* inv = player.inventory();
    if (!inv) return;

    // Ячейка рисуется, только если помещается в область ЦЕЛИКОМ.
    //
    // Сперва отсекались лишь ячейки, ушедшие за край полностью, — и
    // проверка показала, что этого мало: частично уехавший ряд
    // рисуется весь и вылезает на заголовок экрана (девяносто восемь
    // лишних вершин над областью). Нажать такую ячейку можно было бы
    // там, где её не видно.
    //
    // Графической обрезки у этого интерфейса нет, поэтому выбор
    // простой: ряд либо виден весь, либо не виден вовсе. При
    // прокрутке он пропадает шагом — зато ничего никуда не наползает.
    const bool clipped = clip.h > 0.f;
    auto hidden = [&](const Rect& r) {
        return clipped && (r.y < clip.y - 0.01f ||
                           r.y + r.h > clip.y + clip.h + 0.01f);
    };

    for (u32 i = 0; i < count; ++i) {
        const Rect r = g.at(i);
        if (hidden(r)) continue;
        const u32 slot = firstSlot + i;

        const int idx = ui_.pushInteractiveRect(r, [this, slot]() {
            // Отпускание, завершающее перенос, — не тап: выбирать им
            // ячейку нельзя. Обработчик срабатывает раньше, чем
            // updateSlotDrag разберёт отпускание, поэтому drag.active
            // здесь ещё поднят.
            if (drag.active) return;
            selectedInvSlot = (selectedInvSlot == (i32)slot) ? -1 : (i32)slot;
        });
        const bool pressed  = ui_.isInteractivePressed(idx);
        const bool selected = (selectedInvSlot == (i32)slot);

        // Палец лёг на непустую ячейку — запоминаем её как возможное
        // начало переноса. Само решение принимается позже: по сдвигу
        // или по времени.
        if (!drag.active && dragPressSlot_ < 0 && ui_.hasActivePointer() &&
            r.contains(ui_.pointerX(), ui_.pointerY()) &&
            !inv->at(slot).empty())
        {
            dragPressSlot_ = (i32)slot;
            dragPressPos_  = { ui_.pointerX(), ui_.pointerY() };
            dragPressTime_ = 0.f;
        }

        // Цель переноса. Активного тача в кадре отпускания уже нет,
        // но pointerX/Y хранят координаты именно события UP — то есть
        // точное место, где палец оторвали.
        if (drag.active && dragPointerWas_ && !ui_.hasActivePointer() &&
            r.contains(ui_.pointerX(), ui_.pointerY()))
        {
            dragDropSlot_ = (i32)slot;
        }

        ui_.rect(r.x, r.y, r.w, r.h,
                 pressed ? theme::PanelRaised : theme::Panel);

        // Выбранное отличается не только цветом: рамка толще.
        ui_.rectOutline(r.x, r.y, r.w, r.h,
                        layout_.dp(selected ? theme::STROKE_SELECTED_DP
                                            : theme::STROKE_DP),
                        selected ? theme::Accent : theme::Stroke);

        // Во время переноса ячейка-источник показывает ОСТАТОК.
        // Предмет из неё не вынут — иначе выход из экрана посреди
        // переноса терял бы его, — поэтому остаток считается здесь.
        items::ItemStack st = inv->at(slot);
        if (drag.active && (i32)slot == (i32)drag.fromSlot) {
            if (st.count > drag.stack.count) st.count -= drag.stack.count;
            else                             st.clear();
        }
        if (!st.empty()) {
            const f32 pad = layout_.dp(theme::SPACE_XS_DP);
            drawItemIcon(st, r.x + pad, r.y + pad, r.w - pad * 2.f, selected);
        }

        // Активная ячейка пояса помечена уголком — признак помимо цвета.
        if (slot >= items::INV_HOTBAR_OFFSET &&
            slot <  items::INV_HOTBAR_OFFSET + items::INV_HOTBAR_SLOTS &&
            (slot - items::INV_HOTBAR_OFFSET) == (u32)inv->activeHotbar) {
            const f32 m = layout_.dp(theme::SPACE_S_DP);
            ui_.rect(r.x, r.y, m, m, theme::Accent);
        }
    }
}

// ============================================================
// Перенос предмета пальцем
// ============================================================
//
// Модуль ui::DragDrop был написан целиком — захват, порог, деление
// стека долгим тапом, отмена — и его begin() не звал никто: drag.active
// не поднимался ни разу за всю жизнь программы. Вместе с ним без
// применения стояла половина API инвентаря.
//
// Перекладка делается одним движением здесь, при отпускании. Пока
// палец в пути, в drag лежит только «что несём и откуда»: предмет
// остаётся в сумке, и выход из экрана посреди переноса ничего не
// теряет.
namespace {

/// Переложить count штук из src в dst.
///
/// Обычный случай — уместить в цель, сколько влезет, и ровно столько
/// снять с источника. Если не влезло ничего, а несут стек ЦЕЛИКОМ и
/// цель занята несовместимым — меняем ячейки местами: это то, чего
/// игрок и ждёт, перетаскивая предмет на занятое место.
void moveBetweenSlots(items::Inventory& inv, u32 src, u32 dst, u16 count) {
    if (src == dst || count == 0) return;
    if (src >= items::INV_TOTAL_SLOTS || dst >= items::INV_TOTAL_SLOTS) return;

    items::ItemStack from = inv.at(src);
    if (from.empty()) return;
    if (count > from.count) count = from.count;

    items::ItemStack moving = from;
    moving.count = count;

    const items::AddResult res = inv.putStack(dst, moving);
    if (res.added > 0) {
        inv.removeFromSlot(src, res.added);
        return;
    }
    if (count == from.count && !inv.at(dst).canStackWith(moving))
        inv.swapSlots(src, dst);
}

} // namespace

// ============================================================
// Прокрутка пальцем
// ============================================================
//
// `Scroll` умеет перетаскивание с самого своего появления, и не звал
// его никто: списки листались одними кнопками «вверх» и «вниз».
// Написанный и неподключённый жест — ровно та болезнь, которую в
// этом проекте уже лечили у квестовых уведомлений и у эликсиров.
void UiSystem::feedScrollDrag(Scroll& sc, const Rect& area, bool blocked) {
    const bool ptr = ui_.hasActivePointer();

    if (!ptr) { sc.endDrag(sc.touchId); return; }

    const i32 id = ui_.activePointerId();
    const f32 y  = ui_.pointerY();

    if (sc.dragging) { sc.updateDrag(id, y); return; }
    if (blocked) return;
    if (sc.maxOffset <= 0.f) return;          // листать нечего
    if (!area.contains(ui_.pointerX(), y)) return;
    sc.beginDrag(id, y);
}

void UiSystem::updateSlotDrag(items::Inventory& inv) {
    const bool ptr = ui_.hasActivePointer();

    // ---- Отпускание ----
    if (dragPointerWas_ && !ptr) {
        if (drag.active) {
            if (dragDropSlot_ >= 0)
                moveBetweenSlots(inv, drag.fromSlot, (u32)dragDropSlot_,
                                 drag.stack.count);
            // Мимо ячеек — предмет просто остаётся на месте: он из
            // сумки и не уходил.
            drag.end();
        }
        dragPressSlot_ = -1;
        dragPressTime_ = 0.f;
        dragDropSlot_  = -1;
        dragPointerWas_ = false;
        return;
    }
    dragPointerWas_ = ptr;

    if (!ptr) { dragPressSlot_ = -1; dragPressTime_ = 0.f; return; }
    if (drag.active || dragPressSlot_ < 0) return;

    const items::ItemStack& src = inv.at((u32)dragPressSlot_);
    if (src.empty()) { dragPressSlot_ = -1; return; }

    dragPressTime_ += uiDt_;

    const glm::vec2 now{ ui_.pointerX(), ui_.pointerY() };
    const glm::vec2 d = now - dragPressPos_;
    const bool moved = glm::dot(d, d) >=
                       DragDrop::DRAG_THRESHOLD * DragDrop::DRAG_THRESHOLD;
    const bool longTap = dragPressTime_ >= DragDrop::LONG_TAP_TIME;
    if (!moved && !longTap) return;

    // Сдвинулся — несём стек целиком. Простоял на месте — половину:
    // делить стек больше нечем, отдельной кнопки для этого нет.
    items::ItemStack carried = src;
    const bool split = (!moved && longTap && src.count >= 2);
    if (split) carried.count = (u16)(src.count / 2);

    drag.begin(ui_.activePointerId(), (u32)dragPressSlot_, now, carried);
    drag.isSplit = split;
    // Выделение снимаем: сведения справа относятся к ячейке, из
    // которой предмет вот-вот уедет.
    selectedInvSlot = -1;
}

// ============================================================
// Settings
// ============================================================
// ============================================================
// Сведения о выбранном предмете
// ============================================================
//
// Их не было нигде: игрок не мог узнать, что это, сколько у него и
// можно ли этим воспользоваться. При этом тап по ячейке сразу
// использовал предмет — то есть узнать можно было только выпив.
//
// Не стена текста: название, редкость, количество и ровно те
// действия, которые к предмету применимы.
void UiSystem::drawItemDetails(player::Player& player) {
    auto* inv = player.inventory();

    // Ничего не выбрано — панели НЕТ вовсе.
    //
    // Она стояла всегда, занимая треть экрана под одной надписью
    // «нажми, чтобы использовать». Подсказку эту игрок прочтёт один
    // раз в жизни, а ячейки, которых из-за неё не помещалось,
    // нужны ему каждый раз.
    if (!inv || selectedInvSlot < 0 ||
        selectedInvSlot >= (i32)items::INV_TOTAL_SLOTS ||
        inv->at((u32)selectedInvSlot).empty()) {
        return;
    }

    const Rect d = layout_.invDetails();
    ui_.rect(d.x, d.y, d.w, d.h, theme::Panel);
    ui_.rectOutline(d.x, d.y, d.w, d.h, layout_.dp(theme::STROKE_DP),
                    theme::Stroke);

    const f32 pad = layout_.dp(theme::PANEL_PAD_DP);

    const u32 slot = (u32)selectedInvSlot;
    auto& st = inv->at(slot);
    const auto& def = items::items().get(st.itemId);

    f32 y = d.y + pad;

    // Название цветом редкости — и рядом словом, потому что одним
    // цветом ценность передавать нельзя.
    if (def.name) {
        ui_.text(items::items().name(st.itemId), d.x + pad, y, theme::TEXT_BODY,
                 items::rarityColor(def.rarity));
        y += layout_.dp(theme::SPACE_L_DP) + ui_.textHeight(theme::TEXT_BODY);
    }
    if (const char* rn = items::rarityName(def.rarity)) {
        ui_.text(rn, d.x + pad, y, theme::TEXT_CAPTION, theme::TextSecondary);
        y += layout_.dp(theme::SPACE_M_DP) + ui_.textHeight(theme::TEXT_CAPTION);
    }

    char cnt[48];
    std::snprintf(cnt, sizeof(cnt), "x%u", (unsigned)st.count);
    ui_.text(cnt, d.x + pad, y, theme::TEXT_LABEL, theme::TextPrimary);
    y += layout_.dp(theme::SPACE_M_DP) + ui_.textHeight(theme::TEXT_LABEL);

    if (st.enchant.id != combat::EnchantmentId::None) {
        if (const char* en = combat::enchantmentName(st.enchant.id))
            ui_.text(en, d.x + pad, y, theme::TEXT_CAPTION, theme::Xp);
    }

    // ---- Действия ----
    //
    // Каждое — отдельной кнопкой. Одно касание делает ровно одно.
    const bool inHotbar = slot >= items::INV_HOTBAR_OFFSET &&
                          slot <  items::INV_HOTBAR_OFFSET + items::INV_HOTBAR_SLOTS;

    struct Action { const char* label; bool danger; std::function<void()> run; };
    std::vector<Action> acts;

    acts.push_back({ T(StrKey::Inv_Use), false, [this, slot]() {
        if (onUseItem) onUseItem(slot);
    }});

    if (inHotbar) {
        acts.push_back({ T(StrKey::Inv_Equipped), false, [this, slot]() {
            if (onEquipHotbar) onEquipHotbar(slot);
        }});
    } else {
        acts.push_back({ T(StrKey::Inv_Hotbar), false, [this, inv, slot]() {
            // В первую свободную ячейку пояса, иначе в активную.
            u32 dst = items::INV_HOTBAR_OFFSET + inv->activeHotbar;
            for (u32 i = 0; i < items::INV_HOTBAR_SLOTS; ++i) {
                if (!inv->at(items::INV_HOTBAR_OFFSET + i).empty()) continue;
                dst = items::INV_HOTBAR_OFFSET + i;
                break;
            }
            if (onMoveItem) onMoveItem(slot, (i32)dst);
            selectedInvSlot = -1;
        }});
    }

    // Выбросить необратимо — поэтому с вопросом и в опасном виде.
    acts.push_back({ T(StrKey::Inv_Drop), true, [this, slot]() {
        askConfirm(T(StrKey::Inv_Drop), T(StrKey::Inv_Drop), [this, slot]() {
            if (onDropItem) onDropItem(slot);
            selectedInvSlot = -1;
        });
    }});

    for (u32 i = 0; i < (u32)acts.size(); ++i) {
        const Rect r = layout_.invAction(i, (u32)acts.size());
        const int idx = ui_.pushInteractiveRect(r, acts[i].run);
        const bool pressed = ui_.isInteractivePressed(idx);
        const UiColor accent = acts[i].danger ? theme::Danger : theme::Stroke;

        ui_.rect(r.x, r.y, r.w, r.h,
                 pressed ? accent : theme::PanelRaised);
        ui_.rectOutline(r.x, r.y, r.w, r.h,
                        layout_.dp(acts[i].danger ? theme::STROKE_SELECTED_DP
                                                  : theme::STROKE_DP),
                        accent);
        const f32 tw = ui_.textWidth(acts[i].label, theme::TEXT_BODY);
        ui_.text(acts[i].label, r.x + (r.w - tw) * 0.5f,
                 r.y + (r.h - ui_.textHeight(theme::TEXT_BODY)) * 0.5f,
                 theme::TEXT_BODY,
                 pressed ? theme::TextPrimary
                         : (acts[i].danger ? theme::Danger : theme::TextPrimary));
    }
}

Rect settingsTabRect(u32 index) {
    const f32 tabW = 180.f;
    return { 40.f + (f32)index * (tabW + 6.f), 84.f, tabW, 48.f };
}

u32 settingsRowCount(SettingsTab tab, bool buttonLayout) {
    switch (tab) {
        // Чувствительность, две инверсии, левша, радиус, мёртвая зона,
        // прозрачность стика, размер и прозрачность кнопок, режим
        // перемещения. Включённый режим добавляет подсказку и сброс.
        case SettingsTab::Input:  return buttonLayout ? 12u : 10u;
        case SettingsTab::Ui:     return 4u;
        case SettingsTab::Audio:  return 3u;
        // Язык, автосейв, его интервал, журнал.
        case SettingsTab::Game:   return 5u;
        case SettingsTab::Render: return 2u;
        default:                  return 0u;
    }
}

SettingsLayout UiSystem::settingsLayout() const {
    SettingsLayout L;
    L.panel  = layout_.menuArea();
    L.rowH   = layout_.dp(theme::TOUCH_REGULAR_DP);
    L.rowGap = layout_.dp(theme::SPACE_S_DP);
    L.pad    = layout_.dp(theme::PANEL_PAD_DP);
    L.colGap = layout_.dp(theme::SPACE_L_DP);

    // Вкладка «Управление» содержит двенадцать строк: в один столбец
    // это 938 точек, а дно панели на экране 1280x720 — 680. Последние
    // четыре настройки были недостижимы, прокрутки у настроек нет.
    // Колонок столько, сколько нужно, чтобы всё поместилось.
    const f32 availH = L.panel.h - L.pad * 2.f;
    L.perCol = (u32)((availH + L.rowGap) / (L.rowH + L.rowGap));

    // Ширина колонки зависит от их числа, а оно — от вкладки; берём
    // худший случай, чтобы колонки не прыгали между вкладками.
    const u32 maxRows  = settingsRowCount(SettingsTab::Input, true);
    const u32 colCount = L.perCol ? ((maxRows + L.perCol - 1) / L.perCol) : 1;
    L.colW = (L.panel.w - L.pad * 2.f - L.colGap * (f32)(colCount - 1))
           / (f32)(colCount ? colCount : 1);

    L.resetAll = { L.panel.x + L.pad, (f32)screenH_ - 90.f, 260.f, 50.f };
    return L;
}

void UiSystem::drawSettingsScreen(player::Player& /*player*/) {
    drawMenuBackdrop(T(StrKey::Settings_Title));

    drawCloseButton([this]() { screen = Screen::PauseMenu; });

    // Табы
    const char* tabNames[(u32)SettingsTab::Count] = {
        T(StrKey::Settings_Input),
        T(StrKey::Settings_Ui),
        T(StrKey::Settings_Audio),
        T(StrKey::Settings_Game),
        T(StrKey::Settings_Render),
    };

    for (u32 i = 0; i < (u32)SettingsTab::Count; ++i) {
        const Rect r = settingsTabRect(i);
        bool active = ((u32)settingsTab == i);
        int idx = ui_.pushInteractiveRect(r, [this, i]() {
            settingsTab = (SettingsTab)i;
        });
        bool pressed = ui_.isInteractivePressed(idx);
        UiColor bg = active ? rgba(80, 120, 80, 255)
                            : (pressed ? rgba(120,120,120,255) : rgba(40,40,40,220));
        ui_.rect(r.x, r.y, r.w, r.h, bg);
        ui_.rectOutline(r.x, r.y, r.w, r.h, 2.f, COL_BLACK);
        float tw = ui_.textWidth(tabNames[i], 1.6f);
        ui_.text(tabNames[i], r.x + (r.w - tw) * 0.5f, r.y + 14.f, 1.6f, COL_WHITE);
    }

    auto& s = cfg::settings();

    const SettingsLayout L = settingsLayout();
    const Rect panel = L.panel;

    ui_.rect(panel.x, panel.y, panel.w, panel.h, theme::Panel);
    ui_.rectOutline(panel.x, panel.y, panel.w, panel.h,
                    layout_.dp(theme::STROKE_DP), theme::Stroke);

    u32 rowIdx = 0;
    auto nextRow = [&L, &rowIdx]() -> Rect { return L.row(rowIdx++); };

    switch (settingsTab) {
        case SettingsTab::Input: {
            // Sensitivity
            {
                Rect r = nextRow();
                sliderWidget(ui_, r, &cfg::settings().cameraSensitivity,
                             0.1f, 5.0f, T(StrKey::Settings_CameraSens),
                             0.05f, [this](float){ notifySettingsChanged(); });
            }
            // Invert X
            {
                Rect r = nextRow();
                const int idx = ui_.pushInteractiveRect(r, [this]() {
                    cfg::settings().invertX = !cfg::settings().invertX; if (onSettingsChanged) onSettingsChanged();
                });
                toggleWidget(ui_, r, idx, &s.invertX, T(StrKey::Settings_InvertX));
            }
            // Invert Y
            {
                Rect r = nextRow();
                const int idx = ui_.pushInteractiveRect(r, [this]() {
                    cfg::settings().invertY = !cfg::settings().invertY; if (onSettingsChanged) onSettingsChanged();
                });
                toggleWidget(ui_, r, idx, &s.invertY, T(StrKey::Settings_InvertY));
            }
            // Joystick left
            {
                Rect r = nextRow();
                int idx = ui_.pushInteractiveRect(r, [this]() {
                    cfg::settings().joystickLeftHanded = !cfg::settings().joystickLeftHanded; if (onSettingsChanged) onSettingsChanged();
                });
                toggleWidget(ui_, r, idx, &s.joystickLeftHanded,
                             T(StrKey::Settings_JoystickLeft));
            }
            // Joystick radius
            {
                Rect r = nextRow();
                sliderWidget(ui_, r, &s.joystickRadius,
                             80.f, 220.f, T(StrKey::Settings_JoystickRadius),
                             5.f, [this](float){ notifySettingsChanged(); });
            }
            // Deadzone
            {
                Rect r = nextRow();
                sliderWidget(ui_, r, &s.joystickDeadzone,
                             0.05f, 0.40f, T(StrKey::Settings_JoystickDeadzone),
                             0.01f, [this](float){ notifySettingsChanged(); });
            }
            // Прозрачность джойстика
            {
                Rect r = nextRow();
                sliderWidget(ui_, r, &s.joystickOpacity,
                             0.2f, 1.0f, T(StrKey::Settings_JoystickOpacity),
                             0.05f, [this](float){ notifySettingsChanged(); });
            }
            // Размер кнопок
            {
                Rect r = nextRow();
                sliderWidget(ui_, r, &s.buttonScale,
                             0.6f, 1.6f, T(StrKey::Settings_ButtonScale),
                             0.05f, [this](float){ notifySettingsChanged(); });
            }
            // Прозрачность кнопок
            {
                Rect r = nextRow();
                sliderWidget(ui_, r, &s.buttonOpacity,
                             0.2f, 1.0f, T(StrKey::Settings_ButtonOpacity),
                             0.05f, [this](float){ notifySettingsChanged(); });
            }
            // Режим перемещения кнопок (ТЗ 5.2)
            {
                Rect r = nextRow();
                const int idx = ui_.pushInteractiveRect(r, [this]() {
                    buttonLayoutMode = !buttonLayoutMode;
                });
                toggleWidget(ui_, r, idx, &buttonLayoutMode,
                             T(StrKey::Settings_ButtonLayout));
            }
            if (buttonLayoutMode) {
                const Rect hint = nextRow();
                ui_.text(T(StrKey::Settings_LayoutHint),
                         hint.x, hint.y + (hint.h -
                             ui_.textHeight(theme::TEXT_CAPTION)) * 0.5f,
                         theme::TEXT_CAPTION, theme::TextSecondary);

                Rect r = nextRow();
                int idx = ui_.pushInteractiveRect(r, [this]() {
                    for (u32 i = 0; i < cfg::Settings::BUTTON_SLOTS; ++i) {
                        cfg::settings().buttonOffsetX[i] = 0.f;
                        cfg::settings().buttonOffsetY[i] = 0.f;
                    }
                    if (onSettingsChanged) onSettingsChanged();
                });
                ui_.button(T(StrKey::Settings_ResetLayout), r, idx,
                           rgba(90, 90, 110, 255), COL_WHITE);
            }
            break;
        }

        case SettingsTab::Ui: {
            {
                Rect r = nextRow();
                sliderWidget(ui_, r, &s.uiScale, 0.75f, 1.5f,
                             T(StrKey::Settings_UiScale), 0.05f,
                             [this](float){ notifySettingsChanged(); });
            }
            {
                Rect r = nextRow();
                sliderWidget(ui_, r, &s.uiOpacity, 0.4f, 1.0f,
                             T(StrKey::Settings_UiOpacity), 0.05f,
                             [this](float){ notifySettingsChanged(); });
            }
            {
                Rect r = nextRow();
                int idx = ui_.pushInteractiveRect(r, [this]() {
                    cfg::settings().showFps = !cfg::settings().showFps; if (onSettingsChanged) onSettingsChanged();
                });
                toggleWidget(ui_, r, idx, &s.showFps, T(StrKey::Settings_ShowFps));
            }
            {
                Rect r = nextRow();
                int idx = ui_.pushInteractiveRect(r, [this]() {
                    cfg::settings().showDebugPos = !cfg::settings().showDebugPos; if (onSettingsChanged) onSettingsChanged();
                });
                toggleWidget(ui_, r, idx, &s.showDebugPos, T(StrKey::Settings_ShowDebug));
            }
            break;
        }

        case SettingsTab::Audio: {
            {
                Rect r = nextRow();
                sliderWidget(ui_, r, &s.masterVolume, 0.f, 1.f,
                             T(StrKey::Settings_Master), 0.05f,
                             [this](float){ notifySettingsChanged(); });
            }
            {
                Rect r = nextRow();
                sliderWidget(ui_, r, &s.musicVolume, 0.f, 1.f,
                             T(StrKey::Settings_Music), 0.05f,
                             [this](float){ notifySettingsChanged(); });
            }
            {
                Rect r = nextRow();
                sliderWidget(ui_, r, &s.sfxVolume, 0.f, 1.f,
                             T(StrKey::Settings_Sfx), 0.05f,
                             [this](float){ notifySettingsChanged(); });
            }
            break;
        }

        case SettingsTab::Game: {
            // Language cycle
            {
                Rect r = nextRow();
                const char* opts[(u32)cfg::Language::Count] = {
                    cfg::languageName(cfg::Language::English),
                    cfg::languageName(cfg::Language::Russian),
                };
                u32 cur = (u32)s.language;
                int idx = ui_.pushInteractiveRect(r, [this]() {
                    u32 n = (u32)cfg::settings().language + 1;
                    if (n >= (u32)cfg::Language::Count) n = 0;
                    cfg::settings().language = (cfg::Language)n;
                    cfg::L().setLanguage(cfg::settings().language);
                    if (onSettingsChanged) onSettingsChanged();
                });
                cycleWidget(ui_, r, idx, T(StrKey::Settings_Language), opts,
                            (u32)cfg::Language::Count, &cur);
            }
            {
                Rect r = nextRow();
                int idx = ui_.pushInteractiveRect(r, [this]() {
                    cfg::settings().autosaveEnabled = !cfg::settings().autosaveEnabled; if (onSettingsChanged) onSettingsChanged();
                });
                toggleWidget(ui_, r, idx, &s.autosaveEnabled,
                             T(StrKey::Settings_Autosave));
            }
            {
                Rect r = nextRow();
                sliderWidget(ui_, r, &s.autosaveInterval, 60.f, 900.f,
                             T(StrKey::Settings_AutosaveInterval), 30.f,
                             [this](float){ notifySettingsChanged(); });
            }
            // Журнал: выключается целиком, вместе с logcat. Отчёт о
            // падении пишется всё равно — см. core/crashlog.h.
            {
                Rect r = nextRow();
                int idx = ui_.pushInteractiveRect(r, [this]() {
                    cfg::settings().logEnabled = !cfg::settings().logEnabled;
                    if (onSettingsChanged) onSettingsChanged();
                });
                toggleWidget(ui_, r, idx, &s.logEnabled,
                             T(StrKey::Settings_Logging));
            }
            // Изометрический снимок мира — не настройка, а действие,
            // поэтому кнопка, а не переключатель.
            {
                Rect r = nextRow();
                const int idx = ui_.pushInteractiveRect(r, [this]() {
                    openScreen(Screen::IsoSnapshot);
                    if (onIsoParamsChanged) onIsoParamsChanged();
                });
                ui_.button(T(StrKey::Iso_Open), r, idx,
                           theme::PanelRaised, theme::TextPrimary);
            }
            break;
        }

        case SettingsTab::Render: {
            {
                Rect r = nextRow();
                f32 vd = (f32)s.viewDistance;
                sliderWidget(ui_, r, &vd, 4.f, 12.f,
                             T(StrKey::Settings_ViewDistance), 1.f,
                             [this](float v) {
                                 cfg::settings().viewDistance = (i32)(v + 0.5f);
                                 notifySettingsChanged();
                             });
            }
            {
                Rect r = nextRow();
                int idx = ui_.pushInteractiveRect(r, [this]() {
                    cfg::settings().unlimitedFps = !cfg::settings().unlimitedFps; if (onSettingsChanged) onSettingsChanged();
                });
                toggleWidget(ui_, r, idx, &s.unlimitedFps,
                             T(StrKey::Settings_UnlimitedFps));
            }
            break;
        }

        default: break;
    }

    // Кнопка "Reset to defaults"
    {
        const Rect r = L.resetAll;
        int idx = ui_.pushInteractiveRect(r, [this]() {
            cfg::Settings def{};
            cfg::settings() = def;
            cfg::L().setLanguage(cfg::settings().language);
            if (onSettingsChanged) onSettingsChanged();
        });
        ui_.button(T(StrKey::Settings_ResetAll), r, idx,
                   rgba(140, 60, 60, 255), COL_WHITE);
    }
}

// ============================================================
// Skill Tree
// ============================================================
void UiSystem::drawSkillTreeScreen(player::Player& player) {
    drawMenuBackdrop(cfg::tr("TREE OF KNOWLEDGE"));

    auto* tree = player.skillTree();
    if (!tree) return;

    drawCloseButton([this]() { screen = Screen::Hud; });

    // Сколько очков осталось — в строке заголовка, слева от кнопки
    // закрытия.
    //
    // Стояло это по `screenW_ - 340.f` от верха, то есть на кнопке
    // закрытия при узком экране и в пустоте при широком.
    {
        const Rect r = layout_.titleAction(POINTS_LABEL_W_DP);
        char ptsBuf[64];
        std::snprintf(ptsBuf, sizeof(ptsBuf), cfg::tr("Skill Points: %d"),
                      tree->unspentPoints);
        const f32 tw = ui_.textWidth(ptsBuf, theme::TEXT_LABEL);
        ui_.text(ptsBuf, r.x + r.w - tw,
                 r.y + (r.h - ui_.textHeight(theme::TEXT_LABEL)) * 0.5f,
                 theme::TEXT_LABEL,
                 tree->unspentPoints > 0 ? theme::Accent : theme::TextPrimary);
    }

    // Три столбца ветвей.
    //
    // Раньше столбцы считались от ширины экрана и начинались с
    // постоянных 150 точек сверху: на 1280x720 шапки ветвей
    // ложились на заголовок экрана, а третий столбец накрывал
    // кнопку закрытия — экран нельзя было закрыть.
    const Rect area = layout_.menuArea();
    const f32 gap   = layout_.dp(theme::SPACE_M_DP);
    const f32 colW  = (area.w - gap * 2.f) / 3.f;
    const f32 headH = layout_.dp(theme::SPACE_XXL_DP);

    const SkillBranch branches[3] = {
        SkillBranch::Strength, SkillBranch::Agility, SkillBranch::Wisdom,
    };
    const char* branchNames[3] = { cfg::tr("STRENGTH"), cfg::tr("AGILITY"),
                                   cfg::tr("WISDOM") };
    UiColor branchColors[3] = {
        rgba(220, 80, 60, 255),
        rgba(80, 200, 80, 255),
        rgba(80, 140, 240, 255),
    };

    // Сколько узлов в самой длинной ветви — столько рядов и считаем.
    u16 maxNodes = 1;
    for (int b = 0; b < 3; ++b) {
        u16 n = 0;
        skillTree().nodesInBranch(branches[b], n);
        if (n > maxNodes) maxNodes = n;
    }

    // Восемь узлов в столбце не помещаются ни на один телефон в
    // портретной высоте 720: ряд ниже цели касания делать нельзя, а
    // восемь целей касания — это 384 точки при 200 доступных.
    // Поэтому столбцы прокручиваются, все три разом: это
    // параллельные списки одной длины.
    const Rect rows{ area.x, area.y + headH + gap,
                     area.w, area.h - headH - gap };
    const auto grid = layout_.cardGrid(rows, 3, theme::TOUCH_MIN_DP);

    skillScroll.setContentHeight((f32)maxNodes * grid.rowStride(),
                                 (f32)grid.rowsVisible() * grid.rowStride());
    feedScrollDrag(skillScroll, rows);

    for (int b = 0; b < 3; ++b) {
        const f32 cx = area.x + (f32)b * (colW + gap);

        ui_.rect(cx, area.y, colW, headH, branchColors[b]);
        const f32 tw = ui_.textWidth(branchNames[b], theme::TEXT_LABEL);
        ui_.text(branchNames[b], cx + (colW - tw) * 0.5f,
                 area.y + (headH - ui_.textHeight(theme::TEXT_LABEL)) * 0.5f,
                 theme::TEXT_LABEL, COL_WHITE);

        u16 nodeCount = 0;
        const auto* nodes = skillTree().nodesInBranch(branches[b], nodeCount);

        for (u16 i = 0; i < nodeCount; ++i) {
            const auto& def = nodes[i];
            Rect nr = grid.at((u32)b, (u32)i);
            nr.y -= skillScroll.offset;
            // Ряд, не поместившийся целиком, не рисуется и не ловит
            // касаний: половина ряда поверх пояса хуже, чем её
            // отсутствие. То же правило, что у сумки и журнала.
            if (nr.y < rows.y - 0.5f) continue;
            if (nr.y + nr.h > rows.y + rows.h + 0.5f) break;

            bool maxed   = tree->rank(def.id) >= def.maxRank;
            bool canOpen = tree->canUnlock(def.id);
            u8   rank    = tree->rank(def.id);

            UiColor bg;
            if (maxed)        bg = rgba(220, 180, 60, 220);
            else if (canOpen) bg = rgba(60, 100, 60, 220);
            else              bg = rgba(40, 40, 40, 200);

            int ni = ui_.pushInteractiveRect(nr, [tree, id = def.id]() {
                tree->unlock(id);
            });

            bool pressed = ui_.isInteractivePressed(ni);
            if (pressed) bg = rgba(140, 140, 200, 255);

            ui_.rect(nr.x, nr.y, nr.w, nr.h, bg);
            ui_.rectOutline(nr.x, nr.y, nr.w, nr.h,
                            layout_.dp(theme::STROKE_DP), COL_BLACK);

            char label[96];
            std::snprintf(label, sizeof(label), "%s  [%u/%u]",
                          cfg::tr(def.name), (unsigned)rank,
                          (unsigned)def.maxRank);
            ui_.text(label, nr.x + layout_.dp(theme::SPACE_S_DP),
                     nr.y + (nr.h - ui_.textHeight(theme::TEXT_LABEL)) * 0.5f,
                     theme::TEXT_LABEL,
                     canOpen || maxed ? COL_WHITE : rgba(160, 160, 160, 255));
        }
    }
}

// ============================================================
// Attributes
// ============================================================
void UiSystem::drawAttributesScreen(player::Player& player) {
    drawMenuBackdrop(T(StrKey::Menu_Attributes));

    auto* prog = player.progression();
    auto* attr = player.attributes();
    if (!prog || !attr) return;
    auto* reg = player.registryHandle();
    const auto* buffs = reg
        ? reg->get<progression::AttributeBuffs>(player.entity()) : nullptr;

    const Rect title = layout_.menuTitle();

    // Свободные очки — то, ради чего сюда заходят, поэтому крупно и
    // акцентом, пока они есть.
    char pts[64];
    std::snprintf(pts, sizeof(pts), "%s %d", T(StrKey::Hud_Level),
                  prog->availableAttrPoints);
    const f32 pw = ui_.textWidth(pts, theme::TEXT_BODY);
    ui_.text(pts, title.x + title.w - pw,
             title.y + (title.h - ui_.textHeight(theme::TEXT_BODY)) * 0.5f,
             theme::TEXT_BODY,
             prog->availableAttrPoints > 0 ? theme::Accent : theme::TextSecondary);

    struct AttrRow { const char* label; const char* desc; i32* value; UiColor color; };
    AttrRow rows[4] = {
        { cfg::tr("STRENGTH"),     cfg::tr("+MELEE DAMAGE, +CRIT DAMAGE"),
          &attr->strength,     theme::Hp },
        { cfg::tr("AGILITY"),      cfg::tr("+ATTACK SPEED, +CRIT, +MOVE"),
          &attr->agility,      theme::Sp },
        { cfg::tr("INTELLIGENCE"), cfg::tr("+MANA, +SPELL POWER"),
          &attr->intelligence, theme::Mp },
        { cfg::tr("ENDURANCE"),    cfg::tr("+HEALTH, +RESIST, +STAMINA"),
          &attr->endurance,    theme::Accent },
    };

    for (u32 i = 0; i < 4; ++i) {
        const Rect r = layout_.attrRow(i);
        ui_.rect(r.x, r.y, r.w, r.h, theme::PanelRaised);
        ui_.rectOutline(r.x, r.y, r.w, r.h, layout_.dp(theme::STROKE_DP),
                        theme::Stroke);
        // Цветная полоса слева — опознавательный знак характеристики.
        ui_.rect(r.x, r.y, layout_.dp(6.f), r.h, rows[i].color);

        const f32 tx = r.x + layout_.dp(theme::SPACE_L_DP);
        ui_.text(rows[i].label, tx, r.y + layout_.dp(theme::SPACE_M_DP),
                 theme::TEXT_BODY, theme::TextPrimary);
        ui_.text(rows[i].desc, tx,
                 r.y + r.h - layout_.dp(theme::SPACE_M_DP)
                     - ui_.textHeight(theme::TEXT_CAPTION),
                 theme::TEXT_CAPTION, theme::TextSecondary);

        // Действующий эликсир показан прибавкой рядом с числом и
        // цветом самого числа. Иначе выпитый эликсир никак себя не
        // обнаруживает: очки те же, а считается игра по другим.
        const i32 bonus = buffs ? buffs->add[i] : 0;

        char val[16];
        std::snprintf(val, sizeof(val), "%d", *rows[i].value);
        const Rect minus = layout_.attrButton(i, false);
        const f32 vw = ui_.textWidth(val, theme::TEXT_TITLE);
        const f32 vx = minus.x - layout_.dp(theme::SPACE_L_DP) - vw;
        ui_.text(val, vx, r.y + (r.h - ui_.textHeight(theme::TEXT_TITLE)) * 0.5f,
                 theme::TEXT_TITLE, bonus > 0 ? theme::Accent : theme::TextPrimary);
        if (bonus > 0) {
            char add[16];
            std::snprintf(add, sizeof(add), "+%d", bonus);
            const f32 aw = ui_.textWidth(add, theme::TEXT_CAPTION);
            ui_.text(add, vx - layout_.dp(theme::SPACE_S_DP) - aw,
                     r.y + (r.h - ui_.textHeight(theme::TEXT_CAPTION)) * 0.5f,
                     theme::TEXT_CAPTION, theme::Accent);
        }

        // ---- убавить ----
        const bool canSub = (*rows[i].value > 1);
        {
            const int idx = ui_.pushInteractiveRect(minus, [this, prog, i, rows]() {
                if (*rows[i].value <= 1) return;
                *rows[i].value -= 1;
                prog->availableAttrPoints += 1;
                prog->derivedDirty = true;
                notify(rows[i].label, theme::NotifyPriority::Low);
            });
            drawStepper(minus, "-", ui_.isInteractivePressed(idx) && canSub, canSub);
        }

        // ---- прибавить ----
        const bool canAdd = prog->availableAttrPoints > 0 && *rows[i].value < 99;
        {
            const Rect pr = layout_.attrButton(i, true);
            const int idx = ui_.pushInteractiveRect(pr, [this, prog, i, rows, canAdd]() {
                if (!canAdd) return;
                *rows[i].value += 1;
                prog->availableAttrPoints -= 1;
                prog->derivedDirty = true;
                // Обратная связь: иначе непонятно, засчиталось ли.
                notify(rows[i].label, theme::NotifyPriority::Low);
            });
            drawStepper(pr, "+", ui_.isInteractivePressed(idx) && canAdd, canAdd);
        }
    }
}

// Кнопка «+»/«−» одного вида на всю игру.
//
// Недоступность показана не только цветом: у выключенной рамка
// тоньше и текст приглушён.
void UiSystem::drawStepper(const Rect& r, const char* label,
                           bool pressed, bool enabled)
{
    const UiColor fill = !enabled ? withAlpha(theme::Panel, theme::ALPHA_DISABLED)
                       : (pressed ? theme::Accent : theme::PanelRaised);
    const UiColor edge = !enabled ? withAlpha(theme::Stroke, theme::ALPHA_DISABLED)
                       : (pressed ? theme::AccentPressed : theme::Stroke);
    ui_.rect(r.x, r.y, r.w, r.h, fill);
    ui_.rectOutline(r.x, r.y, r.w, r.h,
                    layout_.dp(enabled ? theme::STROKE_SELECTED_DP
                                       : theme::STROKE_DP), edge);

    const f32 tw = ui_.textWidth(label, theme::TEXT_TITLE);
    ui_.text(label, r.x + (r.w - tw) * 0.5f,
             r.y + (r.h - ui_.textHeight(theme::TEXT_TITLE)) * 0.5f,
             theme::TEXT_TITLE,
             !enabled ? theme::TextDisabled
                      : (pressed ? theme::Ink : theme::TextPrimary));
}

// ============================================================
// Dialogue
// ============================================================
void UiSystem::drawDialogueScreen(player::Player& player,
                                  world::ChunkManager& world) {
    auto* dlg = player.activeDialogue();
    if (!dlg || !dlg->active) { screen = Screen::Hud; return; }

    npc::DialogueNode* node = dlg->findNode(dlg->currentNodeId);
    if (!node) { screen = Screen::Hud; return; }

    // Затемняем мир слабее, чем под меню: разговор идёт В мире, а не
    // поверх него, и собеседника должно быть видно.
    ui_.rect(0, 0, (f32)screenW_, (f32)screenH_,
             withAlpha(theme::Ink, (u8)(theme::ALPHA_SCRIM / 2)));

    const Rect panel = layout_.dialoguePanel();
    ui_.rect(panel.x, panel.y, panel.w, panel.h,
             withAlpha(theme::Panel, theme::ALPHA_PANEL));
    ui_.rectOutline(panel.x, panel.y, panel.w, panel.h,
                    layout_.dp(theme::STROKE_SELECTED_DP), theme::Accent);

    const f32 pad = layout_.dp(theme::PANEL_PAD_DP);
    f32 y = panel.y + pad;

    // ---- Кто говорит ----
    //
    // Имени не было: реплика висела в пустоте, и понять, с кем идёт
    // разговор, можно было только по тому, на кого смотришь.
    if (auto* reg = player.registryHandle()) {
        if (auto* tag = reg->get<npc::NpcTag>(dlg->npcEntity)) {
            const auto& def = npc::npcRegistry().get(tag->id);
            if (def.name) {
                ui_.text(npc::npcRegistry().name(tag->id), panel.x + pad, y,
                         theme::TEXT_LABEL, theme::Accent);
                y += ui_.textHeight(theme::TEXT_LABEL)
                   + layout_.dp(theme::SPACE_M_DP);
            }
        }
    }

    // ---- Реплика ----
    //
    // С переносом по словам: раньше длинная строка уходила за панель.
    const f32 textW = panel.w - pad * 2.f;
    y += ui_.textWrapped(node->text, panel.x + pad, y, textW,
                         theme::TEXT_BODY, theme::TextPrimary);
    y += layout_.dp(theme::SPACE_L_DP);

    // ---- Варианты ----
    for (usize i = 0; i < node->choices.size(); ++i) {
        const Rect cr = layout_.dialogueChoice(y, (u32)i);
        if (cr.y + cr.h > panel.y + panel.h - pad) break;   // не влезло

        // Игрока берём указателем, а не ссылкой на ссылку: обработчик
        // переживает кадр, а ссылочная переменная — нет.
        const int idx = ui_.pushInteractiveRect(cr,
            [this, pl = &player, wd = &world, i]() {
            auto* d = pl->activeDialogue();
            auto* reg = pl->registryHandle();
            if (!d || !d->active || !reg) return;
            npc::DialogueNode* n = d->findNode(d->currentNodeId);
            if (!n || i >= n->choices.size()) return;

            // Копия: applyChoice может сменить узел, а ссылка на
            // выбор живёт внутри прежнего.
            const npc::DialogueChoice chosen = n->choices[i];
            if (!npc::applyChoice(*reg, *wd, *d, chosen)) {
                if (onCloseDialogue) onCloseDialogue();
                screen = Screen::Hud;
            }
        });
        const bool pressed = ui_.isInteractivePressed(idx);

        ui_.rect(cr.x, cr.y, cr.w, cr.h,
                 pressed ? theme::Accent : theme::PanelRaised);
        ui_.rectOutline(cr.x, cr.y, cr.w, cr.h,
                        layout_.dp(pressed ? theme::STROKE_SELECTED_DP
                                           : theme::STROKE_DP),
                        pressed ? theme::AccentPressed : theme::Stroke);

        char line[256];
        std::snprintf(line, sizeof(line), "%d. %s",
                      (int)(i + 1), node->choices[i].text.c_str());
        ui_.text(line, cr.x + layout_.dp(theme::SPACE_M_DP),
                 cr.y + (cr.h - ui_.textHeight(theme::TEXT_BODY)) * 0.5f,
                 theme::TEXT_BODY, pressed ? theme::Ink : theme::TextPrimary);
    }
}

// ============================================================
// Quest Log
// ============================================================
void UiSystem::drawQuestLogScreen(player::Player& player) {
    drawMenuBackdrop(T(StrKey::Quest_Title));

    auto* log = player.questLog();
    auto* reg = player.registryHandle();
    if (!log || !reg) return;

    // ---- Список ----
    //
    // Раньше здесь для активных заданий печаталось ТОЛЬКО ИХ ЧИСЛО:
    // игрок с тремя заданиями видел «3». Ни названий, ни целей, ни
    // прогресса — то есть главный вопрос «что мне делать сейчас»
    // журнал не отвечал вовсе.
    // Список ПРОКРУЧИВАЕТСЯ, а не обрывается на первых строках.
    //
    // Строк помещается мало: на 1280x720 — две, на 2400x1080 — три.
    // Всё, что дальше, просто не рисовалось: игрок с пятью заданиями
    // видел два и не мог добраться до остальных. Ровно то же было у
    // ячеек сумки, и лечится оно тем же — прокруткой.
    const Rect listArea = layout_.questList();
    u32 row = 0;

    auto rowButton = [&](const std::string& label, f32 pct, bool done,
                         i32 questIdx) {
        const Rect base = layout_.questRow(row++);
        const Rect r{ base.x, base.y - questScroll.offset, base.w, base.h };

        // Строка видна, только если помещается целиком: графической
        // обрезки здесь нет, и половина строки поверх заголовка —
        // хуже, чем её отсутствие.
        if (r.y < listArea.y - 0.01f ||
            r.y + r.h > listArea.y + listArea.h + 0.01f) return;

        const int idx = ui_.pushInteractiveRect(r, [this, questIdx]() {
            selectedQuest = (selectedQuest == questIdx) ? -1 : questIdx;
        });
        const bool pressed  = ui_.isInteractivePressed(idx);
        const bool selected = (questIdx >= 0 && selectedQuest == questIdx);

        ui_.rect(r.x, r.y, r.w, r.h,
                 pressed ? theme::Accent : theme::PanelRaised);
        ui_.rectOutline(r.x, r.y, r.w, r.h,
                        layout_.dp(selected ? theme::STROKE_SELECTED_DP
                                            : theme::STROKE_DP),
                        selected ? theme::Accent : theme::Stroke);

        ui_.text(label, r.x + layout_.dp(theme::SPACE_M_DP),
                 r.y + layout_.dp(theme::SPACE_S_DP),
                 theme::TEXT_BODY,
                 pressed ? theme::Ink
                         : (done ? theme::TextSecondary : theme::TextPrimary));

        // Полоса прогресса прямо в строке: сколько осталось, видно
        // не открывая подробности.
        const f32 bh = layout_.dp(4.f);
        const f32 by = r.y + r.h - bh - layout_.dp(theme::SPACE_S_DP);
        const f32 bx = r.x + layout_.dp(theme::SPACE_M_DP);
        const f32 bw = r.w - layout_.dp(theme::SPACE_M_DP) * 2.f;
        ui_.rect(bx, by, bw, bh, theme::XpBed);
        ui_.rect(bx, by, bw * pct, bh, done ? theme::Success : theme::Accent);
    };

    // Активные — первыми: они и есть ответ на «что делать сейчас».
    ui_.text(T(StrKey::Quest_Active), layout_.questList().x,
             layout_.questList().y - layout_.dp(theme::SPACE_M_DP)
                 - ui_.textHeight(theme::TEXT_LABEL),
             theme::TEXT_LABEL, theme::Accent);

    if (log->activeQuests.empty()) {
        const Rect r = layout_.questRow(row++);
        ui_.text(T(StrKey::Quest_None), r.x + layout_.dp(theme::SPACE_M_DP),
                 r.y + (r.h - ui_.textHeight(theme::TEXT_BODY)) * 0.5f,
                 theme::TEXT_BODY, theme::TextDisabled);
    }

    for (usize i = 0; i < log->activeQuests.size(); ++i) {
        auto* q = reg->get<quests::Quest>(log->activeQuests[i]);
        if (!q) continue;
        rowButton(quests::questTitle(*q), q->progressPct(),
                  q->isComplete(), (i32)i);
    }

    // Выполненные — ниже и приглушённо.
    for (usize i = 0; i < log->history.size(); ++i) {
        const auto& h = log->history[log->history.size() - 1 - i];
        rowButton(quests::questTitle(h), 1.f, true, -1);
    }

    // Сколько строк вышло — столько и прокручивается.
    {
        const Rect probe = layout_.questRow(0);
        const f32 step = layout_.questRow(1).y - probe.y;
        questScroll.setContentHeight((f32)row * step, listArea.h);
        feedScrollDrag(questScroll, listArea);
    }

    drawQuestDetails(player);
}

// ============================================================
// Подробности задания
// ============================================================
void UiSystem::drawQuestDetails(player::Player& player) {
    const Rect d = layout_.questDetails();
    ui_.rect(d.x, d.y, d.w, d.h, theme::Panel);
    ui_.rectOutline(d.x, d.y, d.w, d.h, layout_.dp(theme::STROKE_DP),
                    theme::Stroke);

    auto* log = player.questLog();
    auto* reg = player.registryHandle();
    const f32 pad = layout_.dp(theme::PANEL_PAD_DP);

    if (!log || !reg || selectedQuest < 0 ||
        selectedQuest >= (i32)log->activeQuests.size()) {
        ui_.text(T(StrKey::Quest_None), d.x + pad, d.y + pad,
                 theme::TEXT_LABEL, theme::TextDisabled);
        return;
    }

    auto* q = reg->get<quests::Quest>(log->activeQuests[(usize)selectedQuest]);
    if (!q) return;

    const f32 tw = d.w - pad * 2.f;
    f32 y = d.y + pad;

    const std::string title = quests::questTitle(*q);
    if (!title.empty()) {
        y += ui_.textWrapped(title, d.x + pad, y, tw,
                             theme::TEXT_BODY, theme::TextPrimary);
        y += layout_.dp(theme::SPACE_M_DP);
    }
    const std::string desc = quests::questDescription(*q);
    if (!desc.empty()) {
        y += ui_.textWrapped(desc, d.x + pad, y, tw,
                             theme::TEXT_CAPTION, theme::TextSecondary);
        y += layout_.dp(theme::SPACE_L_DP);
    }

    // ---- Сколько сделано ----
    char prog[64];
    std::snprintf(prog, sizeof(prog), "%d / %d",
                  (int)q->progress, (int)q->tmpl.requiredCount);
    ui_.text(prog, d.x + pad, y, theme::TEXT_BODY,
             q->isComplete() ? theme::Success : theme::TextPrimary);
    y += ui_.textHeight(theme::TEXT_BODY) + layout_.dp(theme::SPACE_S_DP);

    const f32 bh = layout_.dp(6.f);
    ui_.rect(d.x + pad, y, tw, bh, theme::XpBed);
    ui_.rect(d.x + pad, y, tw * q->progressPct(), bh,
             q->isComplete() ? theme::Success : theme::Accent);
    y += bh + layout_.dp(theme::SPACE_L_DP);

    // ---- За что ----
    ui_.text(T(StrKey::Quest_Rewards), d.x + pad, y,
             theme::TEXT_LABEL, theme::Accent);
    y += ui_.textHeight(theme::TEXT_LABEL) + layout_.dp(theme::SPACE_S_DP);

    char rew[96];
    if (q->rewards.xp) {
        std::snprintf(rew, sizeof(rew), "%s +%llu", T(StrKey::Hud_Xp),
                      (unsigned long long)q->rewards.xp);
        ui_.text(rew, d.x + pad, y, theme::TEXT_CAPTION, theme::Xp);
        y += ui_.textHeight(theme::TEXT_CAPTION) + layout_.dp(theme::SPACE_XS_DP);
    }
    if (q->rewards.gold) {
        std::snprintf(rew, sizeof(rew), "%s +%u", T(StrKey::Hud_Gold),
                      (unsigned)q->rewards.gold);
        ui_.text(rew, d.x + pad, y, theme::TEXT_CAPTION, theme::Accent);
        y += ui_.textHeight(theme::TEXT_CAPTION) + layout_.dp(theme::SPACE_XS_DP);
    }
    if (q->rewards.itemCount) {
        // «x3» без названия — не обещание, а загадка. Номер здесь
        // блочный, имя спрашивается у реестра предметов ровно тем
        // же переводом, каким награда и выдаётся (grantRewards).
        const u16 itemId = items::items().blockToItem(q->rewards.itemBlockId);
        if (itemId != items::ITEM_NONE) {
            std::snprintf(rew, sizeof(rew), "%s x%u",
                          items::items().name(itemId),
                          (unsigned)q->rewards.itemCount);
            ui_.text(rew, d.x + pad, y, theme::TEXT_CAPTION, theme::TextSecondary);
        }
    }
}

// ============================================================
// Текущая цель на HUD
// ============================================================
//
// Чтобы узнать, что делать, приходилось открывать журнал — а журнал
// показывал только ЧИСЛО активных заданий. Строка на HUD отвечает на
// этот вопрос, не прерывая игру.
// ============================================================
// Откуда бьют
// ============================================================
//
// Полоса цели отвечает на вопрос «кого бью я». На «кто бьёт меня»
// не отвечало ничто: обзор на телефоне узкий, и удар со спины игрок
// читал только по убывающей полоске здоровья — то есть узнавал о
// нём последним и не знал, куда повернуться.
//
// Дуга по краю свободного центра, а не стрелка и не мигание всего
// экрана: направление читается мгновенно, место занято ровно то,
// которое игра и так держит пустым, а сила удара видна по толщине и
// яркости — слабый укус и удар в половину здоровья обязаны
// различаться.
/// Куда идти за оставленным на месте гибели.
///
/// В воксельном мире ночью место гибели не найти: примет нет, а
/// узелок размером с блок. Поэтому — указатель на краю круга, тем
/// же углом, каким считается направление удара.
///
/// Отдельного поля «где я умер» нет: узелок и есть эта память.
/// Ищется он по признаку «ждёт» — истлевающие выпавшие предметы
/// таким не помечены, и указатель на них не наводится.
void UiSystem::drawLostGoldMark(player::Player& player) {
    auto* reg = player.registryHandle();
    if (!reg) return;
    const auto* tf = reg->get<ecs::Transform>(player.entity());
    if (!tf) return;

    // Ближайший из ждущих: умереть можно и дважды, не собрав первый.
    const glm::vec3* best = nullptr;
    f32 bestD2 = 0.f;
    auto& pool = reg->pool<items::ItemPickup>();
    for (usize i = 0; i < pool.size(); ++i) {
        const ecs::Entity e = pool.entityAt((u32)i);
        const auto* pk = pool.get(e);
        if (!pk || !pk->waits) continue;
        const auto* ptf = reg->get<ecs::Transform>(e);
        if (!ptf) continue;
        const glm::vec3 d = ptf->position - tf->position;
        const f32 d2 = glm::dot(d, d);
        if (!best || d2 < bestD2) { best = &ptf->position; bestD2 = d2; }
    }
    if (!best) return;

    // Пришёл — указатель не нужен: узелок виден глазами и вот-вот
    // подберётся сам.
    if (bestD2 < 4.f) return;

    const f32 cx = (f32)screenW_ * 0.5f;
    const f32 cy = (f32)screenH_ * 0.5f;
    const f32 radius = layout_.hurtRingRadius();
    const f32 size   = layout_.hurtRingThickness();

    const f32 a = combat::hurtAngle(tf->position, viewYaw_, *best);
    const f32 px = cx + std::sin(a) * radius;
    const f32 py = cy - std::cos(a) * radius;

    ui_.rect(px - size * 0.5f, py - size * 0.5f, size, size,
             rgba(255, 220, 100, 210));
}

void UiSystem::drawHurtMarks(player::Player& player) {
    auto* reg = player.registryHandle();
    if (!reg) return;
    const auto* hm = reg->get<combat::HurtMarks>(player.entity());
    const auto* tf = reg->get<ecs::Transform>(player.entity());
    if (!hm || !tf) return;

    const f32 cx = (f32)screenW_ * 0.5f;
    const f32 cy = (f32)screenH_ * 0.5f;
    const f32 radius = layout_.hurtRingRadius();
    const f32 thick  = layout_.hurtRingThickness();

    for (const combat::HurtMark& m : hm->marks) {
        if (m.life <= 0.f) continue;

        const f32 fade = m.life / combat::HurtMarks::LIFETIME;
        const f32 a = combat::hurtAngle(tf->position, viewYaw_, m.from);

        // Дуга набирается короткими отрезками: своего примитива для
        // дуги у этого интерфейса нет, а заводить его ради одного
        // места — лишняя сущность.
        // Сегментов столько, чтобы квадраты перекрывались: при
        // семи между ними оставались просветы, и дуга читалась как
        // пунктир. Дуга должна быть дугой.
        constexpr u32 SEGMENTS = 13;
        const f32 half = ui::HudLayout::HURT_ARC_HALF;
        const f32 step = (half * 2.f) / (f32)SEGMENTS;
        const f32 grow = thick * (0.6f + 0.8f * m.weight);

        for (u32 i = 0; i < SEGMENTS; ++i) {
            const f32 ang = a - half + step * ((f32)i + 0.5f);
            // Экранные координаты: угол ноль — вверх (прямо перед
            // игроком), вправо — положительный.
            const f32 px = cx + std::sin(ang) * radius;
            const f32 py = cy - std::cos(ang) * radius;
            // К краям дуга сходит на нет — так она читается как
            // направление, а не как отрезок стены.
            const f32 w = grow * (0.45f + 0.55f * std::cos(
                              (ang - a) / half * 1.5707963f));
            const u8 alpha = (u8)(255.f * fade *
                                  (0.45f + 0.55f * m.weight));
            ui_.rect(px - w * 0.5f, py - w * 0.5f, w, w,
                     withAlpha(theme::Danger, alpha));
        }
    }
}

void UiSystem::drawQuestTracker(player::Player& player) {
    auto* log = player.questLog();
    auto* reg = player.registryHandle();
    if (!log || !reg || log->activeQuests.empty()) return;

    const quests::Quest* q = nullptr;
    for (auto e : log->activeQuests) {
        if (auto* c = reg->get<quests::Quest>(e)) { q = c; break; }
    }
    if (!q) return;
    const std::string title = quests::questTitle(*q);
    if (title.empty()) return;

    const Rect r = layout_.questTracker();
    ui_.text(title, r.x, r.y, theme::TEXT_CAPTION,
             hudTint(q->isComplete() ? theme::Success : theme::TextPrimary));

    char prog[48];
    std::snprintf(prog, sizeof(prog), "%d / %d",
                  (int)q->progress, (int)q->tmpl.requiredCount);
    ui_.text(prog, r.x, r.y + ui_.textHeight(theme::TEXT_CAPTION)
                      + layout_.dp(theme::SPACE_XS_DP),
             theme::TEXT_CAPTION, hudTint(theme::TextSecondary));
}

// ============================================================
// Reputation
// ============================================================
void UiSystem::drawReputationScreen(player::Player& player) {
    drawMenuBackdrop(T(StrKey::Rep_Title));

    auto* rep = player.reputation();
    if (!rep) return;

    drawCloseButton([this]() { screen = Screen::Hud; });

    // Ряды фракций — в области меню, а не от постоянных 80 и 150
    // точек: пять рядов по 56 dp плюс отступы не помещались в
    // высоту 720 и уезжали на пояс и за край.
    const Rect area = layout_.menuArea();
    const auto grid = layout_.cardGrid(area, 1, REP_ROW_H_DP);

    using factions::FactionId;
    FactionId facs[] = {
        FactionId::Villagers, FactionId::Traders, FactionId::Mages,
        FactionId::Bandits,   FactionId::Wildlings,
    };

    auto colorFor = [](factions::ReputationTier t) -> UiColor {
        using factions::ReputationTier;
        switch (t) {
            case ReputationTier::Hated:      return rgba(220, 40, 40, 255);
            case ReputationTier::Hostile:    return rgba(200, 80, 40, 255);
            case ReputationTier::Unfriendly: return rgba(200, 140, 40, 255);
            case ReputationTier::Neutral:    return rgba(180, 180, 180, 255);
            case ReputationTier::Friendly:   return rgba(120, 200, 120, 255);
            case ReputationTier::Honored:    return rgba(80, 200, 240, 255);
            case ReputationTier::Exalted:    return rgba(240, 200, 80, 255);
        }
        return COL_WHITE;
    };

    const f32 pad    = layout_.dp(theme::SPACE_L_DP);
    const f32 stroke = layout_.dp(theme::STROKE_DP);

    u32 i = 0;
    for (factions::FactionId f : facs) {
        const Rect r = grid.at(0, i++);
        // Ряд, не поместившийся целиком, не рисуется: фракций пять,
        // и на самом тесном экране видно не все. Лучше четыре
        // целых, чем пятая половиной поверх пояса.
        if (r.y + r.h > area.y + area.h + 0.5f) break;

        i32 v = rep->get(f);
        auto tier = rep->tier(f);

        ui_.rect(r.x, r.y, r.w, r.h, rgba(30, 40, 55, 240));
        ui_.rectOutline(r.x, r.y, r.w, r.h, stroke, COL_BLACK);

        const f32 tx = r.x + pad;
        f32 ty = r.y + pad;
        ui_.text(factions::factionName(f), tx, ty, theme::TEXT_BODY,
                 COL_WHITE);
        ty += ui_.textHeight(theme::TEXT_BODY)
            + layout_.dp(theme::SPACE_S_DP);

        char valueBuf[64];
        std::snprintf(valueBuf, sizeof(valueBuf), "%d  (%s)",
                      v, factions::tierName(tier));
        ui_.text(valueBuf, tx, ty, theme::TEXT_LABEL, colorFor(tier));

        f32 norm = ((f32)v + 1000.f) / 2000.f;
        if (norm < 0.f) norm = 0.f;
        if (norm > 1.f) norm = 1.f;

        const f32 barW = r.w * 0.40f;
        const f32 barH = layout_.dp(theme::SPACE_L_DP);
        const f32 barX = r.x + r.w - pad - barW;
        const f32 barY = r.y + (r.h - barH) * 0.5f;

        ui_.rect(barX - stroke, barY - stroke,
                 barW + stroke * 2.f, barH + stroke * 2.f, COL_BLACK);
        ui_.rect(barX, barY, barW, barH, rgba(40, 40, 40, 255));
        ui_.rect(barX, barY, barW * norm, barH, colorFor(tier));
    }
}

// ============================================================
// Save / Load
// ============================================================
void UiSystem::drawSaveLoadScreen(player::Player& player) {
    (void)player;
    const char* title = (saveLoadMode == SaveLoadMode::Save)
        ? cfg::tr("SAVE GAME") : cfg::tr("LOAD GAME");
    drawMenuBackdrop(title);

    drawCloseButton([this]() { screen = Screen::Hud; });

    // Переключатель режима — в строке заголовка, слева от кнопки
    // закрытия.
    //
    // Стоял он по постоянным (40, 74) — то есть ровно на полосах
    // здоровья и маны, которые считаются от плотности экрана и
    // кончаются кто где. Под вкладкой ему тоже не место: единственная
    // кнопка съедала бы целый ряд, а на 1280x720 это ряд карточек.
    {
        const Rect r = layout_.titleAction(MODE_BUTTON_W_DP);
        const char* label = (saveLoadMode == SaveLoadMode::Save)
            ? cfg::tr("MODE: SAVE") : cfg::tr("MODE: LOAD");
        int idx = ui_.pushInteractiveRect(r, [this]() {
            saveLoadMode = (saveLoadMode == SaveLoadMode::Save)
                ? SaveLoadMode::Load : SaveLoadMode::Save;
        });
        if (ui_.button(label, r, idx, rgba(80, 100, 140, 255), COL_WHITE)) {
            saveLoadMode = (saveLoadMode == SaveLoadMode::Save)
                ? SaveLoadMode::Load : SaveLoadMode::Save;
        }
    }

    const u32 P = save::SaveSlotManager::NUM_PROFILES;
    const u32 S = save::SaveSlotManager::NUM_SLOTS;

    // Сетка считается от области под вкладками, а не от постоянных
    // чисел: девять ячеек обязаны быть достижимы на любом экране.
    //
    // Достижимы — не значит «видны разом». На 1280x720 три ряда
    // карточек в полный рост не помещаются, и раньше нижний ряд
    // просто уходил за край экрана: три сохранения из девяти
    // достать было нельзя. Теперь ряды прокручиваются, а ряд, не
    // поместившийся целиком, не рисуется и не ловит касаний.
    const Rect area = layout_.menuArea();
    const auto grid = layout_.cardGrid(area, S, ui::HudLayout::SAVE_CARD_H_DP);
    const f32 pad = layout_.dp(theme::SPACE_M_DP);
    const f32 line = ui_.textHeight(theme::TEXT_LABEL)
                   + layout_.dp(theme::SPACE_XS_DP);

    saveScroll.setContentHeight((f32)P * grid.rowStride(),
                                (f32)grid.rowsVisible() * grid.rowStride());
    feedScrollDrag(saveScroll, area);

    for (u32 p = 0; p < P; ++p) {
        for (u32 s = 0; s < S; ++s) {
            const auto& meta = slotMeta[p][s];
            Rect cell = grid.at(s, p);
            cell.y -= saveScroll.offset;
            if (cell.y < area.y - 0.5f) continue;
            if (cell.y + cell.h > area.y + area.h + 0.5f) continue;

            int idx = ui_.pushInteractiveRect(cell, [this, p, s]() {
                if (saveLoadMode == SaveLoadMode::Save) {
                    if (onSaveRequested) onSaveRequested(p, s);
                } else {
                    if (onLoadRequested) onLoadRequested(p, s);
                }
            });

            bool pressed = ui_.isInteractivePressed(idx);
            UiColor bg = meta.exists
                ? (pressed ? rgba(80, 100, 80, 240) : rgba(30, 50, 40, 240))
                : (pressed ? rgba(80, 60, 60, 240) : rgba(40, 30, 30, 240));

            ui_.rect(cell.x, cell.y, cell.w, cell.h, bg);
            ui_.rectOutline(cell.x, cell.y, cell.w, cell.h,
                            layout_.dp(theme::STROKE_DP),
                            meta.exists ? rgba(180, 200, 160, 255)
                                        : rgba(120, 100, 100, 255));

            const f32 tx = cell.x + pad;
            f32 y = cell.y + pad;

            char hdr[48];
            std::snprintf(hdr, sizeof(hdr), cfg::tr("P%u  S%u"), p + 1, s + 1);
            ui_.text(hdr, tx, y, theme::TEXT_LABEL, COL_WHITE);
            y += line;

            if (meta.exists) {
                ui_.text(meta.worldName, tx, y, theme::TEXT_LABEL,
                         rgba(200, 220, 255, 255));
                y += line;

                char buf[64];
                std::snprintf(buf, sizeof(buf), cfg::tr("Lv %u  |  %us"),
                              meta.playerLevel, meta.playtimeSec);
                ui_.text(buf, tx, y, theme::TEXT_CAPTION,
                         rgba(220, 220, 180, 255));
                y += line;

                time_t ts = (time_t)(meta.timestampMs / 1000ULL);
                struct tm* tmv = std::localtime(&ts);
                if (tmv) {
                    char dateBuf[64];
                    std::snprintf(dateBuf, sizeof(dateBuf),
                                  "%02d %s %04d  %02d:%02d",
                                  tmv->tm_mday,
                                  cfg::tr(gMonthNames[tmv->tm_mon % 12]),
                                  tmv->tm_year + 1900,
                                  tmv->tm_hour, tmv->tm_min);
                    ui_.text(dateBuf, tx, y, theme::TEXT_CAPTION,
                             rgba(200, 200, 200, 255));
                }

                // Кнопка удаления — цель касания обычного размера, а
                // не 36 точек, и прижата к правому нижнему углу
                // ячейки.
                const f32 delW = layout_.dp(theme::TOUCH_PRIMARY_DP);
                const f32 delH = layout_.dp(theme::TOUCH_MIN_DP);
                Rect del{ cell.x + cell.w - delW - pad,
                          cell.y + cell.h - delH - pad, delW, delH };
                int di = ui_.pushInteractiveRect(del, [this, p, s]() {
                    if (onDeleteRequested) onDeleteRequested(p, s);
                });
                bool dpressed = ui_.isInteractivePressed(di);
                ui_.rect(del.x, del.y, del.w, del.h,
                         dpressed ? rgba(200, 60, 60, 255)
                                  : rgba(120, 40, 40, 220));
                ui_.rectOutline(del.x, del.y, del.w, del.h,
                                layout_.dp(theme::STROKE_DP), COL_BLACK);
                const char* dl = cfg::tr("DEL");
                const f32 dw = ui_.textWidth(dl, theme::TEXT_LABEL);
                ui_.text(dl, del.x + (del.w - dw) * 0.5f,
                         del.y + (del.h - ui_.textHeight(theme::TEXT_LABEL)) * 0.5f,
                         theme::TEXT_LABEL, COL_WHITE);
            } else {
                ui_.text(cfg::tr("Empty"), tx, y, theme::TEXT_BODY,
                         rgba(160, 160, 160, 255));
            }
        }
    }
}

// ============================================================
// Миры: список, создание, удаление
// ============================================================
//
// Мир был один и создавался молча при запуске. Выбрать другой,
// назвать свой или расстаться со старым было нечем: девять слотов
// сохранения показывали «World A1B2C3D4» и умели только «сохранить»
// и «загрузить».
//
// Здесь мир становится вещью, о которой игрок знает: у него есть имя,
// его открывают из списка, его удаляют с вопросом и его можно унести
// файлом.

namespace {

/// Кнопка с подписью. Одна на весь раздел: три разных вида кнопок на
/// одном экране читаются как три разных по важности действия.
struct BtnStyle {
    UiColor fill, fillPressed, stroke;
};

const BtnStyle BTN_NORMAL { theme::PanelRaised, theme::Accent, theme::Stroke };
const BtnStyle BTN_PRIMARY{ theme::Accent, theme::AccentPressed, theme::Accent };
const BtnStyle BTN_DANGER { theme::Danger, theme::AccentPressed, theme::Danger };

} // namespace

void UiSystem::drawWorldsScreen() {
    ui_.rect(0, 0, (f32)screenW_, (f32)screenH_,
             withAlpha(theme::Ink, theme::ALPHA_SCRIM));

    const Rect title = layout_.menuTitle();
    ui_.text(T(StrKey::World_Title), title.x,
             title.y + (title.h - ui_.textHeight(theme::TEXT_TITLE)) * 0.5f,
             theme::TEXT_TITLE, theme::TextPrimary);

    drawCloseButton([this]() { screen = returnTo; returnTo = Screen::Hud; });

    auto textButton = [&](Rect r, const char* label, const BtnStyle& st,
                          f32 size, std::function<void()> onClick)
    {
        const int idx = ui_.pushInteractiveRect(r, std::move(onClick));
        const bool pressed = ui_.isInteractivePressed(idx);
        ui_.rect(r.x, r.y, r.w, r.h, pressed ? st.fillPressed : st.fill);
        ui_.rectOutline(r.x, r.y, r.w, r.h, layout_.dp(theme::STROKE_DP),
                        st.stroke);
        const f32 tw = ui_.textWidth(label, size);
        ui_.text(label, r.x + (r.w - tw) * 0.5f,
                 r.y + (r.h - ui_.textHeight(size)) * 0.5f, size,
                 pressed ? theme::Ink : theme::TextPrimary);
    };

    constexpr u32 COLS = 3;
    const u32 P = save::SaveSlotManager::NUM_PROFILES;
    const u32 S = save::SaveSlotManager::NUM_SLOTS;
    // Верхний ряд — «новый мир», под ним сетка миров.
    const u32 ROWS = 1 + P;

    // ---- Новый мир: во всю ширину, главное действие экрана ----
    {
        const Rect a = layout_.menuCell(0, 0, COLS, ROWS);
        const Rect c2 = layout_.menuCell(COLS - 1, 0, COLS, ROWS);
        const Rect r{ a.x, a.y, (c2.x + c2.w) - a.x, a.h };
        textButton(r, T(StrKey::World_New), BTN_PRIMARY, theme::TEXT_BODY,
                   [this]() { openNewWorld(); });
    }

    const f32 pad = layout_.dp(theme::SPACE_S_DP);
    const f32 small = theme::TEXT_CAPTION;

    for (u32 p = 0; p < P; ++p) {
        for (u32 s = 0; s < S; ++s) {
            const Rect cell = layout_.menuCell(s, p + 1, COLS, ROWS);
            const auto& meta = slotMeta[p][s];

            // Кнопки в углу ячейки, а сама ячейка — «открыть».
            const f32 btnH = layout_.dp(theme::TOUCH_MIN_DP) * 0.7f;
            const f32 btnW = (cell.w - pad * 3.f) * 0.5f;
            const Rect bLeft { cell.x + pad, cell.y + cell.h - pad - btnH,
                               btnW, btnH };
            const Rect bRight{ bLeft.x + btnW + pad, bLeft.y, btnW, btnH };

            const bool current = meta.exists && meta.seed == currentWorldSeed;

            // Ссылку на meta в обработчик не кладём: он переживает
            // кадр, а список слотов между кадрами перечитывается.
            const bool exists = meta.exists;
            const int idx = ui_.pushInteractiveRect(cell, [this, p, s, exists]() {
                if (exists && onLoadRequested) onLoadRequested(p, s);
            });
            const bool pressed = ui_.isInteractivePressed(idx) && exists;

            ui_.rect(cell.x, cell.y, cell.w, cell.h,
                     pressed ? theme::Accent
                             : (meta.exists ? theme::PanelRaised : theme::Panel));
            ui_.rectOutline(cell.x, cell.y, cell.w, cell.h,
                            layout_.dp(current ? theme::STROKE_SELECTED_DP
                                               : theme::STROKE_DP),
                            current ? theme::Accent : theme::Stroke);

            f32 ty = cell.y + pad;
            if (!meta.exists) {
                ui_.text(T(StrKey::World_Empty), cell.x + pad, ty, small,
                         theme::TextDisabled);
                // В пустой мир можно ПРИНЕСТИ файл — это единственное,
                // что с пустой ячейкой вообще можно сделать.
                textButton(bRight, T(StrKey::World_Import), BTN_NORMAL, small,
                           [this, p, s]() {
                               if (onImportRequested) onImportRequested(p, s);
                           });
                continue;
            }

            ui_.text(meta.worldName, cell.x + pad, ty, theme::TEXT_LABEL,
                     pressed ? theme::Ink : theme::TextPrimary);
            ty += ui_.textHeight(theme::TEXT_LABEL) + layout_.dp(theme::SPACE_XS_DP);

            char buf[96];
            std::snprintf(buf, sizeof(buf), "%s %llu", T(StrKey::World_Seed),
                          (unsigned long long)meta.seed);
            ui_.text(buf, cell.x + pad, ty, small, theme::TextSecondary);
            ty += ui_.textHeight(small) + layout_.dp(theme::SPACE_XS_DP);

            std::snprintf(buf, sizeof(buf), "%s %u   %u:%02u",
                          T(StrKey::Hud_Level), meta.playerLevel,
                          meta.playtimeSec / 3600u,
                          (meta.playtimeSec / 60u) % 60u);
            ui_.text(buf, cell.x + pad, ty, small, theme::TextSecondary);
            ty += ui_.textHeight(small) + layout_.dp(theme::SPACE_XS_DP);

            if (current)
                ui_.text(T(StrKey::World_Current), cell.x + pad, ty, small,
                         theme::Accent);

            textButton(bLeft, T(StrKey::World_Export), BTN_NORMAL, small,
                       [this, p, s]() {
                           if (onExportRequested) onExportRequested(p, s);
                       });
            // Удаление спрашивает: мир, которого не вернуть, не
            // удаляют одним касанием по тесной кнопке.
            textButton(bRight, T(StrKey::Delete), BTN_DANGER, small,
                       [this, p, s]() {
                           askConfirm(T(StrKey::World_DeleteAsk),
                                      T(StrKey::Delete), [this, p, s]() {
                                          if (onDeleteRequested)
                                              onDeleteRequested(p, s);
                                      });
                       });
        }
    }
}

NewWorldLayout UiSystem::newWorldLayout() const {
    NewWorldLayout out;
    const Rect area = layout_.menuArea();
    const f32 gap = layout_.dp(theme::SPACE_M_DP);
    const f32 rowH = layout_.dp(theme::TOUCH_REGULAR_DP);

    // Всё в один ряд, а не столбиком. Столбиком поля с кнопкой
    // занимали столько, что клавиатуре оставалось меньше высоты
    // одной клавиши: она вылезала за экран и накрывала «создать»,
    // и нажать его было нельзя вовсе.
    const f32 w = area.w - gap * 3.f;
    const f32 nameW   = w * 0.34f;
    const f32 seedW   = w * 0.34f;
    const f32 randW   = w * 0.14f;
    const f32 createW = w - nameW - seedW - randW;

    out.name   = { area.x, area.y, nameW, rowH };
    out.seed   = { out.name.x + nameW + gap, area.y, seedW, rowH };
    out.random = { out.seed.x + seedW + gap, area.y, randW, rowH };
    out.create = { out.random.x + randW + gap, area.y, createW, rowH };

    // Клавиатуре — вся оставшаяся высота и вся ширина: ряд из
    // одиннадцати клавиш узок и в полную ширину экрана.
    const f32 kbTop = area.y + rowH + gap;
    out.keyboard = { area.x, kbTop, area.w, area.y + area.h - kbTop };
    return out;
}

// ============================================================
// Изометрический снимок мира
// ============================================================
//
// Экран ничего не рисует сам и ничего не считает: размер и сторону
// выбирает игрок, всё остальное делает render::IsoSnapshot, а сюда
// приходит только его состояние. Предпросмотр — настоящая картинка
// того же рендера, положенная в текстуру; своей приблизительной
// отрисовки здесь нет и быть не должно, иначе игрок выбирал бы по
// одному изображению, а получал другое.
void UiSystem::setPreviewImage(VkImageView view, VkSampler sampler) {
    ui_.setImage(view, sampler);
}

void UiSystem::drawIsoSnapshotScreen() {
    ui_.rect(0, 0, (f32)screenW_, (f32)screenH_,
             withAlpha(theme::Ink, theme::ALPHA_SCRIM));

    const Rect title = layout_.menuTitle();
    ui_.text(T(StrKey::Iso_Title), title.x,
             title.y + (title.h - ui_.textHeight(theme::TEXT_TITLE)) * 0.5f,
             theme::TEXT_TITLE, theme::TextPrimary);

    const bool busy = iso.capturing &&
                      (iso.stage == 1 || iso.stage == 2 || iso.stage == 3);

    drawCloseButton([this]() {
        if (onIsoCancel) onIsoCancel();
        openScreen(Screen::Settings);
    });

    const Rect panel = layout_.menuArea();
    ui_.rect(panel.x, panel.y, panel.w, panel.h, theme::Panel);
    ui_.rectOutline(panel.x, panel.y, panel.w, panel.h,
                    layout_.dp(theme::STROKE_DP), theme::Stroke);

    const f32 pad  = layout_.dp(theme::PANEL_PAD_DP);
    const f32 gap  = layout_.dp(theme::SPACE_M_DP);
    const f32 rowH = layout_.dp(theme::TOUCH_REGULAR_DP);

    auto textButton = [&](Rect r, const char* label, const BtnStyle& st,
                          f32 size, bool enabled, std::function<void()> onClick)
    {
        const int idx = enabled
            ? ui_.pushInteractiveRect(r, std::move(onClick)) : -1;
        const bool pressed = enabled && ui_.isInteractivePressed(idx);
        ui_.rect(r.x, r.y, r.w, r.h,
                 enabled ? (pressed ? st.fillPressed : st.fill) : theme::Panel);
        ui_.rectOutline(r.x, r.y, r.w, r.h, layout_.dp(theme::STROKE_DP),
                        enabled ? st.stroke : theme::Stroke);
        const f32 tw = ui_.textWidth(label, size);
        ui_.text(label, r.x + (r.w - tw) * 0.5f,
                 r.y + (r.h - ui_.textHeight(size)) * 0.5f, size,
                 !enabled ? theme::TextDisabled
                          : (pressed ? theme::Ink : theme::TextPrimary));
    };

    // ---- Левая колонка: настройки. Правая: предпросмотр. ----
    const f32 colGap = layout_.dp(theme::SPACE_L_DP);
    const f32 leftW  = (panel.w - pad * 2.f - colGap) * 0.42f;
    const f32 leftX  = panel.x + pad;
    const f32 rightX = leftX + leftW + colGap;
    const f32 rightW = panel.w - pad * 2.f - leftW - colGap;
    f32 y = panel.y + pad;

    // ---- Центр области ----
    {
        char buf[128];
        std::snprintf(buf, sizeof(buf), "%s: %d, %d",
                      T(StrKey::Iso_Center), iso.centerX, iso.centerZ);
        ui_.text(buf, leftX, y, theme::TEXT_LABEL, theme::TextSecondary);
        y += ui_.textHeight(theme::TEXT_LABEL) + gap;
    }

    // ---- Размер области ----
    {
        ui_.text(T(StrKey::Iso_Size), leftX, y, theme::TEXT_LABEL,
                 theme::TextSecondary);
        y += ui_.textHeight(theme::TEXT_LABEL) + layout_.dp(theme::SPACE_XS_DP);

        // Шаг ступенями: 16..512 удваивается не всегда, поэтому
        // список фиксированный — так игрок не подбирает число
        // колесом, а выбирает из осмысленных.
        static constexpr i32 STEPS[] = { 32, 64, 100, 160, 256, 384, 512 };
        constexpr int STEP_COUNT = (int)(sizeof(STEPS) / sizeof(STEPS[0]));

        const f32 bw = (leftW - gap * 2.f) / 3.f;
        const Rect minus{ leftX, y, bw, rowH };
        const Rect value{ leftX + bw + gap, y, bw, rowH };
        const Rect plus { leftX + (bw + gap) * 2.f, y, bw, rowH };

        auto stepIndex = [&]() {
            int best = 0;
            for (int i = 0; i < STEP_COUNT; ++i)
                if (STEPS[i] <= iso.size) best = i;
            return best;
        };

        textButton(minus, "-", BTN_NORMAL, theme::TEXT_BODY,
                   !busy && iso.size > STEPS[0], [this, stepIndex]() {
            static constexpr i32 S[] = { 32, 64, 100, 160, 256, 384, 512 };
            const int i = stepIndex();
            iso.size = S[i > 0 ? i - 1 : 0];
            if (onIsoParamsChanged) onIsoParamsChanged();
        });

        char buf[64];
        std::snprintf(buf, sizeof(buf), "%d x %d", iso.size, iso.size);
        ui_.rect(value.x, value.y, value.w, value.h, theme::PanelRaised);
        ui_.rectOutline(value.x, value.y, value.w, value.h,
                        layout_.dp(theme::STROKE_DP), theme::Stroke);
        const f32 tw = ui_.textWidth(buf, theme::TEXT_BODY);
        ui_.text(buf, value.x + (value.w - tw) * 0.5f,
                 value.y + (value.h - ui_.textHeight(theme::TEXT_BODY)) * 0.5f,
                 theme::TEXT_BODY, theme::TextPrimary);

        textButton(plus, "+", BTN_NORMAL, theme::TEXT_BODY,
                   !busy && iso.size < STEPS[STEP_COUNT - 1],
                   [this, stepIndex]() {
            static constexpr i32 S[] = { 32, 64, 100, 160, 256, 384, 512 };
            constexpr int N = (int)(sizeof(S) / sizeof(S[0]));
            const int i = stepIndex();
            iso.size = S[i + 1 < N ? i + 1 : N - 1];
            if (onIsoParamsChanged) onIsoParamsChanged();
        });
        y += rowH + gap;
    }

    // ---- Сторона обзора ----
    {
        ui_.text(T(StrKey::Iso_View), leftX, y, theme::TEXT_LABEL,
                 theme::TextSecondary);
        y += ui_.textHeight(theme::TEXT_LABEL) + layout_.dp(theme::SPACE_XS_DP);

        const config::StrKey names[4] = {
            StrKey::Iso_North, StrKey::Iso_East,
            StrKey::Iso_South, StrKey::Iso_West,
        };
        const f32 bw = (leftW - gap * 3.f) / 4.f;
        for (u32 i = 0; i < 4; ++i) {
            const Rect r{ leftX + (bw + gap) * (f32)i, y, bw, rowH };
            const bool active = (iso.view == i);
            const int idx = busy ? -1 : ui_.pushInteractiveRect(r, [this, i]() {
                iso.view = i;
                if (onIsoParamsChanged) onIsoParamsChanged();
            });
            const bool pressed = !busy && ui_.isInteractivePressed(idx);
            ui_.rect(r.x, r.y, r.w, r.h,
                     active ? theme::Accent
                            : (pressed ? theme::PanelRaised : theme::Panel));
            ui_.rectOutline(r.x, r.y, r.w, r.h,
                            layout_.dp(active ? theme::STROKE_SELECTED_DP
                                              : theme::STROKE_DP),
                            active ? theme::AccentPressed : theme::Stroke);
            const char* nm = T(names[i]);
            const f32 tw = ui_.textWidth(nm, theme::TEXT_LABEL);
            ui_.text(nm, r.x + (r.w - tw) * 0.5f,
                     r.y + (r.h - ui_.textHeight(theme::TEXT_LABEL)) * 0.5f,
                     theme::TEXT_LABEL,
                     active ? theme::Ink : theme::TextPrimary);
        }
        y += rowH + gap;
    }

    // ---- Состояние: что сейчас происходит ----
    {
        const char* msg = nullptr;
        UiColor col = theme::TextSecondary;
        if (iso.stage == 5) {
            switch (iso.error) {
                case 1: msg = T(StrKey::Iso_ErrMemory);     break;
                case 2: msg = T(StrKey::Iso_ErrGeneration); break;
                case 3: msg = T(StrKey::Iso_ErrTarget);     break;
                default: msg = T(StrKey::Iso_ErrSave);      break;
            }
            col = theme::Danger;
        } else if (iso.capturing && iso.stage == 3) {
            msg = T(StrKey::Iso_Rendering);
        } else if (iso.capturing && (iso.stage == 1 || iso.stage == 2)) {
            msg = T(StrKey::Iso_Preparing);
        } else if (!iso.savedPath.empty()) {
            msg = T(StrKey::Iso_Saved);
            col = theme::Success;
        }
        if (msg) {
            ui_.text(msg, leftX, y, theme::TEXT_BODY, col);
            y += ui_.textHeight(theme::TEXT_BODY) + layout_.dp(theme::SPACE_XS_DP);
        }

        if (busy) {
            const f32 barH = layout_.dp(theme::SPACE_S_DP);
            ui_.rect(leftX, y, leftW, barH, theme::Ink);
            const f32 p = iso.progress < 0.f ? 0.f
                        : (iso.progress > 1.f ? 1.f : iso.progress);
            if (p > 0.f) ui_.rect(leftX, y, leftW * p, barH, theme::Success);
            y += barH + layout_.dp(theme::SPACE_XS_DP);
        }

        // Путь показываем целиком: без него «сохранено» не отвечает
        // на единственный вопрос, который у игрока остался.
        if (!iso.savedPath.empty() && iso.stage != 5) {
            char buf[256];
            std::snprintf(buf, sizeof(buf), "%ux%u", iso.outW, iso.outH);
            ui_.text(buf, leftX, y, theme::TEXT_CAPTION, theme::TextSecondary);
            y += ui_.textHeight(theme::TEXT_CAPTION) + layout_.dp(theme::SPACE_XS_DP);
            ui_.text(iso.savedPath, leftX, y, theme::TEXT_CAPTION,
                     theme::TextSecondary);
            y += ui_.textHeight(theme::TEXT_CAPTION) + gap;
        }
    }

    // ---- Предпросмотр: настоящая картинка того же рендера ----
    {
        const f32 previewH = panel.h - pad * 2.f - rowH - gap;
        const Rect pv{ rightX, panel.y + pad, rightW, previewH };
        ui_.rect(pv.x, pv.y, pv.w, pv.h, theme::Ink);
        ui_.rectOutline(pv.x, pv.y, pv.w, pv.h,
                        layout_.dp(theme::STROKE_DP), theme::Stroke);

        if (ui_.hasImage() && iso.outW > 0 && iso.outH > 0) {
            // Вписываем картинку целиком, сохраняя пропорции: снимок
            // почти никогда не той же формы, что окно предпросмотра,
            // а растянутая изометрия — уже не изометрия.
            const f32 sx = pv.w / (f32)iso.outW;
            const f32 sy = pv.h / (f32)iso.outH;
            const f32 sc = sx < sy ? sx : sy;
            const f32 iw = (f32)iso.outW * sc;
            const f32 ih = (f32)iso.outH * sc;
            ui_.imageQuad(pv.x + (pv.w - iw) * 0.5f,
                          pv.y + (pv.h - ih) * 0.5f, iw, ih, COL_WHITE);
        } else {
            const char* wait = T(StrKey::Iso_PreviewWait);
            const f32 tw = ui_.textWidth(wait, theme::TEXT_LABEL);
            ui_.text(wait, pv.x + (pv.w - tw) * 0.5f,
                     pv.y + (pv.h - ui_.textHeight(theme::TEXT_LABEL)) * 0.5f,
                     theme::TEXT_LABEL, theme::TextDisabled);
        }

        // ---- Готово / Отмена ----
        const f32 bw = (rightW - gap) * 0.5f;
        const f32 by = pv.y + pv.h + gap;
        textButton({ rightX, by, bw, rowH }, T(StrKey::Cancel),
                   BTN_NORMAL, theme::TEXT_BODY, true, [this]() {
            if (onIsoCancel) onIsoCancel();
            openScreen(Screen::Settings);
        });
        textButton({ rightX + bw + gap, by, bw, rowH }, T(StrKey::Iso_Done),
                   BTN_PRIMARY, theme::TEXT_BODY, !busy, [this]() {
            if (onIsoCapture) onIsoCapture();
        });
    }
}

void UiSystem::drawNewWorldScreen() {
    ui_.rect(0, 0, (f32)screenW_, (f32)screenH_,
             withAlpha(theme::Ink, theme::ALPHA_SCRIM));

    const Rect title = layout_.menuTitle();
    ui_.text(T(StrKey::World_New), title.x,
             title.y + (title.h - ui_.textHeight(theme::TEXT_TITLE)) * 0.5f,
             theme::TEXT_TITLE, theme::TextPrimary);

    drawCloseButton([this]() { openScreen(Screen::Worlds); });

    const NewWorldLayout NW = newWorldLayout();
    const f32 pad = layout_.dp(theme::SPACE_M_DP);
    const f32 small = theme::TEXT_CAPTION;

    auto textButton = [&](Rect r, const char* label, const BtnStyle& st,
                          f32 size, std::function<void()> onClick)
    {
        const int idx = ui_.pushInteractiveRect(r, std::move(onClick));
        const bool pressed = ui_.isInteractivePressed(idx);
        ui_.rect(r.x, r.y, r.w, r.h, pressed ? st.fillPressed : st.fill);
        ui_.rectOutline(r.x, r.y, r.w, r.h, layout_.dp(theme::STROKE_DP),
                        st.stroke);
        const f32 tw = ui_.textWidth(label, size);
        ui_.text(label, r.x + (r.w - tw) * 0.5f,
                 r.y + (r.h - ui_.textHeight(size)) * 0.5f, size,
                 pressed ? theme::Ink : theme::TextPrimary);
    };

    // ---- Поле: подпись слева, набранное внутри рамки ----
    auto field = [&](Rect r, const char* label, TextEntry& te, WorldField which,
                     const char* hint)
    {
        const bool focused = (newWorldField == which);
        const int idx = ui_.pushInteractiveRect(r, [this, which]() {
            newWorldField = which;
        });
        (void)idx;
        ui_.rect(r.x, r.y, r.w, r.h, theme::Panel);
        ui_.rectOutline(r.x, r.y, r.w, r.h,
                        layout_.dp(focused ? theme::STROKE_SELECTED_DP
                                           : theme::STROKE_DP),
                        focused ? theme::Accent : theme::Stroke);

        ui_.text(label, r.x + pad, r.y + layout_.dp(theme::SPACE_XS_DP),
                 small, theme::TextSecondary);

        const f32 ty = r.y + r.h - pad - ui_.textHeight(theme::TEXT_BODY);
        if (te.empty()) {
            // Подсказка серым: пустое поле без неё не объясняет, что
            // будет, если ничего не набирать.
            ui_.text(hint, r.x + pad, ty, theme::TEXT_BODY, theme::TextDisabled);
        } else {
            ui_.text(te.text(), r.x + pad, ty, theme::TEXT_BODY,
                     theme::TextPrimary);
        }
        // Курсор виден только у того поля, которое набирают.
        if (focused) {
            const f32 tw = te.empty() ? 0.f : ui_.textWidth(te.text(), theme::TEXT_BODY);
            ui_.rect(r.x + pad + tw + layout_.dp(theme::SPACE_XS_DP), ty,
                     layout_.dp(theme::STROKE_SELECTED_DP),
                     ui_.textHeight(theme::TEXT_BODY), theme::Accent);
        }
    };

    // Подсказка в пустом поле имени — не «пусто», а то, КАК мир
    // будет называться, если имя не набирать. Пока зерно не задано,
    // сказать нечего: оно будет случайным.
    char nameHint[world::WORLD_NAME_CAP];
    if (newWorldSeed.empty()) {
        std::snprintf(nameHint, sizeof(nameHint), "%s",
                      T(StrKey::World_RandomSeed));
    } else {
        world::defaultWorldName(nameHint, sizeof(nameHint),
                                world::seedFromText(newWorldSeed.text()));
    }

    field(NW.name, T(StrKey::World_Name), newWorldName, WorldField::Name,
          nameHint);
    field(NW.seed, T(StrKey::World_Seed), newWorldSeed, WorldField::Seed,
          T(StrKey::World_RandomSeed));

    // Кнопка «случайно» рядом с полем зерна: она именно про него.
    textButton(NW.random,
               T(StrKey::World_RandomSeed), BTN_NORMAL, small, [this]() {
                   char buf[24];
                   std::snprintf(buf, sizeof(buf), "%llu",
                                 (unsigned long long)
                                 (world::randomWorldSeed() % 1000000000ull));
                   newWorldSeed.setText(buf);
                   newWorldField = WorldField::Seed;
               });

    // ---- Создать ----
    textButton(NW.create, T(StrKey::World_Create), BTN_PRIMARY,
               theme::TEXT_BODY, [this]() {
                   if (onCreateWorld)
                       onCreateWorld(newWorldName.text(), newWorldSeed.text());
               });

    // ---- Клавиатура: всё, что осталось между полями и кнопкой ----
    drawKeyboard(NW.keyboard);
}

// ============================================================
// Экранная клавиатура
// ============================================================
//
// Своя, а не системная: системную поднимают через JNI и InputMethod,
// а её ответ приходит в другом потоке и другим событием. Здесь же
// набирают два коротких поля, и ради них тащить половину Android в
// игровой цикл незачем.
void UiSystem::drawKeyboard(Rect area) {
    if (area.h <= 0.f || area.w <= 0.f) return;

    // Раскладка берётся у общей функции, а не считается здесь: по ней
    // же проверка и нажимает клавиши. Две копии одних правил
    // расходятся молча — палец попадает мимо, и ввода просто нет.
    const KeyboardLayout L = keyboardLayout(
        keyPage, area, layout_.dp(theme::SPACE_XS_DP),
        layout_.dp(theme::TOUCH_MIN_DP));

    auto keyCap = [&](Rect r, const char* label, bool service,
                      std::function<void()> onClick)
    {
        const int idx = ui_.pushInteractiveRect(r, std::move(onClick));
        const bool pressed = ui_.isInteractivePressed(idx);
        ui_.rect(r.x, r.y, r.w, r.h,
                 pressed ? theme::Accent
                         : (service ? theme::Panel : theme::PanelRaised));
        ui_.rectOutline(r.x, r.y, r.w, r.h, layout_.dp(theme::STROKE_DP),
                        theme::Stroke);
        const f32 tw = ui_.textWidth(label, theme::TEXT_LABEL);
        ui_.text(label, r.x + (r.w - tw) * 0.5f,
                 r.y + (r.h - ui_.textHeight(theme::TEXT_LABEL)) * 0.5f,
                 theme::TEXT_LABEL, pressed ? theme::Ink : theme::TextPrimary);
    };

    for (u32 row = 0; row < keyRowCount(keyPage); ++row) {
        const u32 count = keysInRow(keyPage, row);
        for (u32 col = 0; col < count; ++col) {
            const u32 cp = keyAt(keyPage, row, col);
            char label[4] = {};
            if (cp < 0x80) {
                label[0] = (char)cp;
            } else {
                label[0] = (char)(0xC0 | (cp >> 6));
                label[1] = (char)(0x80 | (cp & 0x3F));
            }
            keyCap(L.keyRect(row, col), label, false,
                   [this, cp]() { activeField().insert(cp); });
        }
    }

    keyCap(L.pageRect(), keyPageName(nextKeyPage(keyPage)), true,
           [this]() { keyPage = nextKeyPage(keyPage); });
    keyCap(L.spaceRect(), "___", true,
           [this]() { activeField().insert((u32)' '); });
    keyCap(L.backspaceRect(), "<-", true,
           [this]() { activeField().backspace(); });
}

// ============================================================
// Crafting
// ============================================================
void UiSystem::drawCraftingScreen(player::Player& player) {
    // Без станка заголовок говорит «на ходу», а не прочерк: прочерк
    // читается как ошибка, а не как «руками и на костре».
    const char* stName = nearbyStation == crafting::StationType::None
                       ? T(StrKey::Craft_ByHand)
                       : crafting::stationName(nearbyStation);
    char title[96];
    std::snprintf(title, sizeof(title), "%s — %s",
                  T(StrKey::Craft_Title), stName);
    drawMenuBackdrop(title);

    drawCloseButton([this]() { screen = Screen::Hud; });

    auto* inv = player.inventory();
    auto* tree = player.skillTree();
    auto* prog = player.progression();
    if (!inv) return;

    crafting::CraftContext ctx{};
    ctx.inventory     = inv;
    ctx.skillTree     = tree;
    ctx.playerLevel   = prog ? prog->level : 1;
    ctx.nearbyStation = nearbyStation;
    ctx.craftTierBonus = player.derived().craftTierBonus;

    std::vector<crafting::AvailableRecipe> available;
    crafting::gatherAvailable(ctx, available);

    // Список слева, подробности справа — та самая форма, ради
    // которой в раскладке есть paneLeft/paneRight/paneRow. Написаны
    // они были давно и не звались ни одним экраном: ремесло считало
    // своё место само, от постоянных 40 и 130 точек и доли ширины
    // экрана. На 1280x720 первая строка ложилась на полосы
    // ресурсов, правая панель накрывала кнопку закрытия, а список и
    // панель заходили на пояс.
    const Rect listArea = layout_.paneLeft();
    const Rect pane     = layout_.paneRight();
    const f32 rowH   = layout_.dp(theme::TOUCH_REGULAR_DP);
    const f32 rowGap = layout_.dp(theme::SPACE_S_DP);
    const f32 pad    = layout_.dp(theme::PANEL_PAD_DP);

    const i32 visible = (i32)layout_.paneRowsVisible();
    const i32 total = (i32)available.size();

    craftScroll.setContentHeight((f32)total * (rowH + rowGap),
                                 (f32)visible * (rowH + rowGap));

    // Список листается и пальцем, а не одними кнопками «вверх» и
    // «вниз»: перетаскивание у Scroll написано с самого начала и не
    // вызывалось отсюда ни разу.
    feedScrollDrag(craftScroll, listArea);

    i32 start = (i32)(craftScroll.offset / (rowH + rowGap));

    for (i32 i = 0; i < visible && (start + i) < total; ++i) {
        i32 idx = start + i;
        auto& ar = available[idx];
        const auto& r = *ar.recipe;

        Rect rr = layout_.paneRow((u32)i);
        rr.y -= (craftScroll.offset - (f32)start * (rowH + rowGap));
        // Строка, не поместившаяся целиком, не рисуется: графической
        // обрезки здесь нет.
        if (rr.y + rr.h > listArea.y + listArea.h + 0.5f) break;

        bool canNow = (ar.status == crafting::CraftStatus::Ok);
        bool isSel  = (selectedRecipeIdx == idx);

        UiColor bg = isSel
            ? rgba(120, 100, 60, 240)
            : (canNow ? rgba(40, 60, 40, 220) : rgba(40, 40, 40, 180));

        int ri = ui_.pushInteractiveRect(rr, [this, idx]() {
            selectedRecipeIdx = idx;
        });

        bool pressed = ui_.isInteractivePressed(ri);
        if (pressed) bg = rgba(150, 130, 80, 255);

        ui_.rect(rr.x, rr.y, rr.w, rr.h, bg);
        ui_.rectOutline(rr.x, rr.y, rr.w, rr.h,
                        layout_.dp(theme::STROKE_DP),
                        canNow ? rgba(120, 220, 120, 255) : COL_BLACK);

        const f32 tx = rr.x + layout_.dp(theme::SPACE_M_DP);
        f32 ty = rr.y + layout_.dp(theme::SPACE_XS_DP);
        ui_.text(items::items().name(r.output.itemId), tx, ty,
                 theme::TEXT_LABEL,
                 canNow ? COL_WHITE : rgba(160, 160, 160, 255));
        ty += ui_.textHeight(theme::TEXT_LABEL) + layout_.dp(theme::SPACE_XS_DP);

        char costLine[256] = {};
        for (usize k = 0; k < r.inputs.size(); ++k) {
            const auto& in = r.inputs[k];
            char piece[64];
            std::snprintf(piece, sizeof(piece), "%sx%u ",
                          items::items().name(in.itemId),
                          (unsigned)in.count);
            std::strncat(costLine, piece,
                         sizeof(costLine) - std::strlen(costLine) - 1);
        }
        ui_.text(costLine, tx, ty, theme::TEXT_CAPTION,
                 rgba(200, 200, 200, 255));
        ty += ui_.textHeight(theme::TEXT_CAPTION)
            + layout_.dp(theme::SPACE_XS_DP);

        if (!canNow) {
            ui_.text(crafting::statusString(ar.status), tx, ty,
                     theme::TEXT_CAPTION, rgba(220, 120, 120, 255));
        }
    }

    // Правая панель — подробности выбранного рецепта.
    {
        ui_.rect(pane.x, pane.y, pane.w, pane.h, rgba(30, 25, 20, 240));
        ui_.rectOutline(pane.x, pane.y, pane.w, pane.h,
                        layout_.dp(theme::STROKE_DP), COL_BLACK);

        const f32 px = pane.x + pad;
        f32 py = pane.y + pad;

        if (selectedRecipeIdx >= 0 && selectedRecipeIdx < total) {
            auto& ar = available[selectedRecipeIdx];
            const auto& r = *ar.recipe;

            ui_.text(items::items().name(r.output.itemId), px, py,
                     theme::TEXT_TITLE, COL_WHITE);
            py += ui_.textHeight(theme::TEXT_TITLE)
                + layout_.dp(theme::SPACE_M_DP);

            char lvlBuf[64];
            std::snprintf(lvlBuf, sizeof(lvlBuf), cfg::tr("Level %u"),
                          (unsigned)r.requiredLevel);
            ui_.text(lvlBuf, px, py, theme::TEXT_CAPTION,
                     rgba(200, 200, 200, 255));
            py += ui_.textHeight(theme::TEXT_CAPTION)
                + layout_.dp(theme::SPACE_L_DP);

            ui_.text(T(StrKey::Craft_Ingredients), px, py, theme::TEXT_LABEL,
                     rgba(255, 220, 120, 255));
            py += ui_.textHeight(theme::TEXT_LABEL)
                + layout_.dp(theme::SPACE_M_DP);

            const Rect act = layout_.primaryAction();
            for (const auto& in : r.inputs) {
                // Ниже кнопки действия материалы не пишем: она
                // стоит на своём месте у низа панели, и список,
                // доехав до неё, уходил бы под неё.
                if (py + ui_.textHeight(theme::TEXT_LABEL) > act.y) break;

                u32 have = inv->countOf(in.itemId);
                bool ok = have >= in.count;

                char line[128];
                std::snprintf(line, sizeof(line), "%s  %u / %u",
                              items::items().name(in.itemId),
                              (unsigned)have, (unsigned)in.count);
                ui_.text(line, px + layout_.dp(theme::SPACE_S_DP), py,
                         theme::TEXT_LABEL,
                         ok ? rgba(180, 240, 180, 255)
                            : rgba(240, 160, 160, 255));
                py += ui_.textHeight(theme::TEXT_LABEL)
                    + layout_.dp(theme::SPACE_S_DP);
            }

            if (ar.status == crafting::CraftStatus::Ok) {
                int ci = ui_.pushInteractiveRect(act, [this, r]() {
                    if (onCraft) onCraft(r.id);
                });
                bool cpress = ui_.isInteractivePressed(ci);
                UiColor c = cpress ? rgba(80, 200, 80, 255)
                                   : rgba(40, 120, 40, 255);
                ui_.rect(act.x, act.y, act.w, act.h, c);
                ui_.rectOutline(act.x, act.y, act.w, act.h,
                                layout_.dp(theme::STROKE_DP), COL_BLACK);
                const f32 tw = ui_.textWidth(T(StrKey::Craft_Button),
                                             theme::TEXT_BODY);
                ui_.text(T(StrKey::Craft_Button),
                         act.x + (act.w - tw) * 0.5f,
                         act.y + (act.h - ui_.textHeight(theme::TEXT_BODY)) * 0.5f,
                         theme::TEXT_BODY, COL_WHITE);
            } else {
                ui_.rect(act.x, act.y, act.w, act.h, rgba(60, 40, 40, 255));
                ui_.rectOutline(act.x, act.y, act.w, act.h,
                                layout_.dp(theme::STROKE_DP), COL_BLACK);
                const char* why = crafting::statusString(ar.status);
                const f32 tw = ui_.textWidth(why, theme::TEXT_LABEL);
                ui_.text(why, act.x + (act.w - tw) * 0.5f,
                         act.y + (act.h - ui_.textHeight(theme::TEXT_LABEL)) * 0.5f,
                         theme::TEXT_LABEL, rgba(220, 160, 160, 255));
            }
        } else {
            ui_.text(T(StrKey::Craft_Select), px, py, theme::TEXT_BODY,
                     rgba(180, 180, 180, 255));
        }
    }
}

// ============================================================
// Trade
// ============================================================
void UiSystem::drawTradeScreen(player::Player& player) {
    drawMenuBackdrop(T(StrKey::Trade_Title));

    auto* inv = player.inventory();
    auto* wal = player.wallet();
    if (!inv || !wal) return;

    drawCloseButton([this]() { screen = Screen::Hud; });

    // Вкладки «купить» и «продать».
    //
    // Стояли по (40, 74) в пикселях — то есть ровно на полосах
    // здоровья и маны, и это было видно на снимке. Теперь ряд
    // вкладок отмеряет раскладка, которая про столбец HUD знает.
    {
        const char* names[2] = { T(StrKey::Trade_Buy), T(StrKey::Trade_Sell) };
        for (u32 t = 0; t < 2; ++t) {
            const Rect r = layout_.menuTab(t, 2);
            const int id = ui_.pushInteractiveRect(
                r, [this, t]() { tradeCtx.tab = (int)t; });
            const bool pressed  = ui_.isInteractivePressed(id);
            const bool selected = ((u32)tradeCtx.tab == t);
            ui_.rect(r.x, r.y, r.w, r.h,
                     pressed ? theme::Accent
                             : (selected ? theme::PanelRaised : theme::Panel));
            ui_.rectOutline(r.x, r.y, r.w, r.h,
                            layout_.dp(selected ? theme::STROKE_SELECTED_DP
                                                : theme::STROKE_DP),
                            selected ? theme::Accent : theme::Stroke);
            const f32 tw = ui_.textWidth(names[t], theme::TEXT_BODY);
            ui_.text(names[t], r.x + (r.w - tw) * 0.5f,
                     r.y + (r.h - ui_.textHeight(theme::TEXT_BODY)) * 0.5f,
                     theme::TEXT_BODY,
                     pressed ? theme::Ink : theme::TextPrimary);
        }
    }

    // Золото здесь не печатается: оно уже стоит в столбце HUD, а тот
    // виден и из меню. Две одинаковые цифры в разных углах — не
    // забота, а лишний вопрос «почему их две».

    const Rect listArea = layout_.menuBelowTabs();
    const f32 listX = listArea.x;
    const f32 listY = listArea.y;

    if (tradeCtx.tab == 0) {
        // Ассортимент торговца. Здесь стояла строка-заглушка
        // «(BUY list from trader)»: купить было физически не за что
        // нажать, хотя и цены, и запас, и обработчик покупки уже были
        // написаны.
        auto* reg = player.registryHandle();
        auto* tinv = reg
            ? reg->get<trade::TradeInventory>((ecs::Entity)tradeCtx.traderEntity)
            : nullptr;

        factions::ReputationTier tier = factions::ReputationTier::Neutral;
        if (reg) {
            if (auto* rep = reg->get<factions::Reputation>(player.entity()))
                tier = rep->tier(factions::FactionId::Villagers);
        }

        if (!tinv || tinv->entries.empty()) {
            ui_.text(T(StrKey::Trade_NothingToBuy), listX, listY, 1.8f,
                     rgba(180, 180, 180, 255));
        } else {
            // Ассортимент ПРОКРУЧИВАЕТСЯ, а не обрывается на
            // двадцать четвёртом товаре. «Четыре ряда по шесть» было
            // числом, взятым с потолка: у торговца бывает больше, и
            // всё, что за ним, игрок не видел и купить не мог.
            const f32 cellW = 140.f, cellH = 90.f;
            const int cols = 6;
            int shown = 0;
            for (const auto& e : tinv->entries) {
                if (!e.isBuyable || e.stock == 0) continue;

                const int c = shown % cols, rrow = shown / cols;
                ++shown;

                const auto price = trade::priceFor(e.itemId, e.basePrice, tier,
                                                   e.isBuyable, e.isSellable);

                const f32 x = listX + (f32)c * cellW;
                const f32 y = listY + (f32)rrow * cellH - tradeScroll.offset;
                Rect rr{ x, y, 130.f, 80.f };

                // Карточка видна, только если помещается целиком.
                if (rr.y < listArea.y - 0.01f ||
                    rr.y + rr.h > listArea.y + listArea.h + 0.01f) continue;

                const u16 itemId = e.itemId;
                const bool affordable = wal->gold >= price.buyPrice;
                int ri = ui_.pushInteractiveRect(rr, [this, itemId, affordable]() {
                    tradeCtx.selectedIdx = (i32)itemId;
                    tradeCtx.selCount = 1;
                    if (affordable && onTradeBuy) onTradeBuy(itemId, 1);
                });
                const bool pressed = ui_.isInteractivePressed(ri);

                // Недоступное по деньгам показываем тусклым: иначе
                // нажатие просто ничего не делает, и непонятно почему.
                ui_.rect(rr.x, rr.y, rr.w, rr.h,
                         !affordable ? rgba(40, 40, 40, 200)
                                     : (pressed ? rgba(60, 80, 60, 240)
                                                : rgba(40, 50, 40, 220)));
                ui_.rectOutline(rr.x, rr.y, rr.w, rr.h, 2.f, COL_BLACK);

                char lbl[64];
                std::snprintf(lbl, sizeof(lbl), "%s x%u",
                              items::items().name(e.itemId),
                              (unsigned)e.stock);
                ui_.text(lbl, rr.x + 6.f, rr.y + 8.f, 1.4f,
                         affordable ? COL_WHITE : rgba(130, 130, 130, 255));

                char priceBuf[32];
                std::snprintf(priceBuf, sizeof(priceBuf), "%u g",
                              (unsigned)price.buyPrice);
                ui_.text(priceBuf, rr.x + 6.f, rr.y + 30.f, 1.3f,
                         affordable ? rgba(255, 220, 100, 255)
                                    : rgba(140, 120, 70, 255));
            }
            if (shown == 0) {
                ui_.text(T(StrKey::Trade_NothingToBuy), listX, listY, 1.8f,
                         rgba(180, 180, 180, 255));
            }

            // Сколько рядов вышло — столько и прокручивается.
            const int rows = (shown + cols - 1) / cols;
            tradeScroll.setContentHeight((f32)rows * cellH, listArea.h);
            feedScrollDrag(tradeScroll, listArea);
        }
    } else {
        // SELL — инвентарь
        for (int r = 0; r < 4; ++r) {
            for (int c = 0; c < 6; ++c) {
                u32 slotIdx = items::INV_MAIN_OFFSET + (u32)(r * 6 + c);
                auto& s = inv->at(slotIdx);
                if (s.empty()) continue;

                const auto& def = items::items().get(s.itemId);
                const f32 x = listX + c * 140.f;
                const f32 y = listY + r * 90.f;

                Rect rr{ x, y, 130.f, 80.f };
                int ri = ui_.pushInteractiveRect(rr, [this, s]() {
                    tradeCtx.selectedIdx = (i32)s.itemId;
                    tradeCtx.selCount = 1;
                    if (onTradeSell) onTradeSell(s.itemId, 1);
                });
                bool pressed = ui_.isInteractivePressed(ri);

                ui_.rect(rr.x, rr.y, rr.w, rr.h,
                         pressed ? rgba(80, 60, 60, 240)
                                 : rgba(50, 40, 40, 220));
                ui_.rectOutline(rr.x, rr.y, rr.w, rr.h, 2.f, COL_BLACK);

                char lbl[64];
                std::snprintf(lbl, sizeof(lbl), "%s x%u",
                              items::items().name(s.itemId),
                              (unsigned)s.count);
                ui_.text(lbl, rr.x + 6.f, rr.y + 8.f, 1.4f, COL_WHITE);

                u32 sellPrice = def.value / 2;
                if (sellPrice < 1) sellPrice = 1;
                char priceBuf[32];
                std::snprintf(priceBuf, sizeof(priceBuf), "%u g",
                              (unsigned)sellPrice);
                ui_.text(priceBuf, rr.x + 6.f, rr.y + 30.f, 1.3f,
                         rgba(255, 220, 100, 255));
            }
        }
    }
}

// ============================================================
// Enchant Altar
// ============================================================
void UiSystem::drawEnchantScreen(player::Player& player) {
    drawMenuBackdrop(cfg::tr("ENCHANT ALTAR"));

    drawCloseButton([this]() { screen = Screen::Hud; });

    auto* inv = player.inventory();
    auto* wal = player.wallet();
    if (!inv) return;

    const Rect body     = layout_.menuBelowTabs();
    const Rect listArea = layout_.paneLeftIn(body);
    const Rect pane     = layout_.paneRightIn(body);
    const f32 rowH   = layout_.dp(theme::TOUCH_REGULAR_DP);
    const f32 rowGap = layout_.dp(theme::SPACE_S_DP);
    const f32 pad    = layout_.dp(theme::PANEL_PAD_DP);

    // Что в руках и сколько золота — одной строкой над панелями, а
    // не по постоянным (40, 110) и (screenW - 40, 24): там они
    // ложились на полосы ресурсов и на кнопку закрытия. Одной, а не
    // двумя половинами: правая половина приходится ровно на столбец
    // навигации.
    {
        const Rect r = layout_.menuTab(0, 1);
        const f32 ty = r.y + (r.h - ui_.textHeight(theme::TEXT_LABEL)) * 0.5f;
        const auto& wdef = combat::weapons().get(player.equipped.weaponId);

        char line[192];
        int at = std::snprintf(line, sizeof(line), "%s %s",
                               cfg::tr("Equipped:"),
                               wdef.name ? cfg::tr(wdef.name) : "-");
        if (player.equipped.enchant.id != combat::EnchantmentId::None) {
            char ench[64];
            std::snprintf(ench, sizeof(ench), cfg::tr("Enchant: %s Lv%u"),
                          combat::enchantmentName(player.equipped.enchant.id),
                          player.equipped.enchant.level);
            at += std::snprintf(line + at, sizeof(line) - (usize)at,
                                "   %s", ench);
        }
        if (wal) {
            char gbuf[32];
            wal->format(gbuf, sizeof(gbuf));
            std::snprintf(line + at, sizeof(line) - (usize)at, "   %s", gbuf);
        }
        ui_.text(line, r.x, ty, theme::TEXT_LABEL, COL_WHITE);
    }

    const auto& recipes = world::enchantRecipes().all();
    const i32 total = (i32)recipes.size();
    const i32 visible = (i32)layout_.paneRowsVisible();

    enchantScroll.setContentHeight((f32)total * (rowH + rowGap),
                                   (f32)visible * (rowH + rowGap));
    feedScrollDrag(enchantScroll, listArea);

    const i32 start = (i32)(enchantScroll.offset / (rowH + rowGap));

    for (i32 i = 0; i < visible && (start + i) < total; ++i) {
        const i32 idxR = start + i;
        const auto& r = recipes[(usize)idxR];

        Rect rr = layout_.paneRow((u32)i);
        rr.y -= (enchantScroll.offset - (f32)start * (rowH + rowGap));
        if (rr.y + rr.h > listArea.y + listArea.h + 0.5f) break;

        bool isSel = (enchantCtx.selectedIdx == idxR);
        UiColor bg = isSel ? rgba(120, 100, 60, 240) : rgba(50, 40, 60, 220);

        int idx = ui_.pushInteractiveRect(rr, [this, idxR]() {
            enchantCtx.selectedIdx = idxR;
        });
        if (ui_.isInteractivePressed(idx)) bg = rgba(140, 120, 80, 255);

        ui_.rect(rr.x, rr.y, rr.w, rr.h, bg);
        ui_.rectOutline(rr.x, rr.y, rr.w, rr.h,
                        layout_.dp(theme::STROKE_DP), COL_BLACK);

        const f32 tx = rr.x + layout_.dp(theme::SPACE_M_DP);
        f32 ty = rr.y + layout_.dp(theme::SPACE_XS_DP);

        ui_.text(r.name ? cfg::tr(r.name) : "?", tx, ty, theme::TEXT_LABEL,
                 COL_WHITE);
        ty += ui_.textHeight(theme::TEXT_LABEL) + layout_.dp(theme::SPACE_XS_DP);

        char cost[64];
        std::snprintf(cost, sizeof(cost), cfg::tr("%u gold"), r.goldCost);
        ui_.text(cost, tx, ty, theme::TEXT_CAPTION, rgba(255, 220, 100, 255));
        ty += ui_.textHeight(theme::TEXT_CAPTION)
            + layout_.dp(theme::SPACE_XS_DP);

        char mats[192] = {};
        for (usize k = 0; k < r.materials.size(); ++k) {
            const auto& m = r.materials[k];
            char piece[64];
            std::snprintf(piece, sizeof(piece), "%sx%u ",
                          items::items().name(m.itemId), (unsigned)m.count);
            std::strncat(mats, piece, sizeof(mats) - std::strlen(mats) - 1);
        }
        ui_.text(mats, tx, ty, theme::TEXT_CAPTION, rgba(200, 200, 200, 255));
    }

    // Правая панель — что выбрано и кнопка «зачаровать».
    {
        ui_.rect(pane.x, pane.y, pane.w, pane.h, rgba(30, 20, 40, 240));
        ui_.rectOutline(pane.x, pane.y, pane.w, pane.h,
                        layout_.dp(theme::STROKE_DP), COL_BLACK);

        const f32 px = pane.x + pad;
        const f32 py = pane.y + pad;

        if (enchantCtx.selectedIdx >= 0 &&
            enchantCtx.selectedIdx < total)
        {
            const auto& r = recipes[(usize)enchantCtx.selectedIdx];
            ui_.text(r.name ? cfg::tr(r.name) : "?", px, py,
                     theme::TEXT_TITLE, COL_WHITE);

            // Зачарование необратимо: оно тратит предмет и золото и
            // насовсем меняет оружие. Такое спрашивает.
            const Rect cr = layout_.primaryActionIn(pane);
            const char* label = r.name ? cfg::tr(r.name) : cfg::tr("ENCHANT");
            const int ci = ui_.pushInteractiveRect(cr,
                [this, i = enchantCtx.selectedIdx, label]() {
                    askConfirm(label, label, [this, i]() {
                        if (onEnchant) onEnchant((u32)i);
                    });
                });
            const bool cpress = ui_.isInteractivePressed(ci);

            ui_.rect(cr.x, cr.y, cr.w, cr.h,
                     cpress ? theme::Accent : theme::PanelRaised);
            ui_.rectOutline(cr.x, cr.y, cr.w, cr.h,
                            layout_.dp(theme::STROKE_SELECTED_DP),
                            cpress ? theme::AccentPressed : theme::Accent);
            const f32 tw = ui_.textWidth(cfg::tr("ENCHANT"), theme::TEXT_BODY);
            ui_.text(cfg::tr("ENCHANT"), cr.x + (cr.w - tw) * 0.5f,
                     cr.y + (cr.h - ui_.textHeight(theme::TEXT_BODY)) * 0.5f,
                     theme::TEXT_BODY,
                     cpress ? theme::Ink : theme::TextPrimary);
        } else {
            ui_.text(cfg::tr("Select a recipe"), px, py, theme::TEXT_BODY,
                     rgba(180, 180, 180, 255));
        }
    }
}

// ============================================================
// Toast
// ============================================================
// Кнопка закрытия одного вида на всю игру.
//
// Была написана семью одинаковыми копиями
// `Rect close{ screenW - 90, 74, 80, 50 }` — 20 dp по высоте при
// норме 48. Совпадение, а не правило: любая из семи могла разъехаться
// с остальными, и заметить это было бы негде.
void UiSystem::drawCloseButton(std::function<void()> onClose) {
    const Rect r = layout_.closeButton();
    // Открытие экрана щёлкало, закрытие — нет: uiBack() был написан и
    // ни разу не позван. Здесь единственная кнопка закрытия на все
    // экраны, поэтому звук ставится один раз и на все сразу.
    const int idx = ui_.pushInteractiveRect(
        r, [close = std::move(onClose)]() {
            audio::events().uiBack();
            if (close) close();
        });
    const bool pressed = ui_.isInteractivePressed(idx);

    ui_.rect(r.x, r.y, r.w, r.h, pressed ? theme::Danger : theme::PanelRaised);
    ui_.rectOutline(r.x, r.y, r.w, r.h, layout_.dp(theme::STROKE_DP),
                    pressed ? theme::Danger : theme::Stroke);

    const f32 tw = ui_.textWidth("X", theme::TEXT_BODY);
    ui_.text("X", r.x + (r.w - tw) * 0.5f,
             r.y + (r.h - ui_.textHeight(theme::TEXT_BODY)) * 0.5f,
             theme::TEXT_BODY,
             pressed ? theme::TextPrimary : theme::TextSecondary);
}

// ============================================================
// Модальное подтверждение
// ============================================================
//
// Выход из игры срабатывал сразу: несохранённый прогресс терялся без
// вопроса. То же относится к удалению и перезаписи сохранения.
//
// Модальность здесь не украшение. Попадание ищется среди
// прямоугольников с конца, то есть выигрывает нарисованный позже; но
// касание МИМО окна нашло бы кнопку под ним. Поэтому первым кладём
// прямоугольник во весь экран — он глушит всё, что снаружи.
void UiSystem::drawConfirm() {
    if (!confirm.active) return;

    ui_.rect(0, 0, (f32)screenW_, (f32)screenH_,
             withAlpha(theme::Ink, theme::ALPHA_SCRIM));
    ui_.pushInteractiveRect({ 0.f, 0.f, (f32)screenW_, (f32)screenH_ },
                            [](){});   // глушитель, намеренно пустой

    const Rect p = layout_.confirmPanel();
    ui_.rect(p.x, p.y, p.w, p.h, theme::Panel);
    ui_.rectOutline(p.x, p.y, p.w, p.h,
                    layout_.dp(theme::STROKE_SELECTED_DP), theme::Danger);

    if (confirm.question) {
        const f32 tw = ui_.textWidth(confirm.question, theme::TEXT_TITLE);
        ui_.text(confirm.question, p.x + (p.w - tw) * 0.5f,
                 p.y + layout_.dp(theme::PANEL_PAD_DP),
                 theme::TEXT_TITLE, theme::TextPrimary);
    }

    struct Btn { const char* label; bool danger; };
    const Btn btns[2] = {
        { T(StrKey::Cancel), false },
        { confirm.yesLabel ? confirm.yesLabel : T(StrKey::Ok), true },
    };

    for (u32 i = 0; i < 2; ++i) {
        const Rect r = layout_.confirmButton(i);
        const bool isYes = (i == 1);
        const int idx = ui_.pushInteractiveRect(r, [this, isYes]() {
            auto act = confirm.onYes;
            confirm = Confirm{};
            if (isYes && act) act();
        });
        const bool pressed = ui_.isInteractivePressed(idx);

        const UiColor accent = btns[i].danger ? theme::Danger : theme::Stroke;
        ui_.rect(r.x, r.y, r.w, r.h,
                 pressed ? accent : theme::PanelRaised);
        ui_.rectOutline(r.x, r.y, r.w, r.h,
                        layout_.dp(btns[i].danger ? theme::STROKE_SELECTED_DP
                                                  : theme::STROKE_DP),
                        accent);
        const f32 tw = ui_.textWidth(btns[i].label, theme::TEXT_BODY);
        ui_.text(btns[i].label, r.x + (r.w - tw) * 0.5f,
                 r.y + (r.h - ui_.textHeight(theme::TEXT_BODY)) * 0.5f,
                 theme::TEXT_BODY,
                 pressed ? theme::TextPrimary
                         : (btns[i].danger ? theme::Danger : theme::TextPrimary));
    }
}

void UiSystem::drawStatusToast() {
    if (notices_.empty()) return;

    const u32 shown = (u32)std::min<usize>(notices_.size(),
                                           theme::NOTIFY_MAX_VISIBLE);
    for (u32 i = 0; i < shown; ++i) {
        const Notice& n = notices_[i];
        const bool high = (n.priority == theme::NotifyPriority::High);

        // Важное — по центру и крупнее, рядовое — снизу. Отличаются
        // не только цветом: место, кегль и длительность.
        const f32 scale = high ? theme::TEXT_DISPLAY
                               : (n.priority == theme::NotifyPriority::Low
                                      ? theme::TEXT_CAPTION : theme::TEXT_BODY);
        const Rect r = layout_.notice(i, high, ui_.textWidth(n.text, scale));

        // Вход и выход — сдвигом и затуханием, в пределах потолка.
        f32 k = 1.f;
        if (n.age < theme::ANIM_TOAST_IN_S)
            k = n.age / theme::ANIM_TOAST_IN_S;
        else if (n.timeLeft < theme::ANIM_TOAST_OUT_S)
            k = n.timeLeft / theme::ANIM_TOAST_OUT_S;
        if (k < 0.f) k = 0.f;
        if (k > 1.f) k = 1.f;

        const f32 slide = (1.f - k) * layout_.dp(12.f);
        const u8  a     = (u8)(255.f * k);

        ui_.rect(r.x, r.y + slide, r.w, r.h,
                 withAlpha(theme::Panel, (u8)(a * 0.94f)));
        ui_.rectOutline(r.x, r.y + slide, r.w, r.h,
                        layout_.dp(high ? theme::STROKE_SELECTED_DP
                                        : theme::STROKE_DP),
                        withAlpha(high ? theme::Accent : theme::Stroke, a));

        const f32 tw = ui_.textWidth(n.text, scale);
        ui_.text(n.text, r.x + (r.w - tw) * 0.5f,
                 r.y + slide + (r.h - ui_.textHeight(scale)) * 0.5f,
                 scale, withAlpha(high ? theme::Accent : theme::TextPrimary, a));
    }
}

void UiSystem::destroy() {
    renderer_.destroy();
}

} // namespace ui
