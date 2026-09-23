/**
 * @file save_inventory.cpp
 * @brief Сохранения: бинарный формат, сжатие, дельты мира, слоты.
 */
#include "save_inventory.h"
#include "../ecs/components.h"
#include "../combat/components.h"
#include "../core/log.h"

#include <vector>

namespace save {

using namespace ecs;

void serializeInventory(ByteWriter& w, ecs::Registry& reg, ecs::Entity player) {
    // ---- Inventory ----
    if (auto* inv = reg.get<items::Inventory>(player)) {
        w.writeU8(1);
        w.writeU8(inv->activeHotbar);

        // Все слоты сериализуем подряд.
        for (u32 i = 0; i < items::INV_TOTAL_SLOTS; ++i) {
            const auto& s = inv->at(i);
            if (s.empty()) {
                w.writeU8(0);
                continue;
            }
            w.writeU8(1);
            w.writeU16(s.itemId);
            w.writeU16(s.count);
            w.writeU8((u8)s.enchant.id);
            w.writeU8(s.enchant.level);
        }
    } else w.writeU8(0);

    // ---- Wallet ----
    if (auto* wal = reg.get<items::Wallet>(player)) {
        w.writeU8(1);
        w.writeU64(wal->gold);
    } else w.writeU8(0);

    // ---- Equipped weapon (дублируется для надёжности) ----
    if (auto* eq = reg.get<combat::EquippedWeapon>(player)) {
        w.writeU8(1);
        w.writeU16(eq->weaponId);
        w.writeU8((u8)eq->enchant.id);
        w.writeU8(eq->enchant.level);
    } else w.writeU8(0);
}

bool deserializeInventory(ByteReader& r, ecs::Registry& reg, ecs::Entity player) {
    // ---- Inventory ----
    {
        u8 has = 0;
        if (!r.u8v(has)) return false;
        if (has) {
            auto* inv = reg.get<items::Inventory>(player);
            if (!inv) {
                items::Inventory tmp;
                reg.add(player, tmp);
                inv = reg.get<items::Inventory>(player);
            }
            if (inv) {
                u8 active = 0;
                if (!r.u8v(active)) return false;
                inv->activeHotbar = active;
                if (inv->activeHotbar >= items::INV_HOTBAR_SLOTS)
                    inv->activeHotbar = 0;

                for (u32 i = 0; i < items::INV_TOTAL_SLOTS; ++i) {
                    u8 present = 0;
                    if (!r.u8v(present)) return false;
                    auto& s = inv->at(i);
                    if (!present) { s.clear(); continue; }

                    u16 id = 0, cnt = 0;
                    u8 enchId = 0, enchLvl = 0;
                    if (!r.u16v(id))    return false;
                    if (!r.u16v(cnt))   return false;
                    if (!r.u8v(enchId)) return false;
                    if (!r.u8v(enchLvl))return false;

                    s.itemId = id;
                    s.count  = cnt;
                    s.enchant.id    = (combat::EnchantmentId)enchId;
                    s.enchant.level = enchLvl;
                }
            }
        }
    }

    // ---- Wallet ----
    {
        u8 has = 0;
        if (!r.u8v(has)) return false;
        if (has) {
            auto* wal = reg.get<items::Wallet>(player);
            if (!wal) {
                items::Wallet tmp;
                reg.add(player, tmp);
                wal = reg.get<items::Wallet>(player);
            }
            if (wal) {
                u64 g = 0;
                if (!r.u64v(g)) return false;
                wal->gold = g;
            }
        }
    }

    // ---- Equipped weapon ----
    {
        u8 has = 0;
        if (!r.u8v(has)) return false;
        if (has) {
            auto* eq = reg.get<combat::EquippedWeapon>(player);
            if (!eq) {
                combat::EquippedWeapon tmp;
                reg.add(player, tmp);
                eq = reg.get<combat::EquippedWeapon>(player);
            }
            if (eq) {
                u16 wid = 0;
                u8 enchId = 0, enchLvl = 0;
                r.u16v(wid);
                r.u8v(enchId);
                r.u8v(enchLvl);
                eq->weaponId = wid;
                eq->enchant.id    = (combat::EnchantmentId)enchId;
                eq->enchant.level = enchLvl;
            }
        }
    }

    return r.ok();
}

void serializePickups(ByteWriter& w, ecs::Registry& reg) {
    auto& pool = reg.pool<items::ItemPickup>();

    // Сначала считаем, потом пишем: цикл ниже пропускает предметы без
    // положения в мире, а число записей уже было бы объявлено. Читатель
    // верит числу — и вычитывает за границу своих данных.
    u32 count = 0;
    for (usize i = 0; i < pool.size(); ++i) {
        ecs::Entity e = pool.entityAt((u32)i);
        if (pool.get(e) && reg.get<Transform>(e)) ++count;
    }
    w.varU32(count);

    for (usize i = 0; i < pool.size(); ++i) {
        ecs::Entity e = pool.entityAt((u32)i);
        auto* p  = pool.get(e);
        auto* tf = reg.get<Transform>(e);
        if (!p || !tf) continue;

        w.writeF32(tf->position.x);
        w.writeF32(tf->position.y);
        w.writeF32(tf->position.z);
        w.writeU16(p->stack.itemId);
        w.writeU16(p->stack.count);
        w.writeF32(p->lifeRemaining);
        w.writeU64(p->currencyAmount);
        w.writeU8(p->waits ? 1 : 0);
    }
}

bool deserializePickups(ByteReader& r, ecs::Registry& reg) {
    struct SavedPickup {
        glm::vec3 position{0};
        u16 id = 0;
        u16 count = 0;
        f32 life = 300.f;
        u64 currencyAmount = 0;
        bool waits = false;
    };

    u32 count = 0;
    if (!r.varU32v(count)) return false;
    if (count > 2000) return false;
    std::vector<SavedPickup> saved;
    saved.reserve(count);

    // Полностью разбираем вход до изменения live Registry. Повреждённый
    // сейв не должен уничтожать уже лежащие предметы.
    for (u32 i = 0; i < count; ++i) {
        SavedPickup sp;
        if (!r.f32v(sp.position.x)) return false;
        if (!r.f32v(sp.position.y)) return false;
        if (!r.f32v(sp.position.z)) return false;
        if (!r.u16v(sp.id)) return false;
        if (!r.u16v(sp.count)) return false;
        if (!r.f32v(sp.life)) return false;
        if (!r.u64v(sp.currencyAmount)) return false;
        u8 waits = 0;
        if (!r.u8v(waits)) return false;
        sp.waits = waits != 0;
        if (sp.id == 0 || sp.count == 0) return false;
        if (sp.id == items::ITEM_GOLD_COIN && sp.currencyAmount == 0) return false;
        saved.push_back(sp);
    }

    auto& pool = reg.pool<items::ItemPickup>();
    std::vector<ecs::Entity> toRemove;
    toRemove.reserve(pool.size());
    for (usize i = 0; i < pool.size(); ++i)
        toRemove.push_back(pool.entityAt((u32)i));
    for (auto e : toRemove) reg.destroy(e);

    for (const auto& sp : saved) {
        items::ItemStack s;
        s.itemId = sp.id;
        s.count = sp.count;
        ecs::Entity e = items::spawnPickup(reg, sp.position, s);
        if (auto* p = reg.get<items::ItemPickup>(e)) {
            p->lifeRemaining = sp.life;
            p->currencyAmount = sp.currencyAmount;
            p->waits = sp.waits;
            p->pickDelay = 0.f;
        }
    }
    return true;
}

} // namespace save