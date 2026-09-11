#pragma once
#include "../core/types.h"

namespace items {

// ============================================================
// Компонент «кошелёк». У игрока — один.
// У торговцев — тоже есть запас золота, для покупки у игрока.
// ============================================================
struct Wallet {
    u64 gold = 0;

    bool canAfford(u64 amount) const { return gold >= amount; }

    // Возвращает true, если списание прошло.
    bool spend(u64 amount) {
        if (amount > gold) return false;
        gold -= amount;
        return true;
    }

    void receive(u64 amount) {
        gold += amount;
    }

    // Форматирование для UI: «1 234 g» или «12 345 g».
    void format(char* buf, usize bufSize) const;
};

} // namespace items
