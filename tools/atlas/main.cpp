/**
 * @file main.cpp
 * @brief Сборка атласа блоков в ASTC на этапе сборки проекта.
 *
 * Атлас генерируется тем же кодом, что и на устройстве
 * (render::buildProceduralAtlas), поэтому картинка идентична.
 * Утилита пишет его в .tga, а сжатие в ASTC делает astcenc —
 * своего кодировщика в проекте нет и быть не должно.
 *
 * Собирается и запускается из build.sh; если astcenc не установлен,
 * шаг пропускается и игра соберёт атлас процедурно во время старта.
 */
#include "render/atlas_builder.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace {

/// Пишет несжатый 32-битный TGA — формат, который astcenc читает
/// без дополнительных зависимостей.
bool writeTga(const char* path, const render::AtlasData& a) {
    std::FILE* f = std::fopen(path, "wb");
    if (!f) {
        std::fprintf(stderr, "atlas: не открыть %s\n", path);
        return false;
    }

    unsigned char header[18] = {};
    header[2]  = 2;                                   // без сжатия, truecolor
    header[12] = (unsigned char)(a.width  & 0xFF);
    header[13] = (unsigned char)((a.width  >> 8) & 0xFF);
    header[14] = (unsigned char)(a.height & 0xFF);
    header[15] = (unsigned char)((a.height >> 8) & 0xFF);
    header[16] = 32;                                  // бит на пиксель
    header[17] = 0x28;                                // альфа 8 бит, начало сверху
    std::fwrite(header, 1, sizeof(header), f);

    // TGA хранит BGRA, атлас — RGBA.
    std::vector<unsigned char> row(a.width * 4);
    for (unsigned y = 0; y < a.height; ++y) {
        const unsigned char* src = a.pixels.data() + (size_t)y * a.width * 4;
        for (unsigned x = 0; x < a.width; ++x) {
            row[x * 4 + 0] = src[x * 4 + 2];
            row[x * 4 + 1] = src[x * 4 + 1];
            row[x * 4 + 2] = src[x * 4 + 0];
            row[x * 4 + 3] = src[x * 4 + 3];
        }
        std::fwrite(row.data(), 1, row.size(), f);
    }
    std::fclose(f);
    return true;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr,
            "Использование: atlas_tool <выходной .tga> [выходной .astc]\n");
        return 2;
    }

    const render::AtlasData atlas = render::buildProceduralAtlas();
    std::printf("atlas: %ux%u, тайл %u, %zu КБ RGBA8\n",
                atlas.width, atlas.height, atlas.tileSize,
                atlas.pixels.size() / 1024);

    if (!writeTga(argv[1], atlas)) return 1;
    std::printf("atlas: записан %s\n", argv[1]);

    if (argc < 3) return 0;

    // Имя кодировщика приходит третьим аргументом: официальные сборки
    // astcenc называются по набору инструкций (astcenc-neon,
    // astcenc-sse2, astcenc-avx2), и зашитое astcenc-native на aarch64
    // не находится никогда.
    const char* enc = (argc > 3 && argv[3][0]) ? argv[3] : "astcenc";

    // Сжатие: -exhaustive заметно медленнее, но атлас собирается
    // один раз на сборку, а качество тайлов важно — они видны вблизи.
    std::string cmd = "\"";
    cmd += enc;
    cmd += "\" -cl \"";
    cmd += argv[1];
    cmd += "\" \"";
    cmd += argv[2];
    cmd += "\" 4x4 -thorough";
    std::printf("atlas: %s\n", cmd.c_str());

    const int rc = std::system(cmd.c_str());
    if (rc != 0) {
        std::fprintf(stderr,
            "atlas: astcenc вернул %d — ASTC-атлас не создан.\n"
            "       Игра соберёт атлас процедурно при старте.\n", rc);
        return 0;   // не фатально
    }
    std::printf("atlas: записан %s\n", argv[2]);
    return 0;
}
