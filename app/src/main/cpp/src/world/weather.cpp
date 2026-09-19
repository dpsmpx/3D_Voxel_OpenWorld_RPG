/**
 * @file weather.cpp
 * @brief Мир: погода — циклоны, тучи, осадки, радуга.
 */
#include "weather.h"
#include <cmath>
#include <algorithm>

namespace world {

namespace {

/// Быстрый хэш трёх чисел. Тот же приём, что у структур мира:
/// погода обязана быть одинаковой у всех, кто её спросит, и в любом
/// порядке, поэтому случайность берётся из координат, а не из
/// счётчика.
u32 hash3(i32 a, i32 b, i64 c, u64 seed) {
    u64 h = (u64)(u32)a * 0x9E3779B97F4A7C15ull;
    h ^= (u64)(u32)b * 0xC2B2AE3D27D4EB4Full;
    h ^= (u64)c       * 0x165667B19E3779F9ull;
    h ^= seed;
    h ^= h >> 33; h *= 0xFF51AFD7ED558CCDull;
    h ^= h >> 29; h *= 0xC4CEB9FE1A85EC53ull;
    h ^= h >> 32;
    return (u32)h;
}

/// Дробь 0..1 из хэша по выбранным битам.
f32 frac(u32 h, u32 shift) {
    return (f32)((h >> shift) & 0xFFFFu) / 65535.f;
}

/// Плавная ступенька, как в шейдерах.
f32 smoothStep(f32 e0, f32 e1, f32 x) {
    if (e1 <= e0) return x < e0 ? 0.f : 1.f;
    const f32 t = glm::clamp((x - e0) / (e1 - e0), 0.f, 1.f);
    return t * t * (3.f - 2.f * t);
}

/// Значение шума в узле решётки, 0..1.
f32 lattice(i32 x, i32 z, i64 t, u64 seed) {
    return frac(hash3(x, z, t, seed ^ 0xC10D5ull), 8);
}

/// Медленное поле облачности, не связанное с циклонами.
///
/// Без него пасмурного дня не бывает вовсе: тучи есть только там,
/// где циклон, а между циклонами всегда безоблачно. А пасмурно
/// бывает и просто так.
f32 backgroundCloud(i32 wx, i32 wz, f64 timeSec, u64 seed) {
    constexpr f32 SPACE = 6000.f;     // блоков на клетку шума
    constexpr f64 TIME  = 2400.0;     // секунд на шаг во времени

    const f32 fx = (f32)wx / SPACE;
    const f32 fz = (f32)wz / SPACE;
    const f64 ft = timeSec / TIME;

    const i32 x0 = (i32)std::floor(fx), z0 = (i32)std::floor(fz);
    const i64 t0 = (i64)std::floor(ft);
    const f32 tx = smoothStep(0.f, 1.f, fx - (f32)x0);
    const f32 tz = smoothStep(0.f, 1.f, fz - (f32)z0);
    const f32 tt = smoothStep(0.f, 1.f, (f32)(ft - (f64)t0));

    f32 corner[2];
    for (i32 dt = 0; dt < 2; ++dt) {
        const f32 a = lattice(x0,     z0,     t0 + dt, seed);
        const f32 b = lattice(x0 + 1, z0,     t0 + dt, seed);
        const f32 c = lattice(x0,     z0 + 1, t0 + dt, seed);
        const f32 d = lattice(x0 + 1, z0 + 1, t0 + dt, seed);
        corner[dt] = glm::mix(glm::mix(a, b, tx), glm::mix(c, d, tx), tz);
    }
    return glm::mix(corner[0], corner[1], tt);
}

/// Пределы скорости циклона, блоков в секунду.
///
/// Из них и считается CYCLONE_CELL_SPAN: быстрее — и циклон успеет
/// приехать из ячейки, которую никто не опрашивает.
constexpr f32 DRIFT_MIN = 0.35f;
constexpr f32 DRIFT_MAX = 1.20f;

constexpr f32 RADIUS_MIN = 2000.f;
constexpr f32 RADIUS_MAX = 4200.f;

/// Доля ячеек, где циклон вообще рождается.
constexpr u32 BIRTH_CHANCE_256 = 56;    // ~22%

/// Циклон ячейки (cx, cz), рождённый в эпоху e, на момент timeSec.
/// false — такого нет или он уже мёртв.
bool cycloneOf(i32 cx, i32 cz, i64 epoch, f64 timeSec, u64 seed, Cyclone& out) {
    const u32 h = hash3(cx, cz, epoch, seed ^ 0xC7C10E5ull);
    if ((h & 0xFFu) >= BIRTH_CHANCE_256) return false;

    const f64 born = (f64)epoch * CYCLONE_EPOCH_SEC;
    const f64 age  = timeSec - born;
    if (age < 0.0 || age >= CYCLONE_LIFE_SEC) return false;

    const f32 a01 = (f32)(age / CYCLONE_LIFE_SEC);

    // Рождается, крепнет, держится и уходит. Ровная «сила всю жизнь»
    // включала бы дождь рубильником: вот его не было, вот он идёт.
    // Но и чистая синусоида не годится: полную силу циклон набирал
    // бы ровно в одной точке жизни, и дождь шёл бы считанные минуты
    // на весь циклон. Поэтому площадка: разгон, плато, затухание.
    const f32 envelope = smoothStep(0.f, 0.22f, a01) *
                         (1.f - smoothStep(0.70f, 1.f, a01));

    const f32 rad = RADIUS_MIN + frac(h, 4) * (RADIUS_MAX - RADIUS_MIN);
    const f32 str = 0.70f + frac(h, 12) * 0.30f;

    const f32 dir   = frac(h, 16) * 6.2831853f;
    const f32 speed = DRIFT_MIN + frac(h, 20) * (DRIFT_MAX - DRIFT_MIN);
    const glm::vec2 drift{ std::cos(dir) * speed, std::sin(dir) * speed };

    const glm::vec2 birth{
        (f32)cx * (f32)CYCLONE_CELL + frac(h, 0) * (f32)CYCLONE_CELL,
        (f32)cz * (f32)CYCLONE_CELL + frac(h, 8) * (f32)CYCLONE_CELL
    };

    out.center   = birth + drift * (f32)age;
    out.radius   = rad;
    out.strength = str * envelope;
    out.age      = a01;
    out.drift    = drift;
    return true;
}

/// Вклад циклона в давление, 0..1.
f32 pressureOf(const Cyclone& c, const glm::vec2& p) {
    const glm::vec2 d = p - c.center;
    const f32 r2 = (d.x * d.x + d.y * d.y) / (c.radius * c.radius);
    // Гаусс, а не резкий круг: у циклона нет стены, у него есть край.
    return c.strength * std::exp(-r2 * 2.2f);
}

} // namespace

const char* skyName(Sky s) {
    switch (s) {
        case Sky::Clear:    return "clear";
        case Sky::Fair:     return "fair";
        case Sky::Cloudy:   return "cloudy";
        case Sky::Overcast: return "overcast";
        case Sky::Rain:     return "rain";
        case Sky::Snow:     return "snow";
        case Sky::Storm:    return "storm";
        default:            return "?";
    }
}

u32 WeatherField::cyclonesNear(f64 timeSec, i32 wx, i32 wz,
                               Cyclone* out, u32 cap) const
{
    if (!out || cap == 0) return 0;
    u32 n = 0;

    const i64 epoch0 = (i64)std::floor(timeSec / CYCLONE_EPOCH_SEC);
    const i32 cx0 = (i32)std::floor((f32)wx / (f32)CYCLONE_CELL);
    const i32 cz0 = (i32)std::floor((f32)wz / (f32)CYCLONE_CELL);

    // Живы три поколения: рождённое в эту эпоху и два прежних.
    for (i64 de = 0; de < 3; ++de)
        for (i32 dz = -CYCLONE_CELL_SPAN; dz <= CYCLONE_CELL_SPAN; ++dz)
            for (i32 dx = -CYCLONE_CELL_SPAN; dx <= CYCLONE_CELL_SPAN; ++dx) {
                Cyclone c;
                if (!cycloneOf(cx0 + dx, cz0 + dz, epoch0 - de, timeSec, seed_, c))
                    continue;
                if (c.strength <= 0.001f) continue;

                // Дальние не нужны: за двумя радиусами гаусс уже
                // ниже кванта цвета. Без отсева список забивался
                // циклонами за двадцать пять тысяч блоков, и
                // ближний, найденный последним, в него не помещался.
                const f32 ddx = (f32)wx - c.center.x;
                const f32 ddz = (f32)wz - c.center.y;
                const f32 reach = c.radius * 2.f;
                if (ddx * ddx + ddz * ddz > reach * reach) continue;
                if (n < cap) out[n++] = c;
                if (n == cap) return n;
            }
    return n;
}

bool WeatherField::snowsAt(const TerrainGenerator& terrain, i32 wx, i32 wz) {
    const TerrainGenerator::Column col = terrain.column(wx, wz);
    // Ровно то же число, по которому биом решает, тайга он или лес:
    // температура с поправкой на высоту. Второе правило разошлось бы
    // с первым, и снег пошёл бы над зелёной травой.
    const f32 adjusted =
        col.climate.temperature - (f32)std::max(0, col.surface - 40) * 0.008f;

    // И тот же порог, что у тайги (-0.10 в BiomeField::classify).
    // Снег идёт там, где по климату уже тайга и холоднее: в тундре,
    // в тайге и на холодных равнинах за ними. Своё число здесь
    // означало бы полосу земли, где ёлки стоят, а снег не идёт.
    return adjusted < -0.10f;
}

WeatherSample WeatherField::at(const TerrainGenerator& terrain,
                               i32 wx, i32 wz, f64 timeSec) const
{
    WeatherSample s{};

    Cyclone near[CYCLONE_MAX_NEAR];
    const u32 n = cyclonesNear(timeSec, wx, wz, near, CYCLONE_MAX_NEAR);

    const glm::vec2 p{ (f32)wx, (f32)wz };

    // Давление складывается как объединение, а не суммой: два
    // циклона рядом дают глубокую впадину, но не двойную — за
    // единицу давление не уходит никогда.
    f32 clearProb = 1.f;
    glm::vec2 wind{0.f};

    for (u32 i = 0; i < n; ++i) {
        const f32 c = pressureOf(near[i], p);
        clearProb *= (1.f - glm::clamp(c, 0.f, 1.f));

        // Ветер в циклоне закручен: он дует поперёк радиуса и слегка
        // внутрь. Из-за этого при проходе циклона ветер ПОВОРАЧИВАЕТ,
        // а не просто стихает, — по этому циклон и узнают.
        const glm::vec2 d = p - near[i].center;
        const f32 len = std::sqrt(d.x * d.x + d.y * d.y);
        if (len > 1.f) {
            const glm::vec2 radial = d / len;
            const glm::vec2 tangent{ -radial.y, radial.x };
            const f32 mag = c * 14.f;
            wind += (tangent * 0.85f - radial * 0.35f) * mag;
        }
        wind += near[i].drift * (c * 2.f);
    }
    s.pressure = 1.f - clearProb;

    // Лёгкий ровный ветерок, чтобы в ясную погоду не было мёртвого
    // штиля: снег в безветрие падает отвесной сеткой и выглядит
    // нарисованным.
    const f32 breeze = backgroundCloud(wx + 91000, wz - 47000, timeSec * 0.5, seed_ ^ 0xB2EEull);
    const f32 breezeDir = breeze * 6.2831853f;
    wind += glm::vec2{ std::cos(breezeDir), std::sin(breezeDir) } * 1.6f;
    s.wind = wind;

    // Тучи: своё медленное поле плюс всё, что принёс циклон.
    const f32 bg = backgroundCloud(wx, wz, timeSec, seed_);
    const f32 bgCloud = smoothStep(0.45f, 0.88f, bg) * 0.75f;
    const f32 cycCloud = smoothStep(0.05f, 0.42f, s.pressure);
    s.cloud = glm::clamp(1.f - (1.f - bgCloud) * (1.f - cycCloud), 0.f, 1.f);

    // Осадки — только у сердцевины: тучи бывают и без дождя, а дождь
    // без туч не бывает никогда.
    s.precip = smoothStep(0.48f, 0.78f, s.pressure);
    s.snow   = snowsAt(terrain, wx, wz);

    const f32 windSpeed = std::sqrt(s.wind.x * s.wind.x + s.wind.y * s.wind.y);
    if (s.precip > 0.60f && windSpeed > 12.f) s.sky = Sky::Storm;
    else if (s.precip > 0.12f)               s.sky = s.snow ? Sky::Snow : Sky::Rain;
    else if (s.cloud > 0.72f)                s.sky = Sky::Overcast;
    else if (s.cloud > 0.38f)                s.sky = Sky::Cloudy;
    else if (s.cloud > 0.15f)                s.sky = Sky::Fair;
    else                                     s.sky = Sky::Clear;

    return s;
}

f32 WeatherField::rainbowAt(const TerrainGenerator& terrain,
                            i32 wx, i32 wz, f64 timeSec, f32 sunElevation) const
{
    // Солнце должно быть НИЗКО: центр радуги лежит против солнца, и
    // при высоком солнце дуга уходит под горизонт целиком. Это не
    // условная красивость, а геометрия — радуга бывает утром и
    // вечером.
    const f32 low = smoothStep(0.02f, 0.10f, sunElevation) *
                    (1.f - smoothStep(0.35f, 0.52f, sunElevation));
    if (low <= 0.001f) return 0.f;

    const WeatherSample now = at(terrain, wx, wz, timeSec);
    if (now.snow) return 0.f;                 // на снегу радуги не бывает

    // Ливень не годится: в ливень солнца не видно. А морось —
    // годится, и это не поблажка: радугу как раз и видят, когда
    // дождь ещё сеет, а солнце уже вышло.
    const f32 stopped = 1.f - smoothStep(0.25f, 0.55f, now.precip);
    if (stopped <= 0.001f) return 0.f;

    // И небо со стороны солнца обязано расчиститься.
    const f32 open = 1.f - smoothStep(0.65f, 0.92f, now.cloud);
    if (open <= 0.001f) return 0.f;

    // А дождь — быть недавно. Прошлое спрашивается у той же чистой
    // функции: хранить «когда в последний раз шёл дождь» значило бы
    // хранить погоду, а она нарочно без памяти.
    //
    // Полчаса назад, а не пять минут: дождь здесь идёт полосой в
    // десятки минут, и «только что перестало» — это край этой
    // полосы, а не мгновение после неё.
    f32 wasRaining = 0.f;
    for (i32 k = 1; k <= 8; ++k) {
        const f64 back = timeSec - (f64)k * 225.0;
        if (back < 0.0) break;
        wasRaining = std::max(wasRaining, at(terrain, wx, wz, back).precip);
    }
    const f32 recent = smoothStep(0.20f, 0.50f, wasRaining);
    if (recent <= 0.001f) return 0.f;

    // Дождь обязан именно УХОДИТЬ. Иначе радуга вставала бы и перед
    // грозой — на разгоне, когда всё ещё только начинается.
    const f32 clearing = smoothStep(0.f, 0.15f, wasRaining - now.precip);

    return glm::clamp(low * stopped * open * recent * clearing, 0.f, 1.f);
}

// ============================================================
// Сглаженная погода
// ============================================================

void Weather::reset() {
    sample_ = WeatherSample{};
    cloud_ = precip_ = rainbow_ = snowMix_ = 0.f;
    wind_ = glm::vec2{0.f};
    pullTimer_ = 0.f;
}

void Weather::pull(const TerrainGenerator& terrain, const glm::vec3& pos,
                   f64 timeSec, f32 sunElevation)
{
    const i32 wx = (i32)std::floor(pos.x);
    const i32 wz = (i32)std::floor(pos.z);
    sample_ = field_.at(terrain, wx, wz, timeSec);
    rainbowTarget_ = field_.rainbowAt(terrain, wx, wz, timeSec, sunElevation);
}

void Weather::snap(const TerrainGenerator& terrain, const glm::vec3& pos,
                   f64 timeSec)
{
    pull(terrain, pos, timeSec, 0.f);
    cloud_   = sample_.cloud;
    precip_  = sample_.precip;
    snowMix_ = sample_.snow ? 1.f : 0.f;
    wind_    = sample_.wind;
    rainbow_ = 0.f;
    pullTimer_ = 0.f;
}

void Weather::update(const TerrainGenerator& terrain, const glm::vec3& pos,
                     f64 timeSec, f32 sunElevation, f32 dt)
{
    pullTimer_ -= dt;
    if (pullTimer_ <= 0.f) {
        pullTimer_ = WEATHER_PULL_SEC;
        pull(terrain, pos, timeSec, sunElevation);
    }

    // Подтягивание с постоянной времени: снег, начавшийся ровно на
    // границе биома, иначе переключался бы туда-сюда на каждом шаге.
    auto approach = [dt](f32 cur, f32 target, f32 rate) {
        const f32 k = 1.f - std::exp(-rate * dt);
        return cur + (target - cur) * k;
    };

    cloud_   = approach(cloud_,   sample_.cloud,  0.35f);
    precip_  = approach(precip_,  sample_.precip, 0.25f);
    snowMix_ = approach(snowMix_, sample_.snow ? 1.f : 0.f, 0.20f);
    rainbow_ = approach(rainbow_, rainbowTarget_, 0.15f);
    wind_.x  = approach(wind_.x,  sample_.wind.x, 0.50f);
    wind_.y  = approach(wind_.y,  sample_.wind.y, 0.50f);
}

f32 Weather::lightScale() const {
    // Пасмурный день темнее ясного почти вдвое — это и есть главный
    // признак пасмурности, куда более внятный, чем цвет неба.
    return 1.f - cloud_ * 0.55f;
}

} // namespace world
