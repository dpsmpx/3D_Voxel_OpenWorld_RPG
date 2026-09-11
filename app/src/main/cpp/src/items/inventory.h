#pragma once
#include "../core/types.h"
#include "item_stack.h"
#include <array>

namespace items {

// ============================================================
// Инвентарь:
//   - 27 основных слотов
//   -  9 слотов хотбара
//   -  4 слота экипировки (weapon/head/chest/legs) — Phase 13
//   -  1 слот аксессуара
// Итого 41 слот.
// ============================================================
constexpr u32 INV_MAIN_SLOTS    = 27;
constexpr u32 INV_HOTBAR_SLOTS  =  9;
constexpr u32 INV_ARMOR_SLOTS   =  4;
constexpr u32 INV_ACC_SLOTS     =  1;

constexpr u32 INV_MAIN_OFFSET   = 0;
constexpr u32 INV_HOTBAR_OFFSET = INV_MAIN_SLOTS;
constexpr u32 INV_ARMOR_OFFSET  = INV_HOTBAR_OFFSET + INV_HOTBAR_SLOTS;
constexpr u32 INV_ACC_OFFSET    = INV_ARMOR_OFFSET + INV_ARMOR_SLOTS;
constexpr u32 INV_TOTAL_SLOTS   = INV_ACC_OFFSET + INV_ACC_SLOTS;  // 41

// Индекс активного слота хотбара: 0..8
using HotbarIndex = u8;

// ============================================================
// Результат попытки добавить предмет. Может быть частичным.
// ============================================================
struct AddResult {
    u16 added  = 0;    // сколько реально добавлено
    u16 leftover = 0;  // сколько не влезло

    bool full()      const { return leftover == 0; }
    bool noneAdded() const { return added == 0; }
};

// ============================================================
// Inventory — компонент ECS, но также самостоятельная структура.
// Все операции — потокобезопасно только через внешний mutex
// (в игровом потоке).
// ============================================================
struct Inventory {
    std::array<ItemStack, INV_TOTAL_SLOTS> slots{};
    HotbarIndex activeHotbar = 0;

    // ---- Прямой доступ к слоту ----
    ItemStack&       at(u32 index)       { return slots[index]; }
    const ItemStack& at(u32 index) const { return slots[index]; }

    // ---- Хотбар ----
    ItemStack&       hotbarSlot(u32 i)       { return slots[INV_HOTBAR_OFFSET + i]; }
    const ItemStack& hotbarSlot(u32 i) const { return slots[INV_HOTBAR_OFFSET + i]; }
    ItemStack&       activeSlot()            { return hotbarSlot(activeHotbar); }
    const ItemStack& activeSlot() const      { return hotbarSlot(activeHotbar); }

    // ---- Основные операции ----
    // Добавить стек. Возвращает, сколько влезло и сколько осталось.
    AddResult addStack(const ItemStack& stack);

    // Добавить N одного и того же предмета.
    AddResult addItem(u16 itemId, u16 count);

    // Удалить N предметов с этого itemId. Возвращает,
    // сколько реально удалено.
    u16 removeItem(u16 itemId, u16 count);

    // Удалить с конкретного слота N штук.
    u16 removeFromSlot(u32 slotIndex, u16 count);

    // Обменять содержимое двух слотов.
    void swapSlots(u32 a, u32 b);

    // Разбить слот: переложить половину в свободное место.
    // (Shift+click в UI). Возвращает true, если успешно.
    bool splitStack(u32 slotIndex);

    // Взять полностью стек из слота (для курсора drag-and-drop).
    // Возвращает содержимое, слот становится пустым.
    ItemStack takeStack(u32 slotIndex);

    // Положить стек в слот. Возвращает leftover (сколько
    // не влезло). Если слот не пуст и не совместим — не трогаем.
    AddResult putStack(u32 slotIndex, const ItemStack& stack);

    // ---- Поиск / количество ----
    u32  countOf(u16 itemId) const;
    bool has(u16 itemId, u16 amount) const;
    bool hasSpaceFor(u16 itemId, u16 amount) const;

    // Найти первый слот с этим itemId. Возвращает -1, если нет.
    i32  findItem(u16 itemId) const;

    // Найти первый пустой слот. -1, если нет.
    i32  findEmpty() const;

    // ---- Массовые операции ----
    void clearAll();

    // Очистить только хотбар (не влияет на основные слоты).
    void clearHotbar();

    bool isEmpty() const;

    // ---- Сортировка ----
    // Сортирует основные слоты (не хотбар) по: категория → itemId.
    void sortMain();

    // ---- Сводки для UI ----
    u32 totalItemCount() const;      // сумма всех count
    u32 usedSlotCount()  const;      // сколько слотов занято
    u32 totalInventoryValue() const; // суммарная стоимость
};

} // namespace items
