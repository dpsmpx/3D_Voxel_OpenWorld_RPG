#pragma once
#include "../core/types.h"
#include <glm/glm.hpp>

namespace world {

// ============================================================
// Биом определяется по климату (температура, влажность) и высоте.
// Плавная интерполяция между соседними биомами через blendedHeight.
// ============================================================
enum BiomeId : u8 {
    Ocean = 0,
    Beach,
    Plains,
    Forest,
    Taiga,
    Desert,
    Savanna,
    Tundra,
    Mountains,
    Swamp,
    Volcanic,
    BIOME_COUNT
};

enum class TreeType : u8 {
    None,
    Oak,       // широкий blob
    Pine,      // конус
    Palm,      // зонтик
    Cactus,    // без листвы
    Dead       // сухое дерево
};

struct BiomeDef {
    const char* name;
    u16 surfaceBlock;
    u16 subsurfaceBlock;    // 2-4 блока под поверхностью
    u16 stoneBlock;
    u16 liquidBlock;        // вода/лава/нет
    f32 treeDensity;        // деревьев на чанк (в среднем)
    TreeType treeType;
    f32 grassDensity;       // инстансов травы на чанк
    i8 baseHeightOffset;    // смещение базовой высоты (может быть ниже базы)
    f32 temperature;        // справочная
};

// ============================================================
// BiomeField — климатические поля через Simplex-шум.
// Кэшируем последний запрос — генерация идёт построчно по (x,z),
// а y не влияет на биом, кроме высоты.
// ============================================================
class SimplexNoise;  // fwd

class BiomeField {
public:
    explicit BiomeField(u64 seed);

    struct Sample {
        BiomeId biome;
        f32     temperature;   // -1..1
        f32     humidity;      // -1..1
        f32     continent;     // -1..1 (океан → суша)
        f32     erosion;       //  0..1 (0 = крутые горы, 1 = равнина)
        f32     peaks;         //  0..1
        f32     heightMod;     // модификатор высоты (-20..+40)
    };

    // Основной запрос: по мировым (x,z) и базовой высоте возвращает
    // биом и климатические параметры. Высота используется для
    // падения температуры с высотой и перехода в Mountains.
    Sample sample(i32 x, i32 z, i32 surfaceY) const;

    const BiomeDef& def(BiomeId b) const;

private:
    struct Impl;
    Impl* impl_;   // PIMPL чтобы не тащить шум в заголовок
};

} // namespace world
