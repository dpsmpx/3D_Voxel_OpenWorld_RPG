/**
 * @file ui_system.cpp
 * @brief Интерфейс: immediate-mode UI поверх Vulkan, HUD, меню, миникарта.
 */
#include "ui_system.h"
#include "slider.h"
#include "hud_resources.h"
#include "../combat/weapon.h"
#include "../combat/resonance.h"
#include "../combat/enchantment.h"
#include "../combat/components.h"
#include "../progression/progression.h"
#include "../progression/skill_tree.h"
#include "../quests/quest.h"
#include "../quests/quest_def.h"
#include "../npc/dialogue.h"
#include "../factions/faction.h"
#include "../items/item_def.h"
#include "../items/item_use.h"
#include "../trade/trade.h"
#include "../world/enchant_altar.h"
#include "../config/settings.h"
#include "../config/localization.h"
#include "../ecs/components.h"
#include "../core/log.h"
#include <cstdio>
#include <cstring>
#include <ctime>
#include <algorithm>
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
    ui_.setScreen(w, h);
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

void UiSystem::setStatus(const std::string& msg) {
    statusMessage = msg;
    statusTimer = 3.0f;
}

void UiSystem::tickUi(f32 dt) {
    if (statusTimer > 0.f) {
        statusTimer -= dt;
        if (statusTimer <= 0.f) statusMessage.clear();
    }
    craftScroll.tick(dt);
    questScroll.tick(dt);
    tradeScroll.tick(dt);

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
    ui_.beginFrame();

    // Обновляем кэш ресурсов
    {
        HudResources hr = readHudResources(*player.registryHandle(), player.entity());
        cachedHpPct = hr.hpPct();
        cachedMpPct = hr.mpPct();
        cachedSpPct = hr.spPct();
    }

    switch (screen) {
        case Screen::Hud:
            drawHud(ctx, player, world, fps);
            drawTouchControls();
            if (loading()) drawLoadingOverlay();
            break;
        case Screen::PauseMenu:
            drawHud(ctx, player, world, fps);
            drawPauseMenu(player);
            break;
        case Screen::Inventory:
            drawHud(ctx, player, world, fps);
            drawInventory(player);
            break;
        case Screen::Settings:
            drawSettingsScreen(player);
            break;
        case Screen::SkillTree:
            drawHud(ctx, player, world, fps);
            drawSkillTreeScreen(player);
            break;
        case Screen::Attributes:
            drawHud(ctx, player, world, fps);
            drawAttributesScreen(player);
            break;
        case Screen::Dialogue:
            drawHud(ctx, player, world, fps);
            drawDialogueScreen(player);
            break;
        case Screen::QuestLog:
            drawHud(ctx, player, world, fps);
            drawQuestLogScreen(player);
            break;
        case Screen::Reputation:
            drawHud(ctx, player, world, fps);
            drawReputationScreen(player);
            break;
        case Screen::SaveLoad:
            drawHud(ctx, player, world, fps);
            drawSaveLoadScreen(player);
            break;
        case Screen::Crafting:
            drawHud(ctx, player, world, fps);
            drawCraftingScreen(player);
            break;
        case Screen::Trade:
            drawHud(ctx, player, world, fps);
            drawTradeScreen(player);
            break;
        case Screen::Enchant:
            drawHud(ctx, player, world, fps);
            drawEnchantScreen(player);
            break;
    }

    drawDragOverlay();
    drawStatusToast();

    ui_.endFrame();
    renderer_.flush(ctx);
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
// Экран загрузки: затемнение и полоса прогресса поверх HUD.
// Показывается, пока мир вокруг игрока не догенерировался.
// ============================================================
void UiSystem::drawLoadingOverlay() {
    const float w = (float)screenW_;
    const float h = (float)screenH_;

    // Затемняем сцену, чтобы недостроенный мир не отвлекал.
    ui_.rect(0.f, 0.f, w, h, rgba(8, 12, 18, 190));

    const float barW = w * 0.44f;
    const float barH = 14.f;
    const float bx = (w - barW) * 0.5f;
    const float by = h * 0.62f;

    const char* label = loadLabel ? loadLabel : T(StrKey::Notif_Loading);
    ui_.text(label, bx, by - 30.f, 1.2f, COL_WHITE);

    ui_.rect(bx - 2.f, by - 2.f, barW + 4.f, barH + 4.f, rgba(40, 48, 58, 255));
    ui_.rect(bx, by, barW, barH, rgba(20, 24, 30, 255));

    const float p = loadProgress < 0.f ? 0.f : (loadProgress > 1.f ? 1.f : loadProgress);
    if (p > 0.f)
        ui_.rect(bx, by, barW * p, barH, rgba(110, 190, 130, 255));

    char pct[8];
    std::snprintf(pct, sizeof(pct), "%d%%", (int)(p * 100.f + 0.5f));
    ui_.text(pct, bx + barW + 14.f, by - 2.f, 1.0f, rgba(200, 210, 220, 255));
}

void UiSystem::drawHud(vk::Context& /*ctx*/,
                       player::Player& player,
                       world::ChunkManager& /*world*/,
                       f32 fps)
{
    const auto& st = player.controller.state();

    auto drawMenuButton = [&](Rect r, const char* label,
                              std::function<void()> onClick)
    {
        int idx = ui_.pushInteractiveRect(r, std::move(onClick));
        bool pressed = ui_.isInteractivePressed(idx);
        UiColor fill = pressed ? rgba(180,180,180,255) : rgba(0,0,0,180);
        ui_.rect(r.x, r.y, r.w, r.h, fill);
        ui_.rectOutline(r.x, r.y, r.w, r.h, 2.f, COL_BLACK);
        float tw = ui_.textWidth(label, 2.f);
        ui_.text(label, r.x + (r.w - tw) * 0.5f, r.y + 16.f, 2.f, COL_WHITE);
    };

    drawMenuButton({ (float)screenW_ - 80.f,  12.f, 60.f, 50.f }, "|||",
                   [this]() { screen = Screen::PauseMenu; });
    drawMenuButton({ (float)screenW_ - 80.f,  70.f, 60.f, 50.f }, "INV",
                   [this]() { screen = Screen::Inventory; });
    drawMenuButton({ (float)screenW_ - 80.f, 128.f, 60.f, 50.f }, "SKL",
                   [this]() { screen = Screen::SkillTree; });
    drawMenuButton({ (float)screenW_ - 80.f, 186.f, 60.f, 50.f }, "ATT",
                   [this]() { screen = Screen::Attributes; });
    drawMenuButton({ (float)screenW_ - 80.f, 244.f, 60.f, 50.f }, "QST",
                   [this]() { screen = Screen::QuestLog; });
    drawMenuButton({ (float)screenW_ - 80.f, 302.f, 60.f, 50.f }, "REP",
                   [this]() { screen = Screen::Reputation; });
    drawMenuButton({ (float)screenW_ - 80.f, 360.f, 60.f, 50.f }, "SAV",
                   [this]() {
                       saveLoadMode = SaveLoadMode::Save;
                       screen = Screen::SaveLoad;
                   });

    drawXpBar(player);
    drawHudResources(player);
    drawResonanceBar(player);
    drawStatusIcons(player);
    drawHotbar(player);
    drawMinimap(player);

    if (showFps) {
        char buf[64];
        std::snprintf(buf, sizeof(buf), "FPS %d", (int)fps);
        ui_.text(buf, 12.f, 180.f, 2.f, COL_YELLOW);
    }

    if (player.cameraMode == player::CameraMode::FirstPerson) {
        const float cx = screenW_ * 0.5f;
        const float cy = screenH_ * 0.5f;
        ui_.rect(cx - 10.f, cy - 1.f, 20.f, 2.f, COL_WHITE);
        ui_.rect(cx - 1.f, cy - 10.f, 2.f, 20.f, COL_WHITE);
    }

    drawLevelUpNotification(player);
    drawReputationNotification(player);

    if (cfg::settingsConst().showDebugPos) {
        char dbg[96];
        std::snprintf(dbg, sizeof(dbg), "POS %.1f %.1f %.1f",
                      st.position.x, st.position.y, st.position.z);
        ui_.text(dbg, 12.f, 204.f, 1.5f, COL_WHITE);
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

void UiSystem::drawXpBar(player::Player& player) {
    auto* prog = player.progression();
    if (!prog) return;

    const float barH = 12.f;
    const float y = 12.f;

    ui_.rect(0, y - 2, (float)screenW_, barH + 4, COL_BLACK);
    ui_.rect(0, y, (float)screenW_, barH, rgba(30,20,50,255));
    ui_.rect(0, y, (float)screenW_ * prog->levelProgress(), barH,
             rgba(180, 100, 240, 255));

    char buf[64];
    std::snprintf(buf, sizeof(buf), "LV %u", prog->level);
    ui_.text(buf, 8.f, y - 1.f, 1.6f, COL_WHITE);

    char xpText[64];
    std::snprintf(xpText, sizeof(xpText), "%llu / %llu",
                  (unsigned long long)prog->xpWithinLevel(),
                  (unsigned long long)prog->xpForNextLevel());
    float tw = ui_.textWidth(xpText, 1.5f);
    ui_.text(xpText, (float)screenW_ - tw - 8.f, y - 1.f, 1.5f, COL_WHITE);
}

void UiSystem::drawHudResources(player::Player& player) {
    const float bx = 24.f;
    float by = (float)screenH_ - 260.f;
    const float bw = 260.f;
    const float bh = 22.f;

    // HP
    ui_.rect(bx - 2, by - 2, bw + 4, bh + 4, COL_BLACK);
    ui_.rect(bx, by, bw, bh, rgba(60,20,20,255));
    ui_.rect(bx, by, bw * cachedHpPct, bh,
             cachedHpPct < 0.3f ? rgba(255, 60, 60, 255) : COL_RED);
    ui_.text(T(StrKey::Hud_Health), bx + 6, by + 4, 2.f, COL_WHITE);

    // MP
    by += bh + 6.f;
    ui_.rect(bx - 2, by - 2, bw + 4, bh + 4, COL_BLACK);
    ui_.rect(bx, by, bw, bh, rgba(20,30,60,255));
    ui_.rect(bx, by, bw * cachedMpPct, bh, COL_BLUE);
    ui_.text(T(StrKey::Hud_Mana), bx + 6, by + 4, 2.f, COL_WHITE);

    // SP
    by += bh + 6.f;
    ui_.rect(bx - 2, by - 2, bw + 4, bh + 4, COL_BLACK);
    ui_.rect(bx, by, bw, bh, rgba(20,60,20,255));
    ui_.rect(bx, by, bw * cachedSpPct, bh, COL_GREEN);
    ui_.text(T(StrKey::Hud_Stamina), bx + 6, by + 4, 2.f, COL_WHITE);

    // Золото
    if (auto* wal = player.wallet()) {
        char goldBuf[32];
        wal->format(goldBuf, sizeof(goldBuf));
        ui_.text(goldBuf, bx, by + bh + 6.f, 2.f, rgba(255, 220, 100, 255));
    }
}

void UiSystem::drawMinimap(player::Player& player) {
    if (minimap.size() == 0) return;

    const float mSize = 180.f;
    const float mx = (float)screenW_ - mSize - 20.f;
    const float my = (float)screenH_ - mSize - 20.f;

    // Рамка
    ui_.rect(mx - 4.f, my - 4.f, mSize + 8.f, mSize + 8.f, COL_BLACK);
    ui_.rectOutline(mx - 4.f, my - 4.f, mSize + 8.f, mSize + 8.f,
                    2.f, rgba(180, 160, 100, 255));

    // Фон — заглушка (реальная миникарта рендерится из текстуры,
    // которая сейчас не подключена напрямую к UiRenderer без
    // рефакторинга Texture2D::upload. Показываем упрощённую версию:
    // цвет-плашка и маркер игрока.
    ui_.rect(mx, my, mSize, mSize, rgba(30, 40, 30, 220));

    // Маркер игрока (центр)
    float pxc = mx + mSize * 0.5f;
    float pyc = my + mSize * 0.5f;
    ui_.rect(pxc - 4.f, pyc - 4.f, 8.f, 8.f, rgba(255, 240, 100, 255));
    ui_.rectOutline(pxc - 4.f, pyc - 4.f, 8.f, 8.f, 1.f, COL_BLACK);

    // Стрелка направления
    glm::vec3 fwd = player.aimDir();
    float fLen = 12.f;
    ui_.rect(pxc + fwd.x * fLen - 2.f,
             pyc + fwd.z * fLen - 2.f,
             4.f, 4.f, rgba(255, 100, 100, 255));

    // Компас (N наверху)
    ui_.text("N", pxc - 4.f, my + 4.f, 1.6f, COL_WHITE);

    (void)player;
}

void UiSystem::drawResonanceBar(player::Player& player) {
    const auto& res = player.resonance;
    const float bw = 260.f;
    const float bh = 14.f;
    const float bx = 24.f;
    const float by = (float)screenH_ - 176.f;

    ui_.rect(bx - 2, by - 2, bw + 4, bh + 4, COL_BLACK);
    ui_.rect(bx, by, bw, bh, rgba(40,20,60,255));

    UiColor fillColor = rgba(140, 90, 220, 255);
    if (res.stack >= combat::RESONANCE_MAX_STACKS)
        fillColor = rgba(255, 200, 60, 255);
    else if (res.stack >= 3)
        fillColor = rgba(200, 100, 240, 255);

    ui_.rect(bx, by, bw * res.fill(), bh, fillColor);

    for (i32 i = 1; i < combat::RESONANCE_MAX_STACKS; ++i) {
        float px = bx + bw * ((float)i / (float)combat::RESONANCE_MAX_STACKS);
        ui_.rect(px - 1.f, by, 2.f, bh, COL_BLACK);
    }
    ui_.text(T(StrKey::Hud_Resonance), bx + 4.f, by + 1.f, 1.5f, COL_WHITE);

    if (res.finisherReady) {
        const char* txt = T(StrKey::Hud_FinisherReady);
        float tw = ui_.textWidth(txt, 2.f);
        ui_.text(txt, bx + bw - tw - 4.f, by - 22.f, 2.f,
                 rgba(255, 220, 60, 255));
    }
}

void UiSystem::drawStatusIcons(player::Player& player) {
    const auto& se = player.statuses;
    float x = (float)screenW_ - 300.f;
    float y = 440.f;
    const float step = 46.f;

    auto icon = [&](UiColor c, const char* label) {
        ui_.rect(x, y, 40.f, 40.f, c);
        ui_.rectOutline(x, y, 40.f, 40.f, 2.f, COL_BLACK);
        ui_.text(label, x + 4.f, y + 14.f, 1.5f, COL_WHITE);
        x -= step;
    };

    if (se.burnTime > 0.f)   icon(rgba(220, 100, 40, 220), "FIRE");
    if (se.slowTime > 0.f)   icon(rgba(80, 160, 240, 220), "FROST");
    if (se.stunTime > 0.f)   icon(rgba(240, 220, 80, 220), "STUN");
    if (se.poisonTime > 0.f) icon(rgba(120, 220, 80, 220), "POIS");
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
    const int slots = 9;
    const float sw = 60.f, sh = 60.f;
    const float totalW = slots * (sw + 4.f) - 4.f;
    const float x0 = ((float)screenW_ - totalW) * 0.5f;
    const float y0 = (float)screenH_ - 84.f;

    auto* inv = player.inventory();
    u8 activeHotbar = inv ? inv->activeHotbar : 0;

    for (int i = 0; i < slots; ++i) {
        float x = x0 + i * (sw + 4.f);
        bool isActive = (i == (int)activeHotbar);

        UiColor bg = isActive ? rgba(120,120,80,255) : rgba(40,40,40,220);
        ui_.rect(x, y0, sw, sh, bg);
        ui_.rectOutline(x, y0, sw, sh, 2.f,
                        isActive ? rgba(255, 220, 100, 255) : COL_BLACK);

        if (inv) {
            auto& s = inv->at(items::INV_HOTBAR_OFFSET + i);
            if (!s.empty()) {
                drawItemIcon(s, x + 6.f, y0 + 6.f, sw - 12.f, false);
            }
        }

        char num[4];
        std::snprintf(num, sizeof(num), "%d", i + 1);
        ui_.text(num, x + 4.f, y0 + 4.f, 1.3f, rgba(200, 200, 200, 200));
    }

    const auto& wdef = combat::weapons().get(player.equipped.weaponId);
    if (wdef.name) {
        float tw = ui_.textWidth(wdef.name, 2.f);
        ui_.text(wdef.name,
                 x0 + totalW * 0.5f - tw * 0.5f,
                 y0 - 26.f, 2.f, COL_WHITE);

        if (player.equipped.enchant.id != combat::EnchantmentId::None) {
            const char* en = combat::enchantmentName(player.equipped.enchant.id);
            float ew = ui_.textWidth(en, 1.5f);
            ui_.text(en,
                     x0 + totalW * 0.5f - ew * 0.5f,
                     y0 - 46.f, 1.5f, rgba(180, 120, 240, 255));
        }
    }
}

// ============================================================
// Pause Menu
// ============================================================
void UiSystem::drawPauseMenu(player::Player& player) {
    ui_.rect(0, 0, (float)screenW_, (float)screenH_, rgba(0,0,0,180));

    const float cw = 380.f, ch = 56.f;
    const float cx = ((float)screenW_ - cw) * 0.5f;
    const float cy = (float)screenH_ * 0.5f - 280.f;

    ui_.text(T(StrKey::Menu_Pause), cx + 100.f, cy - 60.f, 3.f, COL_WHITE);

    auto menuBtn = [&](const char* label, float y, UiColor bg,
                       std::function<void()> onClick)
    {
        Rect r{ cx, y, cw, ch };
        ui_.pushInteractiveRect(r, std::move(onClick));
        int idx = (int)ui_.screenWidth(); // заглушка, используем button
        (void)idx;

        // Используем button для отрисовки + обработки
        // Нам нужен idx, но push уже сделал работу. Поэтому
        // перепишем: сначала запомним idx.
        // (Реализация ниже)
    };

    // Прямые кнопки
    {
        Rect r{ cx, cy, cw, ch };
        int idx = ui_.pushInteractiveRect(r, [this]() { screen = Screen::Hud; });
        if (ui_.button(T(StrKey::Menu_Resume), r, idx,
                       rgba(80,120,80,255), COL_WHITE)) {
            screen = Screen::Hud;
        }
    }
    {
        Rect r{ cx, cy + 70.f, cw, ch };
        int idx = ui_.pushInteractiveRect(r, [this]() { screen = Screen::Settings; });
        if (ui_.button(T(StrKey::Menu_Settings), r, idx,
                       rgba(80,80,140,255), COL_WHITE)) {
            screen = Screen::Settings;
        }
    }
    {
        Rect r{ cx, cy + 140.f, cw, ch };
        int idx = ui_.pushInteractiveRect(r, [this]() { screen = Screen::SkillTree; });
        auto* tree = player.skillTree();
        i32 pts = tree ? tree->unspentPoints : 0;
        char label[64];
        if (pts > 0) std::snprintf(label, sizeof(label), "%s (%d)",
                                   T(StrKey::Menu_Skills), pts);
        else std::snprintf(label, sizeof(label), "%s", T(StrKey::Menu_Skills));
        if (ui_.button(label, r, idx, rgba(120,80,180,255), COL_WHITE)) {
            screen = Screen::SkillTree;
        }
    }
    {
        Rect r{ cx, cy + 210.f, cw, ch };
        int idx = ui_.pushInteractiveRect(r, [this]() { screen = Screen::Attributes; });
        auto* prog = player.progression();
        i32 pts = prog ? prog->availableAttrPoints : 0;
        char label[64];
        if (pts > 0) std::snprintf(label, sizeof(label), "%s (%d)",
                                   T(StrKey::Menu_Attributes), pts);
        else std::snprintf(label, sizeof(label), "%s", T(StrKey::Menu_Attributes));
        if (ui_.button(label, r, idx, rgba(180,120,60,255), COL_WHITE)) {
            screen = Screen::Attributes;
        }
    }
    {
        Rect r{ cx, cy + 280.f, cw, ch };
        int idx = ui_.pushInteractiveRect(r, [this]() { screen = Screen::QuestLog; });
        if (ui_.button(T(StrKey::Menu_Quests), r, idx,
                       rgba(200,180,60,255), COL_WHITE)) {
            screen = Screen::QuestLog;
        }
    }
    {
        Rect r{ cx, cy + 350.f, cw, ch };
        int idx = ui_.pushInteractiveRect(r, [this]() { screen = Screen::Reputation; });
        if (ui_.button(T(StrKey::Menu_Reputation), r, idx,
                       rgba(60,140,180,255), COL_WHITE)) {
            screen = Screen::Reputation;
        }
    }
    {
        Rect r{ cx, cy + 420.f, cw, ch };
        int idx = ui_.pushInteractiveRect(r, [this]() {
            saveLoadMode = SaveLoadMode::Save;
            screen = Screen::SaveLoad;
        });
        if (ui_.button(T(StrKey::Menu_SaveLoad), r, idx,
                       rgba(120,120,60,255), COL_WHITE)) {
            saveLoadMode = SaveLoadMode::Save;
            screen = Screen::SaveLoad;
        }
    }
    {
        Rect r{ cx, cy + 490.f, cw, ch };
        int idx = ui_.pushInteractiveRect(r, [this]() { if (onQuit) onQuit(); });
        if (ui_.button(T(StrKey::Menu_Quit), r, idx,
                       rgba(120,60,60,255), COL_WHITE)) {
            if (onQuit) onQuit();
        }
    }

    (void)menuBtn;
}

// ============================================================
// Inventory
// ============================================================
void UiSystem::drawInventory(player::Player& player) {
    ui_.rect(0, 0, (float)screenW_, (float)screenH_, rgba(0,0,0,200));

    auto* inv = player.inventory();
    if (!inv) return;

    ui_.text(T(StrKey::Inv_Title), 40.f, 24.f, 3.f, COL_WHITE);

    if (auto* wal = player.wallet()) {
        char goldBuf[32];
        wal->format(goldBuf, sizeof(goldBuf));
        float tw = ui_.textWidth(goldBuf, 2.f);
        ui_.text(goldBuf, (float)screenW_ - tw - 40.f, 24.f, 2.f,
                 rgba(255, 220, 100, 255));
    }

    // Кнопка сортировки
    {
        Rect r{ 200.f, 24.f, 120.f, 50.f };
        int idx = ui_.pushInteractiveRect(r, [inv]() { inv->sortMain(); });
        if (ui_.button(T(StrKey::Inv_Sort), r, idx,
                       rgba(80, 80, 120, 255), COL_WHITE)) {
            inv->sortMain();
        }
    }

    Rect close{ (float)screenW_ - 90.f, 74.f, 80.f, 50.f };
    int closeIdx = ui_.pushInteractiveRect(close, [this]() {
        screen = Screen::Hud;
        drag.clear();
    });
    if (ui_.button("X", close, closeIdx, rgba(120,60,60,255), COL_WHITE)) {
        screen = Screen::Hud;
        drag.clear();
    }

    // Основная сетка 6×4
    const int cols = 6, rows = 4;
    const float sw = 90.f, sh = 90.f, gap = 8.f;
    const float totalW = cols * sw + (cols - 1) * gap;
    const float x0 = ((float)screenW_ - totalW) * 0.5f;
    const float y0 = 140.f;

    for (int r = 0; r < rows; ++r) {
        for (int c = 0; c < cols; ++c) {
            u32 slotIdx = items::INV_MAIN_OFFSET + (u32)(r * cols + c);
            float x = x0 + c * (sw + gap);
            float y = y0 + r * (sh + gap);

            Rect slotRect{ x, y, sw, sh };

            int idx = ui_.pushInteractiveRect(slotRect, [this, &player, slotIdx]() {
                auto* inv2 = player.inventory();
                if (!inv2) return;

                if (drag.active) {
                    // Дроп в этот слот
                    if (drag.fromSlot == slotIdx) {
                        // Тот же слот — отмена
                        drag.end();
                        return;
                    }
                    auto res = inv2->putStack(slotIdx, drag.stack);
                    if (res.leftover == 0) {
                        drag.end();
                    } else {
                        drag.stack.count = res.leftover;
                    }
                } else {
                    // Обычное использование
                    auto& s = inv2->at(slotIdx);
                    if (s.empty()) return;

                    // Начать drag, если зажат долго
                    // (упрощённо: сразу начинаем drag; UI-контекст даст
                    //  нам координаты через pointerX/Y)
                    drag.begin(ui_.activePointerId(), slotIdx,
                               { ui_.pointerX(), ui_.pointerY() }, s);

                    if (onUseItem) onUseItem(slotIdx);
                }
            });

            bool pressed = ui_.isInteractivePressed(idx);
            bool isDragSrc = drag.active && drag.fromSlot == slotIdx;

            ui_.rect(x, y, sw, sh, pressed ? rgba(80, 80, 80, 220)
                                            : rgba(40, 40, 40, 220));
            ui_.rectOutline(x, y, sw, sh, 2.f,
                            pressed ? rgba(255, 220, 100, 255) : COL_BLACK);

            if (isDragSrc) continue;

            auto& s = inv->at(slotIdx);
            if (!s.empty()) {
                drawItemIcon(s, x + 6.f, y + 6.f, sw - 12.f, false);
            }
        }
    }

    // Хотбар внизу
    {
        const float hsw = 60.f;
        const float totalHW = 9.f * (hsw + 4.f) - 4.f;
        const float hx = ((float)screenW_ - totalHW) * 0.5f;
        const float hy = (float)screenH_ - 110.f;

        for (int i = 0; i < 9; ++i) {
            u32 slotIdx = items::INV_HOTBAR_OFFSET + (u32)i;
            float x = hx + i * (hsw + 4.f);

            Rect slotRect{ x, hy, hsw, hsw };
            int idx = ui_.pushInteractiveRect(slotRect, [this, &player, slotIdx]() {
                auto* inv2 = player.inventory();
                if (!inv2) return;
                if (drag.active) {
                    if (drag.fromSlot == slotIdx) { drag.end(); return; }
                    auto res = inv2->putStack(slotIdx, drag.stack);
                    if (res.leftover == 0) drag.end();
                    else drag.stack.count = res.leftover;
                } else if (onEquipHotbar) {
                    onEquipHotbar(slotIdx);
                }
            });

            bool pressed = ui_.isInteractivePressed(idx);
            ui_.rect(x, hy, hsw, hsw, pressed ? rgba(80, 80, 80, 220)
                                                : rgba(40, 40, 40, 220));
            ui_.rectOutline(x, hy, hsw, hsw, 2.f, COL_BLACK);

            bool isDragSrc = drag.active && drag.fromSlot == slotIdx;
            if (isDragSrc) continue;

            auto& s = inv->at(slotIdx);
            if (!s.empty()) {
                drawItemIcon(s, x + 4.f, hy + 4.f, hsw - 8.f, false);
            }
        }
    }

    // Экипированное
    {
        const float ex = (float)screenW_ - 200.f;
        const float ey = 200.f;
        const float es = 120.f;

        ui_.text(T(StrKey::Inv_Equipped), ex, ey - 30.f, 1.8f,
                 rgba(200,200,200,255));

        ui_.rect(ex, ey, es, es, rgba(60, 40, 20, 220));
        ui_.rectOutline(ex, ey, es, es, 2.f, rgba(200, 160, 60, 255));

        const auto& wdef = combat::weapons().get(player.equipped.weaponId);
        if (wdef.name) {
            ui_.text(wdef.name, ex + 8.f, ey + es + 8.f, 1.6f, COL_WHITE);
        }
    }

    // Подсказка
    ui_.text(T(StrKey::Inv_Hint_Tap), 40.f,
             (float)screenH_ - 160.f, 1.5f, rgba(180, 180, 180, 255));
}

// ============================================================
// Settings
// ============================================================
void UiSystem::drawSettingsScreen(player::Player& /*player*/) {
    ui_.rect(0, 0, (float)screenW_, (float)screenH_, rgba(10, 15, 25, 240));

    ui_.text(T(StrKey::Settings_Title), 40.f, 24.f, 3.f, COL_WHITE);

    Rect close{ (float)screenW_ - 90.f, 24.f, 80.f, 50.f };
    int closeIdx = ui_.pushInteractiveRect(close, [this]() { screen = Screen::PauseMenu; });
    if (ui_.button("X", close, closeIdx, rgba(120,60,60,255), COL_WHITE)) {
        screen = Screen::PauseMenu;
    }

    // Табы
    const char* tabNames[(u32)SettingsTab::Count] = {
        T(StrKey::Settings_Input),
        T(StrKey::Settings_Ui),
        T(StrKey::Settings_Audio),
        T(StrKey::Settings_Game),
        T(StrKey::Settings_Render),
    };

    const float tabW = 180.f;
    const float tabY = 84.f;
    for (u32 i = 0; i < (u32)SettingsTab::Count; ++i) {
        float x = 40.f + (f32)i * (tabW + 6.f);
        Rect r{ x, tabY, tabW, 48.f };
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

    const float panelX = 40.f;
    const float panelY = 150.f;
    const float panelW = (float)screenW_ - 80.f;
    const float rowH   = 56.f;
    const float rowGap = 8.f;

    ui_.rect(panelX, panelY, panelW, (float)screenH_ - panelY - 40.f,
             rgba(30, 30, 40, 240));
    ui_.rectOutline(panelX, panelY, panelW, (float)screenH_ - panelY - 40.f,
                    2.f, COL_BLACK);

    float y = panelY + 20.f;
    const float innerW = panelW - 40.f;
    const float innerX = panelX + 20.f;

    auto changeCb = [this]() { if (onSettingsChanged) onSettingsChanged(); };

    switch (settingsTab) {
        case SettingsTab::Input: {
            // Sensitivity
            {
                Rect r{ innerX, y, innerW, rowH };
                sliderWidget(ui_, r, &s.cameraSensitivity,
                             0.1f, 5.0f, T(StrKey::Settings_CameraSens),
                             0.05f, [&changeCb](float){ changeCb(); });
                y += rowH + rowGap;
            }
            // Invert X
            {
                Rect r{ innerX, y, innerW, rowH };
                ui_.pushInteractiveRect(r, [&s, &changeCb]() {
                    s.invertX = !s.invertX; changeCb();
                });
                toggleWidget(ui_, r, &s.invertX, T(StrKey::Settings_InvertX));
                y += rowH + rowGap;
            }
            // Invert Y
            {
                Rect r{ innerX, y, innerW, rowH };
                ui_.pushInteractiveRect(r, [&s, &changeCb]() {
                    s.invertY = !s.invertY; changeCb();
                });
                toggleWidget(ui_, r, &s.invertY, T(StrKey::Settings_InvertY));
                y += rowH + rowGap;
            }
            // Joystick left
            {
                Rect r{ innerX, y, innerW, rowH };
                int idx = ui_.pushInteractiveRect(r, [&s, &changeCb]() {
                    s.joystickLeftHanded = !s.joystickLeftHanded; changeCb();
                });
                (void)idx;
                toggleWidget(ui_, r, &s.joystickLeftHanded,
                             T(StrKey::Settings_JoystickLeft));
                y += rowH + rowGap;
            }
            // Joystick radius
            {
                Rect r{ innerX, y, innerW, rowH };
                sliderWidget(ui_, r, &s.joystickRadius,
                             80.f, 220.f, T(StrKey::Settings_JoystickRadius),
                             5.f, [&changeCb](float){ changeCb(); });
                y += rowH + rowGap;
            }
            // Deadzone
            {
                Rect r{ innerX, y, innerW, rowH };
                sliderWidget(ui_, r, &s.joystickDeadzone,
                             0.05f, 0.40f, T(StrKey::Settings_JoystickDeadzone),
                             0.01f, [&changeCb](float){ changeCb(); });
                y += rowH + rowGap;
            }
            // Прозрачность джойстика
            {
                Rect r{ innerX, y, innerW, rowH };
                sliderWidget(ui_, r, &s.joystickOpacity,
                             0.2f, 1.0f, T(StrKey::Settings_JoystickOpacity),
                             0.05f, [&changeCb](float){ changeCb(); });
                y += rowH + rowGap;
            }
            // Размер кнопок
            {
                Rect r{ innerX, y, innerW, rowH };
                sliderWidget(ui_, r, &s.buttonScale,
                             0.6f, 1.6f, T(StrKey::Settings_ButtonScale),
                             0.05f, [&changeCb](float){ changeCb(); });
                y += rowH + rowGap;
            }
            // Прозрачность кнопок
            {
                Rect r{ innerX, y, innerW, rowH };
                sliderWidget(ui_, r, &s.buttonOpacity,
                             0.2f, 1.0f, T(StrKey::Settings_ButtonOpacity),
                             0.05f, [&changeCb](float){ changeCb(); });
                y += rowH + rowGap;
            }
            // Режим перемещения кнопок (ТЗ 5.2)
            {
                Rect r{ innerX, y, innerW, rowH };
                ui_.pushInteractiveRect(r, [this]() {
                    buttonLayoutMode = !buttonLayoutMode;
                });
                toggleWidget(ui_, r, &buttonLayoutMode,
                             T(StrKey::Settings_ButtonLayout));
                y += rowH + rowGap;
            }
            if (buttonLayoutMode) {
                ui_.text(T(StrKey::Settings_LayoutHint),
                         innerX + 8.f, y + rowH * 0.5f, 1.f,
                         rgba(176, 176, 176, 255));
                y += rowH + rowGap;

                Rect r{ innerX, y, 260.f, rowH };
                int idx = ui_.pushInteractiveRect(r, [&s, &changeCb]() {
                    for (u32 i = 0; i < cfg::Settings::BUTTON_SLOTS; ++i) {
                        s.buttonOffsetX[i] = 0.f;
                        s.buttonOffsetY[i] = 0.f;
                    }
                    changeCb();
                });
                if (ui_.button(T(StrKey::Settings_ResetLayout), r, idx,
                               rgba(90, 90, 110, 255), COL_WHITE)) {
                    for (u32 i = 0; i < cfg::Settings::BUTTON_SLOTS; ++i) {
                        s.buttonOffsetX[i] = 0.f;
                        s.buttonOffsetY[i] = 0.f;
                    }
                    changeCb();
                }
                y += rowH + rowGap;
            }
            break;
        }

        case SettingsTab::Ui: {
            {
                Rect r{ innerX, y, innerW, rowH };
                sliderWidget(ui_, r, &s.uiScale, 0.75f, 1.5f,
                             T(StrKey::Settings_UiScale), 0.05f,
                             [&changeCb](float){ changeCb(); });
                y += rowH + rowGap;
            }
            {
                Rect r{ innerX, y, innerW, rowH };
                sliderWidget(ui_, r, &s.uiOpacity, 0.4f, 1.0f,
                             T(StrKey::Settings_UiOpacity), 0.05f,
                             [&changeCb](float){ changeCb(); });
                y += rowH + rowGap;
            }
            {
                Rect r{ innerX, y, innerW, rowH };
                int idx = ui_.pushInteractiveRect(r, [&s, &changeCb]() {
                    s.showFps = !s.showFps; changeCb();
                });
                (void)idx;
                toggleWidget(ui_, r, &s.showFps, T(StrKey::Settings_ShowFps));
                y += rowH + rowGap;
            }
            {
                Rect r{ innerX, y, innerW, rowH };
                int idx = ui_.pushInteractiveRect(r, [&s, &changeCb]() {
                    s.showDebugPos = !s.showDebugPos; changeCb();
                });
                (void)idx;
                toggleWidget(ui_, r, &s.showDebugPos, T(StrKey::Settings_ShowDebug));
                y += rowH + rowGap;
            }
            break;
        }

        case SettingsTab::Audio: {
            {
                Rect r{ innerX, y, innerW, rowH };
                sliderWidget(ui_, r, &s.masterVolume, 0.f, 1.f,
                             T(StrKey::Settings_Master), 0.05f,
                             [&changeCb](float){ changeCb(); });
                y += rowH + rowGap;
            }
            {
                Rect r{ innerX, y, innerW, rowH };
                sliderWidget(ui_, r, &s.musicVolume, 0.f, 1.f,
                             T(StrKey::Settings_Music), 0.05f,
                             [&changeCb](float){ changeCb(); });
                y += rowH + rowGap;
            }
            {
                Rect r{ innerX, y, innerW, rowH };
                sliderWidget(ui_, r, &s.sfxVolume, 0.f, 1.f,
                             T(StrKey::Settings_Sfx), 0.05f,
                             [&changeCb](float){ changeCb(); });
                y += rowH + rowGap;
            }
            break;
        }

        case SettingsTab::Game: {
            // Language cycle
            {
                Rect r{ innerX, y, innerW, rowH };
                const char* opts[(u32)cfg::Language::Count] = {
                    cfg::languageName(cfg::Language::English),
                    cfg::languageName(cfg::Language::Russian),
                };
                u32 cur = (u32)s.language;
                int idx = ui_.pushInteractiveRect(r, [&s, &changeCb]() {
                    u32 n = (u32)s.language + 1;
                    if (n >= (u32)cfg::Language::Count) n = 0;
                    s.language = (cfg::Language)n;
                    cfg::L().setLanguage(s.language);
                    changeCb();
                });
                (void)idx;
                cycleWidget(ui_, r, T(StrKey::Settings_Language), opts,
                            (u32)cfg::Language::Count, &cur);
                y += rowH + rowGap;
            }
            {
                Rect r{ innerX, y, innerW, rowH };
                int idx = ui_.pushInteractiveRect(r, [&s, &changeCb]() {
                    s.autosaveEnabled = !s.autosaveEnabled; changeCb();
                });
                (void)idx;
                toggleWidget(ui_, r, &s.autosaveEnabled,
                             T(StrKey::Settings_Autosave));
                y += rowH + rowGap;
            }
            {
                Rect r{ innerX, y, innerW, rowH };
                sliderWidget(ui_, r, &s.autosaveInterval, 60.f, 900.f,
                             T(StrKey::Settings_AutosaveInterval), 30.f,
                             [&changeCb](float){ changeCb(); });
                y += rowH + rowGap;
            }
            break;
        }

        case SettingsTab::Render: {
            {
                Rect r{ innerX, y, innerW, rowH };
                f32 vd = (f32)s.viewDistance;
                sliderWidget(ui_, r, &vd, 4.f, 12.f,
                             T(StrKey::Settings_ViewDistance), 1.f,
                             [&s, &changeCb](float v) {
                                 s.viewDistance = (i32)(v + 0.5f);
                                 changeCb();
                             });
                y += rowH + rowGap;
            }
            {
                Rect r{ innerX, y, innerW, rowH };
                int idx = ui_.pushInteractiveRect(r, [&s, &changeCb]() {
                    s.vsync = !s.vsync; changeCb();
                });
                (void)idx;
                toggleWidget(ui_, r, &s.vsync, T(StrKey::Settings_Vsync));
                y += rowH + rowGap;
            }
            break;
        }

        default: break;
    }

    // Кнопка "Reset to defaults"
    {
        Rect r{ innerX, (float)screenH_ - 90.f, 260.f, 50.f };
        int idx = ui_.pushInteractiveRect(r, [&s, &changeCb]() {
            cfg::Settings def{};
            s = def;
            cfg::L().setLanguage(s.language);
            changeCb();
        });
        if (ui_.button(T(StrKey::Settings_ResetAll), r, idx,
                       rgba(140, 60, 60, 255), COL_WHITE)) {
            cfg::Settings def{};
            s = def;
            cfg::L().setLanguage(s.language);
            changeCb();
        }
    }
}

// ============================================================
// Skill Tree
// ============================================================
void UiSystem::drawSkillTreeScreen(player::Player& player) {
    ui_.rect(0, 0, (float)screenW_, (float)screenH_, rgba(10, 5, 30, 235));

    auto* tree = player.skillTree();
    if (!tree) return;

    ui_.text("TREE OF KNOWLEDGE", (float)screenW_ * 0.35f, 24.f, 3.f, COL_WHITE);

    char ptsBuf[64];
    std::snprintf(ptsBuf, sizeof(ptsBuf), "Skill Points: %d", tree->unspentPoints);
    ui_.text(ptsBuf, (float)screenW_ - 340.f, 24.f, 2.f,
             tree->unspentPoints > 0 ? rgba(255, 220, 100, 255) : COL_WHITE);

    Rect close{ (float)screenW_ - 90.f, 74.f, 80.f, 50.f };
    int closeIdx = ui_.pushInteractiveRect(close, [this]() { screen = Screen::Hud; });
    if (ui_.button("X", close, closeIdx, rgba(120,60,60,255), COL_WHITE)) {
        screen = Screen::Hud; return;
    }

    const f32 colW = (f32)screenW_ * 0.30f;
    const f32 gap  = (f32)screenW_ * 0.02f;
    const f32 totalW = colW * 3.f + gap * 2.f;
    const f32 x0 = ((f32)screenW_ - totalW) * 0.5f;

    const f32 nodeH = (f32)screenH_ * 0.062f;
    const f32 nodeGap = (f32)screenH_ * 0.012f;
    const f32 colTop = 150.f;

    SkillBranch branches[3] = {
        SkillBranch::Strength, SkillBranch::Agility, SkillBranch::Wisdom,
    };
    const char* branchNames[3] = { "STRENGTH", "AGILITY", "WISDOM" };
    UiColor branchColors[3] = {
        rgba(220, 80, 60, 255),
        rgba(80, 200, 80, 255),
        rgba(80, 140, 240, 255),
    };

    for (int b = 0; b < 3; ++b) {
        const f32 cx = x0 + b * (colW + gap);
        ui_.rect(cx, colTop - 40.f, colW, 32.f, branchColors[b]);
        f32 tw = ui_.textWidth(branchNames[b], 2.f);
        ui_.text(branchNames[b], cx + (colW - tw) * 0.5f, colTop - 36.f, 2.f, COL_WHITE);

        u16 nodeCount = 0;
        const auto* nodes = skillTree().nodesInBranch(branches[b], nodeCount);

        for (u16 i = 0; i < nodeCount; ++i) {
            const auto& def = nodes[i];
            f32 ny = colTop + i * (nodeH + nodeGap);

            bool maxed   = tree->rank(def.id) >= def.maxRank;
            bool canOpen = tree->canUnlock(def.id);
            u8   rank    = tree->rank(def.id);

            UiColor bg;
            if (maxed)        bg = rgba(220, 180, 60, 220);
            else if (canOpen) bg = rgba(60, 100, 60, 220);
            else              bg = rgba(40, 40, 40, 200);

            Rect nr{ cx, ny, colW, nodeH };
            int ni = ui_.pushInteractiveRect(nr, [tree, id = def.id]() {
                tree->unlock(id);
            });

            bool pressed = ui_.isInteractivePressed(ni);
            if (pressed) bg = rgba(140, 140, 200, 255);

            ui_.rect(nr.x, nr.y, nr.w, nr.h, bg);
            ui_.rectOutline(nr.x, nr.y, nr.w, nr.h, 2.f, COL_BLACK);

            char label[96];
            std::snprintf(label, sizeof(label), "%s  [%u/%u]",
                          def.name, (unsigned)rank, (unsigned)def.maxRank);
            ui_.text(label, nr.x + 8.f, nr.y + 6.f, 1.6f,
                     canOpen || maxed ? COL_WHITE : rgba(160,160,160,255));
        }
    }
}

// ============================================================
// Attributes
// ============================================================
void UiSystem::drawAttributesScreen(player::Player& player) {
    ui_.rect(0, 0, (float)screenW_, (float)screenH_, rgba(10, 20, 30, 235));

    auto* prog = player.progression();
    auto* attr = player.attributes();
    if (!prog || !attr) return;

    ui_.text(T(StrKey::Menu_Attributes), (float)screenW_ * 0.35f, 24.f, 3.f, COL_WHITE);

    char ptsBuf[64];
    std::snprintf(ptsBuf, sizeof(ptsBuf), "Points: %d", prog->availableAttrPoints);
    ui_.text(ptsBuf, (float)screenW_ - 240.f, 24.f, 2.f,
             prog->availableAttrPoints > 0 ? rgba(255, 220, 100, 255) : COL_WHITE);

    Rect close{ (float)screenW_ - 90.f, 74.f, 80.f, 50.f };
    int closeIdx = ui_.pushInteractiveRect(close, [this]() { screen = Screen::Hud; });
    if (ui_.button("X", close, closeIdx, rgba(120,60,60,255), COL_WHITE)) {
        screen = Screen::Hud; return;
    }

    const f32 rowW = (f32)screenW_ * 0.6f;
    const f32 rowH = 70.f;
    const f32 gap  = 20.f;
    const f32 x0 = ((f32)screenW_ - rowW) * 0.5f;
    const f32 y0 = (f32)screenH_ * 0.25f;

    struct AttrRow {
        const char* label;
        const char* desc;
        i32* value;
        UiColor color;
    };
    AttrRow rows[4] = {
        { "STRENGTH",     "+melee damage, +crit dmg",
          &attr->strength,     rgba(220, 80, 60, 255) },
        { "AGILITY",      "+attack speed, +crit, +move",
          &attr->agility,      rgba(80, 200, 80, 255) },
        { "INTELLIGENCE", "+mana, +spell power",
          &attr->intelligence, rgba(80, 140, 240, 255) },
        { "ENDURANCE",    "+health, +resist, +stamina",
          &attr->endurance,    rgba(220, 160, 60, 255) },
    };

    for (int i = 0; i < 4; ++i) {
        const f32 y = y0 + i * (rowH + gap);
        ui_.rect(x0, y, rowW, rowH, rgba(30, 40, 55, 240));
        ui_.rectOutline(x0, y, rowW, rowH, 2.f, COL_BLACK);
        ui_.rect(x0, y, 8.f, rowH, rows[i].color);

        ui_.text(rows[i].label, x0 + 24.f, y + 8.f, 2.f, COL_WHITE);
        ui_.text(rows[i].desc, x0 + 24.f, y + 34.f, 1.3f, rgba(180, 180, 180, 255));

        char val[16];
        std::snprintf(val, sizeof(val), "%d", *rows[i].value);
        f32 vw = ui_.textWidth(val, 3.f);
        ui_.text(val, x0 + rowW - 200.f - vw * 0.5f, y + 18.f, 3.f, COL_WHITE);

        const f32 btnSize = 50.f;
        const f32 btnY = y + (rowH - btnSize) * 0.5f;
        const f32 plusX  = x0 + rowW - btnSize - 12.f;
        const f32 minusX = plusX - btnSize - 12.f;

        Rect mr{ minusX, btnY, btnSize, btnSize };
        int mi = ui_.pushInteractiveRect(mr, [prog, i, rows]() {
            if (*rows[i].value <= 1) return;
            *rows[i].value -= 1;
            prog->availableAttrPoints += 1;
            prog->derivedDirty = true;
        });
        {
            bool p = ui_.isInteractivePressed(mi);
            UiColor c = p ? rgba(200, 60, 60, 255) : rgba(120, 40, 40, 255);
            ui_.rect(mr.x, mr.y, mr.w, mr.h, c);
            ui_.rectOutline(mr.x, mr.y, mr.w, mr.h, 2.f, COL_BLACK);
            ui_.text("-", mr.x + 16.f, mr.y + 8.f, 3.f, COL_WHITE);
        }

        Rect pr{ plusX, btnY, btnSize, btnSize };
        bool canAdd = prog->availableAttrPoints > 0 && *rows[i].value < 99;
        int pi = ui_.pushInteractiveRect(pr, [prog, i, rows, canAdd]() {
            if (!canAdd) return;
            *rows[i].value += 1;
            prog->availableAttrPoints -= 1;
            prog->derivedDirty = true;
        });
        {
            bool p = ui_.isInteractivePressed(pi) && canAdd;
            UiColor c = !canAdd ? rgba(60, 60, 60, 255)
                       : (p ? rgba(80, 200, 80, 255) : rgba(40, 120, 40, 255));
            ui_.rect(pr.x, pr.y, pr.w, pr.h, c);
            ui_.rectOutline(pr.x, pr.y, pr.w, pr.h, 2.f, COL_BLACK);
            ui_.text("+", pr.x + 14.f, pr.y + 8.f, 3.f, COL_WHITE);
        }
    }
}

// ============================================================
// Dialogue
// ============================================================
void UiSystem::drawDialogueScreen(player::Player& player) {
    auto* dlg = player.activeDialogue();
    if (!dlg || !dlg->active) { screen = Screen::Hud; return; }

    ui_.rect(0, 0, (float)screenW_, (float)screenH_, rgba(0,0,0,80));

    const float panelH = (float)screenH_ * 0.35f;
    const float panelY = (float)screenH_ - panelH - 20.f;
    const float panelX = 40.f;
    const float panelW = (float)screenW_ - 80.f;

    ui_.rect(panelX, panelY, panelW, panelH, rgba(20, 15, 30, 235));
    ui_.rectOutline(panelX, panelY, panelW, panelH, 3.f, rgba(220, 200, 120, 255));

    npc::DialogueNode* node = dlg->findNode(dlg->currentNodeId);
    if (!node) { screen = Screen::Hud; return; }

    ui_.text(node->text, panelX + 24.f, panelY + 24.f, 2.2f, COL_WHITE);

    f32 cy = panelY + 90.f;
    f32 rowH = 44.f;

    for (usize i = 0; i < node->choices.size(); ++i) {
        const auto& c = node->choices[i];
        Rect cr{ panelX + 24.f, cy, panelW - 48.f, rowH };
        int idx = ui_.pushInteractiveRect(cr, [](){});
        bool pressed = ui_.isInteractivePressed(idx);
        UiColor fill = pressed ? rgba(120, 100, 60, 255) : rgba(40, 35, 50, 235);
        ui_.rect(cr.x, cr.y, cr.w, cr.h, fill);
        ui_.rectOutline(cr.x, cr.y, cr.w, cr.h, 2.f, rgba(200, 180, 100, 255));

        char line[256];
        std::snprintf(line, sizeof(line), "%d. %s",
                      (int)(i + 1), c.text.c_str());
        ui_.text(line, cr.x + 12.f, cr.y + 12.f, 2.f, COL_WHITE);
        cy += rowH + 6.f;
    }
}

// ============================================================
// Quest Log
// ============================================================
void UiSystem::drawQuestLogScreen(player::Player& player) {
    ui_.rect(0, 0, (float)screenW_, (float)screenH_, rgba(10, 15, 25, 235));

    auto* log = player.questLog();
    if (!log) return;

    ui_.text(T(StrKey::Quest_Title), (float)screenW_ * 0.36f, 24.f, 3.f, COL_WHITE);

    Rect close{ (float)screenW_ - 90.f, 74.f, 80.f, 50.f };
    int closeIdx = ui_.pushInteractiveRect(close, [this]() { screen = Screen::Hud; });
    if (ui_.button("X", close, closeIdx, rgba(120,60,60,255), COL_WHITE)) {
        screen = Screen::Hud; return;
    }

    const f32 panelX = 40.f;
    const f32 panelY = 140.f;
    const f32 panelW = (f32)screenW_ - 80.f;
    const f32 panelH = (f32)screenH_ - 200.f;

    ui_.rect(panelX, panelY, panelW, panelH, rgba(25, 30, 45, 240));
    ui_.rectOutline(panelX, panelY, panelW, panelH, 2.f, COL_BLACK);

    ui_.text(T(StrKey::Quest_Active), panelX + 16.f, panelY + 12.f, 2.2f,
             rgba(255, 220, 120, 255));

    f32 y = panelY + 60.f;
    if (log->activeQuests.empty()) {
        ui_.text(T(StrKey::Quest_None), panelX + 24.f, y, 2.f,
                 rgba(180,180,180,255));
    } else {
        char buf[64];
        std::snprintf(buf, sizeof(buf), "%zu", log->activeQuests.size());
        ui_.text(buf, panelX + 24.f, y, 2.f, COL_WHITE);
        y += 80.f;
    }

    ui_.text(T(StrKey::Quest_Completed), panelX + 16.f, y + 20.f, 2.2f,
             rgba(120, 220, 140, 255));
    y += 60.f;

    if (log->history.empty()) {
        ui_.text("-", panelX + 24.f, y, 2.f, rgba(140,140,140,255));
    } else {
        usize start = log->history.size() > 8 ? log->history.size() - 8 : 0;
        for (usize i = start; i < log->history.size(); ++i) {
            const auto& h = log->history[i];
            char buf[128];
            std::snprintf(buf, sizeof(buf), "%s", h.title);
            ui_.text(buf, panelX + 24.f, y, 1.8f, COL_WHITE);
            y += 30.f;
        }
    }
}

// ============================================================
// Reputation
// ============================================================
void UiSystem::drawReputationScreen(player::Player& player) {
    ui_.rect(0, 0, (float)screenW_, (float)screenH_, rgba(15, 20, 25, 235));

    auto* rep = player.reputation();
    if (!rep) return;

    ui_.text(T(StrKey::Rep_Title), (float)screenW_ * 0.40f, 24.f, 3.f, COL_WHITE);

    Rect close{ (float)screenW_ - 90.f, 74.f, 80.f, 50.f };
    int closeIdx = ui_.pushInteractiveRect(close, [this]() { screen = Screen::Hud; });
    if (ui_.button("X", close, closeIdx, rgba(120,60,60,255), COL_WHITE)) {
        screen = Screen::Hud; return;
    }

    const f32 panelX = 80.f;
    const f32 panelY = 150.f;
    const f32 panelW = (f32)screenW_ - 160.f;
    const f32 rowH  = 80.f;
    const f32 gap   = 14.f;

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

    f32 y = panelY;
    for (factions::FactionId f : facs) {
        i32 v = rep->get(f);
        auto tier = rep->tier(f);

        ui_.rect(panelX, y, panelW, rowH, rgba(30, 40, 55, 240));
        ui_.rectOutline(panelX, y, panelW, rowH, 2.f, COL_BLACK);

        ui_.text(factions::factionName(f), panelX + 20.f, y + 12.f, 2.2f, COL_WHITE);

        char valueBuf[64];
        std::snprintf(valueBuf, sizeof(valueBuf), "%d  (%s)",
                      v, factions::tierName(tier));
        ui_.text(valueBuf, panelX + 20.f, y + 44.f, 1.8f, colorFor(tier));

        f32 norm = ((f32)v + 1000.f) / 2000.f;
        if (norm < 0.f) norm = 0.f;
        if (norm > 1.f) norm = 1.f;

        const f32 barX = panelX + panelW * 0.55f;
        const f32 barY = y + 26.f;
        const f32 barW = panelW * 0.40f;
        const f32 barH = 20.f;

        ui_.rect(barX - 2, barY - 2, barW + 4, barH + 4, COL_BLACK);
        ui_.rect(barX, barY, barW, barH, rgba(40, 40, 40, 255));
        ui_.rect(barX, barY, barW * norm, barH, colorFor(tier));

        y += rowH + gap;
    }
}

// ============================================================
// Save / Load
// ============================================================
void UiSystem::drawSaveLoadScreen(player::Player& player) {
    (void)player;
    ui_.rect(0, 0, (float)screenW_, (float)screenH_, rgba(5, 10, 20, 240));

    const char* title = (saveLoadMode == SaveLoadMode::Save)
        ? "SAVE GAME" : "LOAD GAME";
    ui_.text(title, (float)screenW_ * 0.38f, 24.f, 3.f, COL_WHITE);

    Rect close{ (float)screenW_ - 90.f, 74.f, 80.f, 50.f };
    int closeIdx = ui_.pushInteractiveRect(close, [this]() { screen = Screen::Hud; });
    if (ui_.button("X", close, closeIdx, rgba(120,60,60,255), COL_WHITE)) {
        screen = Screen::Hud; return;
    }

    {
        Rect r{ 40.f, 74.f, 200.f, 50.f };
        const char* label = (saveLoadMode == SaveLoadMode::Save)
            ? "MODE: SAVE" : "MODE: LOAD";
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

    const f32 gridX = 60.f;
    const f32 gridY = 150.f;
    const f32 cellW = ((f32)screenW_ - gridX * 2.f) / (f32)S - 16.f;
    const f32 cellH = 180.f;
    const f32 gap   = 16.f;

    for (u32 p = 0; p < P; ++p) {
        for (u32 s = 0; s < S; ++s) {
            const f32 x = gridX + (f32)s * (cellW + gap);
            const f32 y = gridY + (f32)p * (cellH + gap);

            const auto& meta = slotMeta[p][s];
            Rect cell{ x, y, cellW, cellH };

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
            ui_.rectOutline(cell.x, cell.y, cell.w, cell.h, 2.f,
                            meta.exists ? rgba(180, 200, 160, 255)
                                        : rgba(120, 100, 100, 255));

            char hdr[48];
            std::snprintf(hdr, sizeof(hdr), "P%u  S%u", p + 1, s + 1);
            ui_.text(hdr, cell.x + 10.f, cell.y + 8.f, 1.8f, COL_WHITE);

            if (meta.exists) {
                ui_.text(meta.worldName, cell.x + 10.f, cell.y + 36.f,
                         1.6f, rgba(200, 220, 255, 255));
                char buf[64];
                std::snprintf(buf, sizeof(buf), "Lv %u  |  %us",
                              meta.playerLevel, meta.playtimeSec);
                ui_.text(buf, cell.x + 10.f, cell.y + 60.f, 1.5f,
                         rgba(220, 220, 180, 255));

                time_t ts = (time_t)(meta.timestampMs / 1000ULL);
                struct tm* tmv = std::localtime(&ts);
                if (tmv) {
                    char dateBuf[64];
                    std::snprintf(dateBuf, sizeof(dateBuf),
                                  "%02d %s %04d  %02d:%02d",
                                  tmv->tm_mday,
                                  gMonthNames[tmv->tm_mon % 12],
                                  tmv->tm_year + 1900,
                                  tmv->tm_hour, tmv->tm_min);
                    ui_.text(dateBuf, cell.x + 10.f, cell.y + 84.f,
                             1.4f, rgba(200,200,200,255));
                }

                Rect del{ cell.x + cell.w - 90.f, cell.y + cell.h - 46.f,
                          80.f, 36.f };
                int di = ui_.pushInteractiveRect(del, [this, p, s]() {
                    if (onDeleteRequested) onDeleteRequested(p, s);
                });
                bool dpressed = ui_.isInteractivePressed(di);
                ui_.rect(del.x, del.y, del.w, del.h,
                         dpressed ? rgba(200, 60, 60, 255)
                                  : rgba(120, 40, 40, 220));
                ui_.rectOutline(del.x, del.y, del.w, del.h, 2.f, COL_BLACK);
                ui_.text("DEL", del.x + 20.f, del.y + 8.f, 1.8f, COL_WHITE);
            } else {
                ui_.text("Empty", cell.x + 10.f, cell.y + 60.f, 2.f,
                         rgba(160, 160, 160, 255));
            }
        }
    }
}

// ============================================================
// Crafting
// ============================================================
void UiSystem::drawCraftingScreen(player::Player& player) {
    ui_.rect(0, 0, (float)screenW_, (float)screenH_, rgba(20, 15, 10, 240));

    const char* stName = crafting::stationName(nearbyStation);
    char title[64];
    std::snprintf(title, sizeof(title), "%s — %s",
                  T(StrKey::Craft_Title), stName);
    ui_.text(title, (float)screenW_ * 0.28f, 24.f, 3.f, COL_WHITE);

    Rect close{ (float)screenW_ - 90.f, 74.f, 80.f, 50.f };
    int closeIdx = ui_.pushInteractiveRect(close, [this]() { screen = Screen::Hud; });
    if (ui_.button("X", close, closeIdx, rgba(120,60,60,255), COL_WHITE)) {
        screen = Screen::Hud; return;
    }

    auto* inv = player.inventory();
    auto* tree = player.skillTree();
    auto* prog = player.progression();
    if (!inv) return;

    crafting::CraftContext ctx{};
    ctx.inventory     = inv;
    ctx.skillTree     = tree;
    ctx.playerLevel   = prog ? prog->level : 1;
    ctx.nearbyStation = nearbyStation;

    std::vector<crafting::AvailableRecipe> available;
    crafting::gatherAvailable(ctx, available);

    const f32 listX = 40.f;
    const f32 listY = 130.f;
    const f32 listW = (f32)screenW_ * 0.55f;
    const f32 rowH  = 70.f;
    const f32 rowGap = 8.f;

    i32 visible = (i32)(((f32)screenH_ - listY - 40.f) / (rowH + rowGap));
    i32 total = (i32)available.size();

    craftScroll.setContentHeight((f32)total * (rowH + rowGap),
                                 (f32)visible * (rowH + rowGap));

    i32 start = (i32)(craftScroll.offset / (rowH + rowGap));

    for (i32 i = 0; i < visible && (start + i) < total; ++i) {
        i32 idx = start + i;
        auto& ar = available[idx];
        const auto& r = *ar.recipe;

        f32 y = listY + i * (rowH + rowGap) - (craftScroll.offset -
                  start * (rowH + rowGap));

        bool canNow = (ar.status == crafting::CraftStatus::Ok);
        bool isSel  = (selectedRecipeIdx == idx);

        UiColor bg = isSel
            ? rgba(120, 100, 60, 240)
            : (canNow ? rgba(40, 60, 40, 220) : rgba(40, 40, 40, 180));

        Rect rr{ listX, y, listW, rowH };
        int ri = ui_.pushInteractiveRect(rr, [this, idx]() {
            selectedRecipeIdx = idx;
        });

        bool pressed = ui_.isInteractivePressed(ri);
        if (pressed) bg = rgba(150, 130, 80, 255);

        ui_.rect(rr.x, rr.y, rr.w, rr.h, bg);
        ui_.rectOutline(rr.x, rr.y, rr.w, rr.h, 2.f,
                        canNow ? rgba(120, 220, 120, 255) : COL_BLACK);

        const auto& outDef = items::items().get(r.output.itemId);
        ui_.text(outDef.name, rr.x + 12.f, rr.y + 8.f, 2.f,
                 canNow ? COL_WHITE : rgba(160, 160, 160, 255));

        char costLine[256] = {};
        for (usize k = 0; k < r.inputs.size(); ++k) {
            const auto& in = r.inputs[k];
            const auto& inDef = items::items().get(in.itemId);
            char piece[64];
            std::snprintf(piece, sizeof(piece), "%sx%u ",
                          inDef.name, (unsigned)in.count);
            std::strncat(costLine, piece,
                         sizeof(costLine) - std::strlen(costLine) - 1);
        }
        ui_.text(costLine, rr.x + 12.f, rr.y + 36.f, 1.4f,
                 rgba(200, 200, 200, 255));

        if (!canNow) {
            ui_.text(crafting::statusString(ar.status),
                     rr.x + 12.f, rr.y + 52.f, 1.3f,
                     rgba(220, 120, 120, 255));
        }
    }

    if (total > visible) {
        Rect upBtn{ listX + listW + 10.f, listY, 60.f, 60.f };
        int ui = ui_.pushInteractiveRect(upBtn, [this]() {
            craftScroll.offset -= 60.f;
            craftScroll.clampOffset();
        });
        bool up = ui_.isInteractivePressed(ui);
        ui_.rect(upBtn.x, upBtn.y, upBtn.w, upBtn.h,
                 up ? rgba(180,180,180,255) : rgba(60,60,60,220));
        ui_.rectOutline(upBtn.x, upBtn.y, upBtn.w, upBtn.h, 2.f, COL_BLACK);
        ui_.text("^", upBtn.x + 20.f, upBtn.y + 16.f, 2.f, COL_WHITE);

        Rect downBtn{ listX + listW + 10.f, listY + 70.f, 60.f, 60.f };
        int di = ui_.pushInteractiveRect(downBtn, [this]() {
            craftScroll.offset += 60.f;
            craftScroll.clampOffset();
        });
        bool dn = ui_.isInteractivePressed(di);
        ui_.rect(downBtn.x, downBtn.y, downBtn.w, downBtn.h,
                 dn ? rgba(180,180,180,255) : rgba(60,60,60,220));
        ui_.rectOutline(downBtn.x, downBtn.y, downBtn.w, downBtn.h, 2.f, COL_BLACK);
        ui_.text("v", downBtn.x + 20.f, downBtn.y + 16.f, 2.f, COL_WHITE);
    }

    // Правая панель
    {
        const f32 pX = (f32)screenW_ * 0.62f;
        const f32 pY = 130.f;
        const f32 pW = (f32)screenW_ - pX - 40.f;
        const f32 pH = (f32)screenH_ - pY - 40.f;

        ui_.rect(pX, pY, pW, pH, rgba(30, 25, 20, 240));
        ui_.rectOutline(pX, pY, pW, pH, 2.f, COL_BLACK);

        if (selectedRecipeIdx >= 0 && selectedRecipeIdx < total) {
            auto& ar = available[selectedRecipeIdx];
            const auto& r = *ar.recipe;
            const auto& outDef = items::items().get(r.output.itemId);

            ui_.text(outDef.name, pX + 16.f, pY + 12.f, 2.2f, COL_WHITE);

            char lvlBuf[64];
            std::snprintf(lvlBuf, sizeof(lvlBuf), "Level %u",
                          (unsigned)r.requiredLevel);
            ui_.text(lvlBuf, pX + 16.f, pY + 44.f, 1.5f,
                     rgba(200, 200, 200, 255));

            ui_.text(T(StrKey::Craft_Ingredients), pX + 16.f, pY + 80.f,
                     1.8f, rgba(255, 220, 120, 255));

            f32 iy = pY + 110.f;
            for (const auto& in : r.inputs) {
                const auto& inDef = items::items().get(in.itemId);
                u32 have = inv->countOf(in.itemId);
                bool ok = have >= in.count;

                char line[128];
                std::snprintf(line, sizeof(line), "%s  %u / %u",
                              inDef.name, (unsigned)have, (unsigned)in.count);
                ui_.text(line, pX + 24.f, iy, 1.6f,
                         ok ? rgba(180, 240, 180, 255)
                            : rgba(240, 160, 160, 255));
                iy += 26.f;
            }

            if (ar.status == crafting::CraftStatus::Ok) {
                Rect cr{ pX + 16.f, pY + pH - 80.f, pW - 32.f, 60.f };
                int ci = ui_.pushInteractiveRect(cr, [this, r]() {
                    if (onCraft) onCraft(r.id);
                });
                bool cpress = ui_.isInteractivePressed(ci);
                UiColor c = cpress ? rgba(80, 200, 80, 255)
                                   : rgba(40, 120, 40, 255);
                ui_.rect(cr.x, cr.y, cr.w, cr.h, c);
                ui_.rectOutline(cr.x, cr.y, cr.w, cr.h, 2.f, COL_BLACK);
                float tw = ui_.textWidth(T(StrKey::Craft_Button), 2.4f);
                ui_.text(T(StrKey::Craft_Button),
                         cr.x + (cr.w - tw) * 0.5f, cr.y + 16.f,
                         2.4f, COL_WHITE);
            } else {
                Rect cr{ pX + 16.f, pY + pH - 80.f, pW - 32.f, 60.f };
                ui_.rect(cr.x, cr.y, cr.w, cr.h, rgba(60, 40, 40, 255));
                ui_.rectOutline(cr.x, cr.y, cr.w, cr.h, 2.f, COL_BLACK);
                float tw = ui_.textWidth(crafting::statusString(ar.status), 1.8f);
                ui_.text(crafting::statusString(ar.status),
                         cr.x + (cr.w - tw) * 0.5f, cr.y + 22.f,
                         1.8f, rgba(220, 160, 160, 255));
            }
        } else {
            ui_.text(T(StrKey::Craft_Select), pX + 16.f, pY + 40.f, 2.f,
                     rgba(180, 180, 180, 255));
        }
    }
}

// ============================================================
// Trade
// ============================================================
void UiSystem::drawTradeScreen(player::Player& player) {
    ui_.rect(0, 0, (float)screenW_, (float)screenH_, rgba(10, 20, 15, 240));

    auto* inv = player.inventory();
    auto* wal = player.wallet();
    if (!inv || !wal) return;

    ui_.text(T(StrKey::Trade_Title), (float)screenW_ * 0.42f, 24.f, 3.f, COL_WHITE);

    Rect close{ (float)screenW_ - 90.f, 74.f, 80.f, 50.f };
    int closeIdx = ui_.pushInteractiveRect(close, [this]() { screen = Screen::Hud; });
    if (ui_.button("X", close, closeIdx, rgba(120,60,60,255), COL_WHITE)) {
        screen = Screen::Hud; return;
    }

    // Табы
    {
        const f32 tabW = 160.f;
        Rect buyTab{ 40.f, 74.f, tabW, 50.f };
        int bi = ui_.pushInteractiveRect(buyTab, [this]() { tradeCtx.tab = 0; });
        bool bp = ui_.isInteractivePressed(bi);
        ui_.rect(buyTab.x, buyTab.y, buyTab.w, buyTab.h,
                 (tradeCtx.tab == 0) ? rgba(120, 160, 120, 255)
                                     : (bp ? rgba(80,80,80,255) : rgba(40,40,40,220)));
        ui_.rectOutline(buyTab.x, buyTab.y, buyTab.w, buyTab.h, 2.f, COL_BLACK);
        ui_.text(T(StrKey::Trade_Buy), buyTab.x + 40.f, buyTab.y + 14.f,
                 2.f, COL_WHITE);

        Rect sellTab{ 210.f, 74.f, tabW, 50.f };
        int si = ui_.pushInteractiveRect(sellTab, [this]() { tradeCtx.tab = 1; });
        bool sp = ui_.isInteractivePressed(si);
        ui_.rect(sellTab.x, sellTab.y, sellTab.w, sellTab.h,
                 (tradeCtx.tab == 1) ? rgba(160, 120, 120, 255)
                                     : (sp ? rgba(80,80,80,255) : rgba(40,40,40,220)));
        ui_.rectOutline(sellTab.x, sellTab.y, sellTab.w, sellTab.h, 2.f, COL_BLACK);
        ui_.text(T(StrKey::Trade_Sell), sellTab.x + 40.f, sellTab.y + 14.f,
                 2.f, COL_WHITE);
    }

    // Золото
    {
        char goldBuf[32];
        wal->format(goldBuf, sizeof(goldBuf));
        float tw = ui_.textWidth(goldBuf, 2.f);
        ui_.text(goldBuf, (float)screenW_ - tw - 40.f, 24.f, 2.f,
                 rgba(255, 220, 100, 255));
    }

    const f32 listX = 40.f;
    const f32 listY = 140.f;

    if (tradeCtx.tab == 0) {
        ui_.text("(BUY list from trader)", listX, listY, 1.8f,
                 rgba(180, 180, 180, 255));
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
                              def.name, (unsigned)s.count);
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
    ui_.rect(0, 0, (float)screenW_, (float)screenH_, rgba(20, 10, 30, 240));

    ui_.text("ENCHANT ALTAR", (float)screenW_ * 0.36f, 24.f, 3.f, COL_WHITE);

    Rect close{ (float)screenW_ - 90.f, 74.f, 80.f, 50.f };
    int closeIdx = ui_.pushInteractiveRect(close, [this]() { screen = Screen::Hud; });
    if (ui_.button("X", close, closeIdx, rgba(120,60,60,255), COL_WHITE)) {
        screen = Screen::Hud; return;
    }

    auto* inv = player.inventory();
    auto* wal = player.wallet();
    if (!inv) return;

    // Текущее оружие
    {
        const auto& wdef = combat::weapons().get(player.equipped.weaponId);
        ui_.text("Equipped:", 40.f, 110.f, 1.8f, rgba(200, 200, 200, 255));
        ui_.text(wdef.name ? wdef.name : "-", 200.f, 110.f, 2.f, COL_WHITE);

        if (player.equipped.enchant.id != combat::EnchantmentId::None) {
            const char* en = combat::enchantmentName(player.equipped.enchant.id);
            char buf[64];
            std::snprintf(buf, sizeof(buf), "Enchant: %s Lv%u",
                          en, player.equipped.enchant.level);
            ui_.text(buf, 200.f, 140.f, 1.6f, rgba(180, 120, 240, 255));
        }
    }

    if (wal) {
        char gbuf[32];
        wal->format(gbuf, sizeof(gbuf));
        float tw = ui_.textWidth(gbuf, 2.f);
        ui_.text(gbuf, (float)screenW_ - tw - 40.f, 24.f, 2.f,
                 rgba(255, 220, 100, 255));
    }

    const auto& recipes = world::enchantRecipes().all();
    const f32 listX = 40.f;
    const f32 listY = 200.f;
    const f32 listW = (f32)screenW_ * 0.55f;
    const f32 rowH  = 70.f;
    const f32 rowGap = 8.f;
    i32 total = (i32)recipes.size();

    for (i32 i = 0; i < total; ++i) {
        const auto& r = recipes[(usize)i];
        f32 y = listY + i * (rowH + rowGap);
        if (y + rowH > (f32)screenH_ - 40.f) break;

        bool isSel = (enchantCtx.selectedIdx == i);
        UiColor bg = isSel ? rgba(120, 100, 60, 240) : rgba(50, 40, 60, 220);

        Rect rr{ listX, y, listW, rowH };
        int idx = ui_.pushInteractiveRect(rr, [this, i]() {
            enchantCtx.selectedIdx = i;
        });
        if (ui_.isInteractivePressed(idx)) bg = rgba(140, 120, 80, 255);

        ui_.rect(rr.x, rr.y, rr.w, rr.h, bg);
        ui_.rectOutline(rr.x, rr.y, rr.w, rr.h, 2.f, COL_BLACK);

        ui_.text(r.name ? r.name : "?", rr.x + 12.f, rr.y + 8.f, 2.f, COL_WHITE);

        char cost[64];
        std::snprintf(cost, sizeof(cost), "%u gold", r.goldCost);
        ui_.text(cost, rr.x + 12.f, rr.y + 36.f, 1.4f,
                 rgba(255, 220, 100, 255));

        char mats[192] = {};
        for (usize k = 0; k < r.materials.size(); ++k) {
            const auto& m = r.materials[k];
            const auto& md = items::items().get(m.itemId);
            char piece[64];
            std::snprintf(piece, sizeof(piece), "%sx%u ",
                          md.name, (unsigned)m.count);
            std::strncat(mats, piece, sizeof(mats) - std::strlen(mats) - 1);
        }
        ui_.text(mats, rr.x + 12.f, rr.y + 52.f, 1.3f,
                 rgba(200, 200, 200, 255));
    }

    // Правая панель — кнопка ENCHANT
    {
        const f32 pX = (f32)screenW_ * 0.62f;
        const f32 pY = 200.f;
        const f32 pW = (f32)screenW_ - pX - 40.f;
        const f32 pH = (f32)screenH_ - pY - 40.f;

        ui_.rect(pX, pY, pW, pH, rgba(30, 20, 40, 240));
        ui_.rectOutline(pX, pY, pW, pH, 2.f, COL_BLACK);

        if (enchantCtx.selectedIdx >= 0 &&
            enchantCtx.selectedIdx < (i32)total)
        {
            const auto& r = recipes[(usize)enchantCtx.selectedIdx];
            ui_.text(r.name ? r.name : "?", pX + 16.f, pY + 12.f, 2.2f, COL_WHITE);

            Rect cr{ pX + 16.f, pY + pH - 80.f, pW - 32.f, 60.f };
            int ci = ui_.pushInteractiveRect(cr, [this, i = enchantCtx.selectedIdx]() {
                if (onEnchant) onEnchant((u32)i);
            });
            bool cpress = ui_.isInteractivePressed(ci);
            UiColor c = cpress ? rgba(160, 80, 200, 255) : rgba(90, 40, 140, 255);
            ui_.rect(cr.x, cr.y, cr.w, cr.h, c);
            ui_.rectOutline(cr.x, cr.y, cr.w, cr.h, 2.f, COL_BLACK);
            float tw = ui_.textWidth("ENCHANT", 2.4f);
            ui_.text("ENCHANT", cr.x + (cr.w - tw) * 0.5f, cr.y + 16.f,
                     2.4f, COL_WHITE);
        } else {
            ui_.text("Select a recipe", pX + 16.f, pY + 40.f, 2.f,
                     rgba(180, 180, 180, 255));
        }
    }
}

// ============================================================
// Toast
// ============================================================
void UiSystem::drawStatusToast() {
    if (statusTimer <= 0.f || statusMessage.empty()) return;

    const f32 tw = ui_.textWidth(statusMessage, 2.f);
    const f32 bw = tw + 40.f;
    const f32 bh = 50.f;
    const f32 bx = ((f32)screenW_ - bw) * 0.5f;
    const f32 by = (f32)screenH_ * 0.85f;

    u8 alpha = (u8)(255.f * std::min(1.f, statusTimer));
    ui_.rect(bx, by, bw, bh, rgba(20, 20, 20, (u8)(alpha * 0.8f)));
    ui_.rectOutline(bx, by, bw, bh, 2.f, rgba(220, 200, 100, alpha));
    ui_.text(statusMessage, bx + 20.f, by + 14.f, 2.f,
             rgba(255, 255, 255, alpha));
}

void UiSystem::destroy() {
    minimap.destroy();
    renderer_.destroy();
}

} // namespace ui
