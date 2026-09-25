/**
 * @file item_use.cpp
 * @brief Предметы: определения, инвентарь, лут, подбор, использование.
 */
#include "item_use.h"
#include "inventory.h"
#include "item_pickup.h"
#include "throwable.h"
#include "item_def.h"
#include "../combat/components.h"
#include "../combat/weapon.h"
#include "../progression/progression.h"
#include "../progression/resource_regen.h"
#include "../ecs/components.h"
#include "../core/log.h"
#include <glm/glm.hpp>
#include <cmath>
#include <algorithm>

namespace items {

using namespace ecs;

const char* useResultString(UseResult r) {
    switch (r) {
        case UseResult::Ok:             return "OK";
        case UseResult::NotUsable:      return "Not usable";
        case UseResult::NoEffect:       return "No effect";
        case UseResult::Consumed:       return "Consumed";
        case UseResult::Equipped:       return "Equipped";
        case UseResult::AlreadyEquipped:return "Already equipped";
        case UseResult::Failed:         return "Failed";
    }
    return "?";
}

namespace {

// Применить зелье/еду. Возвращает true, если был эффект.
bool applyConsumable(ecs::Registry& reg, ecs::Entity player, const ItemDef& def) {
    bool didSomething = false;

    // Узел «Alchemist» считался в potionPowerMult и не доходил сюда:
    // зелье восстанавливало ровно то, что записано в предмете, сколько
    // очков в алхимию ни вложи. Еда идёт тем же путём — это тот же
    // расходник, и делить их правилом «алхимия только на склянки»
    // значило бы завести второе место, где решается одно и то же.
    const f32 power = progression::derivedOf(reg, player).potionPowerMult;

    if (def.restoreHealth > 0.f) {
        auto* h = reg.get<Health>(player);
        if (h && h->current < h->max) {
            h->current = std::min(h->max, h->current + def.restoreHealth * power);
            didSomething = true;
        }
    }
    if (def.restoreMana > 0.f) {
        auto* m = reg.get<Mana>(player);
        if (m && m->current < m->max) {
            m->current = std::min(m->max, m->current + def.restoreMana * power);
            didSomething = true;
        }
    }
    if (def.restoreStamina > 0.f) {
        auto* s = reg.get<Stamina>(player);
        if (s && s->current < s->max) {
            s->current = std::min(s->max, s->current + def.restoreStamina * power);
            didSomething = true;
        }
    }

    // Эликсир: временная прибавка к атрибуту. До сих пор четыре
    // эликсира не делали ничего — ни восстановления, ни эффекта, —
    // и даже не тратились: applyConsumable возвращал «эффекта нет».
    if (def.buffAttr != BuffAttr::None && def.effectDuration > 0.f) {
        auto* b = reg.get<progression::AttributeBuffs>(player);
        if (!b) {
            reg.add(player, progression::AttributeBuffs{});
            b = reg.get<progression::AttributeBuffs>(player);
        }
        if (b) {
            // Алхимия тянет и эликсиры: она про силу зелья вообще, а
            // не про то, восстанавливает оно или усиливает.
            const i32 amount = (i32)((f32)def.buffAmount * power + 0.5f);
            b->apply(def.buffAttr, amount, def.effectDuration);
            if (auto* prog = reg.get<progression::Progression>(player))
                prog->derivedDirty = true;
            didSomething = true;
        }
    }

    return didSomething;
}

} // namespace

UseResult useItemFromSlot(ecs::Registry& reg,
                          ecs::Entity playerEntity,
                          u32 slotIndex)
{
    auto* inv = reg.get<Inventory>(playerEntity);
    if (!inv) return UseResult::Failed;
    if (slotIndex >= INV_TOTAL_SLOTS) return UseResult::Failed;

    auto& s = inv->at(slotIndex);
    if (s.empty()) return UseResult::Failed;

    const ItemDef& def = items().get(s.itemId);
    if (def.category == ItemCategory::Block) {
        // Блоки ставятся в мир, не «используются» через этот API.
        return UseResult::NotUsable;
    }

    // ---- Оружие ----
    if (def.category == ItemCategory::Weapon) {
        auto* eq = reg.get<combat::EquippedWeapon>(playerEntity);
        if (!eq) {
            combat::EquippedWeapon newEq;
            reg.add(playerEntity, newEq);
            eq = reg.get<combat::EquippedWeapon>(playerEntity);
        }
        if (!eq) return UseResult::Failed;

        // Уже экипировано?
        if (eq->weaponId == def.payload.weaponId &&
            eq->enchant.id == s.enchant.id &&
            eq->enchant.level == s.enchant.level)
        {
            return UseResult::AlreadyEquipped;
        }

        // Меняем: старое оружие возвращаем в инвентарь.
        u16 oldWeaponId = eq->weaponId;
        combat::Enchantment oldEnch = eq->enchant;

        eq->weaponId = def.payload.weaponId;
        eq->enchant  = s.enchant;

        // Слот становится пустым, старое оружие — в тот же слот.
        s.clear();
        if (oldWeaponId != 0) {
            // Найдём item id по weapon id через item_def.
            // Быстрый поиск: перебираем все предметы категории Weapon.
            u16 oldItemId = 0;
            for (u16 i = 1; i < ITEM_MAX_DEFS; ++i) {
                const auto& d = items().get(i);
                if (d.category == ItemCategory::Weapon &&
                    d.payload.weaponId == oldWeaponId)
                {
                    oldItemId = i;
                    break;
                }
            }
            if (oldItemId != 0) {
                ItemStack back;
                back.itemId = oldItemId;
                back.count  = 1;
                back.enchant = oldEnch;
                auto res = inv->putStack(slotIndex, back);
                if (res.leftover > 0) {
                    // Куда-то в другое место
                    inv->addStack(back);
                }
            }
        }

        return UseResult::Equipped;
    }

    // ---- Зелья / Еда ----
    if (def.category == ItemCategory::Potion ||
        def.category == ItemCategory::Food)
    {
        bool did = applyConsumable(reg, playerEntity, def);
        if (!did) return UseResult::NoEffect;

        // Списать 1
        s.count -= 1;
        if (s.count == 0) s.clear();
        return UseResult::Consumed;
    }

    // ---- Метательное: бросают, а не «используют» ----
    //
    // Тап по предмету в поясе не должен швырять батут под ноги:
    // бросок идёт по направлению взгляда и через throwItemFromSlot.
    if (def.category == ItemCategory::Throwable) return UseResult::NotUsable;

    // ---- Материалы / Key / Currency — не используются ----
    return UseResult::NotUsable;
}

UseResult throwItemFromSlot(ecs::Registry& reg,
                            world::ChunkManager& world,
                            ecs::Entity playerEntity,
                            u32 slotIndex,
                            const glm::vec3& origin,
                            const glm::vec3& dir)
{
    auto* inv = reg.get<Inventory>(playerEntity);
    if (!inv) return UseResult::Failed;
    if (slotIndex >= INV_TOTAL_SLOTS) return UseResult::Failed;

    auto& s = inv->at(slotIndex);
    if (s.empty()) return UseResult::Failed;

    const ItemDef& def = items().get(s.itemId);
    if (def.category != ItemCategory::Throwable) return UseResult::NotUsable;

    bool thrown = false;
    switch (def.throwKind) {
        case ThrowKind::Trampoline:
            thrown = throwTrampoline(reg, world, origin, dir).valid();
            break;
        case ThrowKind::Shuriken:
            thrown = throwShuriken(reg, playerEntity, origin, dir).valid();
            break;
        default:
            break;
    }
    // Бросать некуда — предмет остаётся в поясе. Иначе батут,
    // кинутый в пропасть, просто исчезал бы из рук.
    if (!thrown) return UseResult::NoEffect;

    s.count -= 1;
    if (s.count == 0) s.clear();
    return UseResult::Consumed;
}

bool dropItemFromSlot(ecs::Registry& reg,
                      ecs::Entity playerEntity,
                      u32 slotIndex,
                      const glm::vec3& worldPos,
                      const glm::vec3& dir)
{
    auto* inv = reg.get<Inventory>(playerEntity);
    if (!inv) return false;
    if (slotIndex >= INV_TOTAL_SLOTS) return false;

    auto& s = inv->at(slotIndex);
    if (s.empty()) return false;
    // Руну навыка из рук не выпускают: она не вещь, а знание.
    if (items().get(s.itemId).bound) return false;

    ItemStack out = s;
    s.clear();

    glm::vec3 d = dir;
    if (glm::length(d) < 1e-4f) d = glm::vec3(0, 0, 1);
    else d = glm::normalize(d);

    glm::vec3 v = d * 4.5f + glm::vec3(0, 3.0f, 0);
    spawnPickup(reg, worldPos + d * 0.5f, out, v);
    return true;
}

} // namespace items
