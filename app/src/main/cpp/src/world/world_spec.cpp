/**
 * @file world_spec.cpp
 * @brief Мир: чем один мир отличается от другого — имя и зерно.
 */
#include "world_spec.h"
#include <chrono>
#include <cstdio>
#include <random>

namespace world {

u64 randomWorldSeed() {
    // random_device на Android читает /dev/urandom, но стандарт этого
    // не обещает: реализация вправе отдавать одну и ту же
    // последовательность. Поэтому к нему домешиваются часы — их-то
    // два запуска подряд точно разведут.
    std::random_device rd;
    u64 s = ((u64)rd() << 32) ^ (u64)rd();

    const auto now = std::chrono::steady_clock::now().time_since_epoch();
    const auto wall = std::chrono::system_clock::now().time_since_epoch();
    s ^= (u64)std::chrono::duration_cast<std::chrono::nanoseconds>(now).count();
    s += (u64)std::chrono::duration_cast<std::chrono::nanoseconds>(wall).count();

    // Перемешать: у часов меняются младшие биты, и без этого два
    // соседних запуска дали бы почти одинаковые зёрна, а рельеф
    // рядом стоящих зёрен различается не так сильно, как хотелось бы.
    s ^= s >> 33; s *= 0xFF51AFD7ED558CCDull;
    s ^= s >> 33; s *= 0xC4CEB9FE1A85EC53ull;
    s ^= s >> 33;
    return s;
}

u64 seedFromText(const char* text) {
    if (!text || text[0] == '\0') return 0;

    // Сперва честная попытка прочесть число. Пробелы по краям — не
    // повод отказать: их оставляет сама клавиатура.
    usize i = 0;
    while (text[i] == ' ') ++i;
    bool neg = false;
    if (text[i] == '-') { neg = true; ++i; }
    const usize digitsStart = i;

    u64 num = 0;
    bool allDigits = text[i] != '\0';
    for (; text[i] != '\0'; ++i) {
        if (text[i] == ' ') {
            // Хвостовые пробелы допустимы, внутренние — нет.
            usize j = i;
            while (text[j] == ' ') ++j;
            if (text[j] != '\0') allDigits = false;
            break;
        }
        if (text[i] < '0' || text[i] > '9') { allDigits = false; break; }
        num = num * 10u + (u64)(text[i] - '0');
    }
    if (allDigits && i > digitsStart)
        return neg ? (u64)(-(i64)num) : num;

    // Не число — хэш. FNV-1a по БАЙТАМ: кириллица двухбайтовая, и
    // посимвольный разбор здесь ничего бы не дал, кроме лишнего кода.
    u64 h = 0xCBF29CE484222325ull;
    for (usize k = 0; text[k] != '\0'; ++k) {
        h ^= (u64)(u8)text[k];
        h *= 0x100000001B3ull;
    }
    return h;
}

void defaultWorldName(char* out, u32 cap, u64 seed) {
    if (!out || cap == 0) return;
    // Шестнадцатеричный хвост зерна: он короткий, он разный у разных
    // миров, и по нему мир узнаётся в списке.
    std::snprintf(out, cap, "Мир %08llX",
                  (unsigned long long)(seed & 0xFFFFFFFFull));
}

} // namespace world
