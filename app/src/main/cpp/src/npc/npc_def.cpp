/**
 * @file npc_def.cpp
 * @brief NPC: роли, диалоги с ветвлением, поведение жителей.
 */
#include "npc_def.h"
#include "../combat/weapon.h"
#include "../core/log.h"

namespace npc {

namespace {
constexpr u32 rgb(u8 r, u8 g, u8 b) {
    return ((u32)r << 24) | ((u32)g << 16) | ((u32)b << 8) | 0xFF;
}
}

NpcRegistry::NpcRegistry() {
    // ---------------- VILLAGER ----------------
    {
        NpcDef d{};
        d.name = "Villager";
        d.role = NpcRole::Villager;
        d.faction = factions::FactionId::Villagers;
        d.maxHealth = 20.f;
        d.moveSpeed = 1.8f;
        d.attackDamage = 0.f;
        d.attackRange = 0.f;
        d.aggroRange = 0.f;
        d.bodyRadius = 0.35f;
        d.bodyHeight = 1.75f;
        d.hostile = false;
        d.bodyColor   = rgb(150, 100, 70);
        d.headColor   = rgb(200, 160, 120);
        d.accentColor = rgb(80, 60, 40);
        d.dialogueRoot = "villager";
        defs_[NPC_VILLAGER] = d;
    }

    // ---------------- QUEST GIVER ----------------
    {
        NpcDef d{};
        d.name = "Elder";
        d.role = NpcRole::QuestGiver;
        d.faction = factions::FactionId::Villagers;
        d.maxHealth = 30.f;
        d.moveSpeed = 1.4f;
        d.attackDamage = 0.f;
        d.attackRange = 0.f;
        d.aggroRange = 0.f;
        d.bodyRadius = 0.35f;
        d.bodyHeight = 1.70f;
        d.hostile = false;
        d.bodyColor   = rgb(80, 60, 120);
        d.headColor   = rgb(200, 170, 140);
        d.accentColor = rgb(220, 180, 60);
        d.dialogueRoot = "elder";
        defs_[NPC_QUEST_GIVER] = d;
    }

    // ---------------- TRADER ----------------
    {
        NpcDef d{};
        d.name = "Trader";
        d.role = NpcRole::Trader;
        d.faction = factions::FactionId::Traders;
        d.maxHealth = 25.f;
        d.moveSpeed = 1.6f;
        d.attackDamage = 0.f;
        d.attackRange = 0.f;
        d.aggroRange = 0.f;
        d.bodyRadius = 0.35f;
        d.bodyHeight = 1.75f;
        d.hostile = false;
        d.bodyColor   = rgb(200, 140, 40);
        d.headColor   = rgb(220, 180, 140);
        d.accentColor = rgb(240, 200, 60);
        d.dialogueRoot = "trader";
        defs_[NPC_TRADER] = d;
    }

    // ---------------- BLACKSMITH ----------------
    {
        NpcDef d{};
        d.name = "Blacksmith";
        d.role = NpcRole::Blacksmith;
        d.faction = factions::FactionId::Villagers;
        d.maxHealth = 35.f;
        d.moveSpeed = 1.5f;
        d.attackDamage = 6.f;
        d.attackRange = 1.8f;
        d.aggroRange = 10.f;
        d.bodyRadius = 0.40f;
        d.bodyHeight = 1.85f;
        d.hostile = false;
        // Кузнец берётся за топор, когда в деревню лезут: он и так
        // весь день им машет.
        d.weaponId = combat::WEAPON_IRON_AXE;
        d.defender = true;
        d.bodyColor   = rgb(60, 60, 70);
        d.headColor   = rgb(200, 170, 130);
        d.accentColor = rgb(180, 100, 60);
        d.dialogueRoot = "blacksmith";
        defs_[NPC_BLACKSMITH] = d;
    }

    // ---------------- GUARD ----------------
    {
        NpcDef d{};
        d.name = "Guard";
        d.role = NpcRole::Guard;
        d.faction = factions::FactionId::Villagers;
        d.maxHealth = 45.f;
        d.moveSpeed = 2.5f;
        d.attackDamage = 7.f;
        d.attackRange = 2.0f;
        d.aggroRange = 18.f;
        d.bodyRadius = 0.40f;
        d.bodyHeight = 1.85f;
        d.hostile = false;
        // Меч. Стража стояла с пустыми руками и била «уроном 7» из
        // воздуха — ни замаха, ни отбрасывания, ни клинка на виду.
        d.weaponId = combat::WEAPON_IRON_SWORD;
        d.defender = true;
        d.bodyColor   = rgb(70, 90, 130);
        d.headColor   = rgb(200, 170, 130);
        d.accentColor = rgb(220, 220, 220);
        d.dialogueRoot = "guard";
        defs_[NPC_GUARD] = d;
    }

    // ---------------- HEALER ----------------
    {
        NpcDef d{};
        d.name = "Healer";
        d.role = NpcRole::Healer;
        d.faction = factions::FactionId::Mages;
        d.maxHealth = 22.f;
        d.moveSpeed = 1.6f;
        d.attackDamage = 0.f;
        d.attackRange = 0.f;
        d.aggroRange = 0.f;
        d.bodyRadius = 0.35f;
        d.bodyHeight = 1.72f;
        d.hostile = false;
        d.bodyColor   = rgb(200, 100, 180);
        d.headColor   = rgb(220, 190, 160);
        d.accentColor = rgb(120, 200, 240);
        d.dialogueRoot = "healer";
        defs_[NPC_HEALER] = d;
    }

    // ---------------- COURIER ----------------
    //
    // Ходит по дороге из деревни в деревню и потому быстрее прочих:
    // житель, бредущий со скоростью 1.8, преодолевал бы перегон в
    // двести пятьдесят блоков две с половиной минуты.
    {
        NpcDef d{};
        d.name = "Courier";
        d.role = NpcRole::Courier;
        d.faction = factions::FactionId::Traders;
        d.maxHealth = 24.f;
        d.moveSpeed = 3.2f;
        d.attackDamage = 0.f;
        d.attackRange = 0.f;
        d.aggroRange = 0.f;
        d.bodyRadius = 0.35f;
        d.bodyHeight = 1.74f;
        d.hostile = false;
        d.bodyColor   = rgb(120, 90, 60);
        d.headColor   = rgb(205, 165, 125);
        d.accentColor = rgb(220, 190, 90);
        d.dialogueRoot = "villager";
        defs_[NPC_COURIER] = d;
    }

    LOGI("NpcRegistry: %u NPC", (unsigned)NPC_COUNT - 1);
}

const NpcRegistry& NpcRegistry::instance() {
    static NpcRegistry r;
    return r;
}

const NpcDef& NpcRegistry::get(u16 id) const {
    if (id >= NPC_COUNT) return defs_[NPC_NONE];
    return defs_[id];
}

} // namespace npc
