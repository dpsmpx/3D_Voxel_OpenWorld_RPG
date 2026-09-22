/**
 * @file currency.cpp
 * @brief Предметы: определения, инвентарь, лут, подбор, использование.
 */
#include "currency.h"
#include "../config/localization.h"
#include <cstdio>

namespace items {

void Wallet::format(char* buf, usize bufSize) const {
    if (!buf || bufSize == 0) return;

    // Разбиваем число на группы по 3 с пробелом.
    char tmp[32];
    std::snprintf(tmp, sizeof(tmp), "%llu", (unsigned long long)gold);

    usize len = 0;
    while (tmp[len] && len < sizeof(tmp) - 1) ++len;

    usize out = 0;
    for (usize i = 0; i < len && out + 2 < bufSize; ++i) {
        if (i > 0 && ((len - i) % 3) == 0) {
            buf[out++] = ' ';
        }
        buf[out++] = tmp[i];
    }
    // Значок валюты — из словаря: в русской игре кошелёк писал
    // «600 g».
    const char* mark = config::tr("g");
    if (out + 1 < bufSize) buf[out++] = ' ';
    for (const char* m = mark; *m && out + 1 < bufSize; ++m) buf[out++] = *m;
    buf[out] = '\0';
}

} // namespace items
