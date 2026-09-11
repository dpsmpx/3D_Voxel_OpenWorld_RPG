#include "save_inventory.h"
#include "../ecs/components.h"
#include "../combat/components.h"
#include "../core/log.h"

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
    w.varU32((u32)pool.size());

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
    }
}

bool deserializePickups(ByteReader& r, ecs::Registry& reg) {
    // Удаляем существующие пикапы.
    {
        auto& pool = reg.pool<items::ItemPickup>();
        std::vector<ecs::Entity> toRemove;
        for (usize i = 0; i < pool.size(); ++i) {
            toRemove.push_back(pool.entityAt((u32)i));
        }
        for (auto e : toRemove) reg.destroy(e);
    }

    u32 count = 0;
    if (!r.varU32v(count)) return false;
    if (count > 2000) return false;

    for (u32 i = 0; i < count; ++i) {
        f32 x = 0, y = 0, z = 0;
        u16 id = 0, cnt = 0;
        f32 life = 300.f;
        if (!r.f32v(x)) return false;
        if (!r.f32v(y)) return false;
        if (!r.f32v(z)) return false;
        if (!r.u16v(id)) return false;
        if (!r.u16v(cnt)) return false;
        if (!r.f32v(life)) return false;

        items::ItemStack s;
        s.itemId = id;
        s.count  = cnt;
        if (s.empty()) continue;

        ecs::Entity e = items::spawnPickup(reg, glm::vec3(x, y, z), s);
        if (auto* p = reg.get<items::ItemPickup>(e)) {
            p->lifeRemaining = life;
            p->pickDelay = 0.f;
        }
    }
    return true;
}

} // namespace save
