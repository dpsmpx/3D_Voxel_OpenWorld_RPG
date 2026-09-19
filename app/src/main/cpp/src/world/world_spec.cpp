/**
 * @file world_spec.cpp
 * @brief Мир: чем один мир отличается от другого — имя и зерно.
 */
#include "world_spec.h"
#include <chrono>
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

} // namespace world
