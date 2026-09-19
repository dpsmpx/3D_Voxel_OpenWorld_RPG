/**
 * @file camera.h
 * @brief Рендер: меширование чанков, отсечение, инстансинг, камера.
 */
#pragma once
#include "../core/types.h"
#include "../core/math.h"
#include "../physics/raycast.h"
#include "../world/chunk_manager.h"
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/matrix_inverse.hpp>
#include <cmath>
#include <algorithm>

namespace render {

/// Раскладка обязана совпадать с блоком CameraUbo во всех шейдерах.
///
/// Последние три поля — свет суток, посчитанный ОДИН раз за кадр.
///
/// Раньше их считал каждый фрагментный шейдер сам: цвет солнца по
/// высоте (смешивание плюс smoothstep), перевод цвета неба в линейное
/// пространство, нормировка оттенка рассеянного света с делением, сила
/// света по времени суток. Всё это зависит ТОЛЬКО от uniform — то есть
/// одинаково для всех пикселей кадра, — и пересчитывалось для каждого
/// из полутора миллионов фрагментов ландшафта.
///
/// Замер (tools/gpubench, полноэкранный проход настоящим voxel.frag,
/// 2306x1080, два круга): было 6.09 и 6.15 мс, стало 5.62 и 5.51 —
/// 0.55 мс, 9% шейдера. Потолок здесь 1.23 мс (4.91 и 4.86 мс, если
/// подставить готовые ЧИСЛА прямо в текст шейдера), но он недостижим:
/// из uniform-буфера значения приходят непрозрачными для компилятора,
/// и свернуть с ними дальнейшие выражения он уже не может.
///
/// Числа хоста переносятся только как отношение (см. предупреждение
/// самого gpubench). На устройстве мерить нужно то же место:
/// математика фрагмента ландшафта была 5.99 мс из 11.00 мс кадра —
/// больше половины всего кадра.
///
/// Заодно это единственный способ не иметь четырёх копий одной
/// формулы: voxel, grass, mob и sky считали её каждый по-своему.
struct CameraUbo {
    glm::mat4 viewProj;
    glm::mat4 invViewProj;
    glm::vec4 cameraPos;
    glm::vec4 screenSize;
    glm::vec4 sunDir;      ///< xyz — направление на солнце, w — освещённость неба
    glm::vec4 fogParams;   ///< start, end, время суток [0,1), время в секундах
    glm::vec4 skyColor;    ///< цвет неба, sRGB; w — доля дня, 0 ночь, 1 день
    glm::vec4 sunLight;    ///< rgb — цвет солнца, линейный; w — день × над горизонтом
    glm::vec4 ambLight;    ///< rgb — оттенок рассеянного света; w — его сила
    glm::vec4 skyLinear;   ///< rgb — цвет неба, линейный; w — солнце над горизонтом
    /// Погода. x — доля неба под тучами, y — сила осадков,
    /// z — яркость радуги, w — снег (0 дождь, 1 снег).
    glm::vec4 weather;
    /// Ветер. xy — блоков в секунду, по которым едут тучи;
    /// z — время в секундах для их движения, w — запас.
    glm::vec4 wind;
};

class Camera {
public:
    void setAspect(f32 a)                 { aspect_ = a; }
    void setViewport(u32 w, u32 h)        { screenW_ = w; screenH_ = h; }
    void setSunDir(const glm::vec3& d)    { sunDir_ = glm::normalize(d); }
    void setFog(f32 start, f32 end)       { fogStart_ = start; fogEnd_ = end; }

    /// Погода: тучи, осадки, радуга, снег и ветер.
    /// Задаются раз в кадр из world::Weather.
    void setWeather(f32 cloud, f32 precip, f32 rainbow, f32 snowMix,
                    const glm::vec2& wind)
    {
        cloud_ = cloud; precip_ = precip; rainbow_ = rainbow;
        snowMix_ = snowMix; wind_ = wind;
    }
    f32 cloudCover() const { return cloud_; }

    /// Параметры суток: цвет неба, сила небесного света и фаза дня.
    /// Задаются раз в кадр из world::DayCycle.
    void setSky(const glm::vec3& color, f32 light, f32 timeOfDay) {
        skyColor_  = color;
        skyLight_  = light;
        timeOfDay_ = timeOfDay;
    }
    const glm::vec3& skyColor() const { return skyColor_; }

    void setYawPitch(f32 y, f32 p) {
        if (debugCamera_) return;              // см. setDebugCamera
        yaw_ = y; pitch_ = glm::clamp(p, -1.5f, 1.5f);
    }
    f32  yaw()   const { return yaw_; }
    f32  pitch() const { return pitch_; }

    /// Камера-таргет (позиция ступней игрока)
    void setTargetPosition(const glm::vec3& t) {
        if (debugCamera_) return;              // см. setDebugCamera
        targetPos_ = t;
    }
    // Все, кто двигает камеру, обязаны молчать при прибитой камере:
    // владелец у неё в отладочном режиме ровно один, см. setDebugCamera.
    void setFirstPerson(bool fp)       { if (!debugCamera_) firstPerson_ = fp; }
    void setFirstPersonEye(f32 e)      { if (!debugCamera_) firstEyeH_ = e; }
    void setThirdPersonDistance(f32 d) { if (!debugCamera_) thirdDist_ = d; }
    void setThirdPersonHeight(f32 h)   { if (!debugCamera_) thirdOff_ = h; }
    void setHeadBob(f32 phase, f32 amount) {
        if (debugCamera_) return;
        bobPhase_ = phase; bobAmount_ = amount;
    }

    /// Прибивает камеру намертво: ни followTarget, ни покачивание
    /// головы, ни ввод её больше не двигают.
    ///
    /// Владелец у камеры в отладочном режиме обязан быть ровно один.
    /// Раньше отладочные значения ставились ДО followTarget, и та
    /// пересчитывала position_ заново — совпадение держалось только на
    /// том, что рост глаз и тряска обнулены. Достаточно кому-нибудь
    /// поставить рост глаз позже, и сцена тихо разъезжается.
    void setDebugCamera(const glm::vec3& eye, f32 yaw, f32 pitch) {
        debugCamera_ = true;
        position_    = eye;
        targetPos_   = eye;
        yaw_         = yaw;
        pitch_       = pitch;
        bobPhase_    = 0.f;
        bobAmount_   = 0.f;
        firstEyeH_   = 0.f;
        firstPerson_ = true;
    }
    bool debugCamera() const { return debugCamera_; }

    /// Вычислить позицию камеры с учётом коллизии. Вызывается после
    /// setTargetPosition.
    void followTarget(world::ChunkManager& world, const glm::vec3& aimDir) {
        // Прибитую камеру не двигает никто.
        if (debugCamera_) return;

        /// Сглаженная цель (чуть выше ступней для третьего лица)
        glm::vec3 pivot = targetPos_ + glm::vec3(0, thirdOff_, 0);

        if (firstPerson_) {
            /// Лёгкая тряска при ходьбе
            f32 bobY = std::sin(bobPhase_ * 2.f) * bobAmount_;
            f32 bobX = std::cos(bobPhase_) * bobAmount_ * 0.5f;
            /// Сдвиг по осям камеры. Берём общий right(): здесь стояла
            /// его копия с обратным знаком, и хотя покачивание головы
            /// симметрично и разницы не видно, в следующий раз эту
            /// формулу скопируют туда, где знак уже важен.
            position_ = targetPos_ + glm::vec3(0, firstEyeH_, 0)
                      + glm::vec3(0, bobY, 0)
                      + right() * bobX;
        } else {
            /// Third person: желаемое положение за спиной
            glm::vec3 desired = pivot - aimDir * thirdDist_;

            // Raycast от pivot к desired. Если что-то мешает — сократить дистанцию.
            auto hit = physics::raycastVoxels(world, pivot, -aimDir, thirdDist_);
            if (hit.hit) {
                f32 d = std::max(0.6f, hit.distance - 0.35f);
                desired = pivot - aimDir * d;
            }
            position_ = desired;
        }
    }

    /// Направления (используются игроком и UBO)
    glm::vec3 forward() const {
        f32 cp = std::cos(pitch_), sp = std::sin(pitch_);
        return { cp * std::sin(yaw_), sp, cp * std::cos(yaw_) };
    }
    /// cross(forward, up) — то же, что первая строка поворота в lookAt.
    glm::vec3 right() const { return { -std::cos(yaw_), 0.f, std::sin(yaw_) }; }

    glm::mat4 view() const {
        return glm::lookAt(position_, position_ + forward(), glm::vec3(0,1,0));
    }
    /// На сколько градусов композитор повернёт наш кадр при выводе.
    /// Мы пообещали ему это через preTransform, значит поворачиваем
    /// содержимое сами — здесь, в проекции.
    void setSurfaceRotation(u32 degrees) { surfaceRot_ = degrees; }

    glm::mat4 projection() const {
        // При довороте на 90 или 270 стороны кадра меняются местами:
        // соотношение, посчитанное по буферу, надо перевернуть вместе
        // с картинкой. Раньше это знание жило в вызывающем коде, и
        // одно из двух мест про него забывало — мир выходил сплющен
        // поперёк (0.468 вместо 2.135). Теперь поворот и соотношение
        // считаются в одном месте и разойтись не могут.
        const bool swaps = (surfaceRot_ == 90 || surfaceRot_ == 270);
        const f32 a = (swaps && aspect_ > 0.f) ? 1.f / aspect_ : aspect_;
        auto p = glm::perspective(glm::radians(fov_), a, near_, far_);
        p[1][1] *= -1.f;
        if (surfaceRot_ == 0) return p;
        // Знак проверен устройством, а не рассуждением. Без доворота
        // мир лежал на боку; доворот на -90 сделал его вверх ногами,
        // то есть добавил те же 90 градусов вместо того чтобы снять.
        // Значит знак обратный тому, что кажется очевидным из правила
        // «ось Y в координатах отсечения смотрит вниз».
        //
        // Путь этот запасной: обычно мы просим у композитора IDENTITY,
        // и сюда не попадаем вовсе.
        const f32 deg = surfaceRot_ == 90  ?  90.f
                      : surfaceRot_ == 270 ? -90.f : 180.f;
        return glm::rotate(glm::mat4(1.f), glm::radians(deg),
                           glm::vec3(0.f, 0.f, 1.f)) * p;
    }
    glm::mat4 viewProj() const { return projection() * view(); }

    /// Сколько экранных пикселей занимает одна мировая единица на
    /// расстоянии в одну единицу от камеры. Нужна тем, кто решает по
    /// НАСТОЯЩЕМУ размеру на экране, а не по расстоянию: размер
    /// зависит ещё и от поля зрения, и от разрешения.
    f32 pixelsPerUnit() const {
        const f32 half = std::tan(glm::radians(fov_) * 0.5f);
        return half > 1e-6f ? (f32)screenH_ * 0.5f / half : 0.f;
    }

    /// Отладочный вид террейна, 0 — обычная картинка. См. voxel.frag.
    void setDebugShading(i32 mode) { debugShading_ = mode; }

    CameraUbo toUbo(f32 timeSec) const {
        CameraUbo u;
        u.viewProj    = viewProj();
        u.invViewProj = glm::inverse(u.viewProj);
        u.cameraPos   = glm::vec4(position_, 1.f);
        // z — номер отладочного вида террейна (0 — обычная картинка).
        // Свободная компонента вместо отдельного конвейера: включать
        // вид можно на живом устройстве, не пересобирая шейдеры.
        u.screenSize  = glm::vec4((f32)screenW_, (f32)screenH_,
                                  (f32)debugShading_, 0.f);
        u.sunDir      = glm::vec4(sunDir_, skyLight_);
        u.fogParams   = glm::vec4(fogStart_, fogEnd_, timeOfDay_, timeSec);

        // ---- свет суток: считается здесь и только здесь ----
        //
        // Ровно те же выражения, что стояли во фрагментных шейдерах, —
        // но раз в кадр вместо раза на пиксель. См. комментарий к
        // CameraUbo.
        auto toLin = [](const glm::vec3& c) { return c * c; };

        const f32 day   = glm::clamp(skyLight_, 0.f, 1.f);
        const f32 above = glm::smoothstep(-0.10f, 0.06f, sunDir_.y);
        const glm::vec3 sunTint =
            toLin(glm::mix(glm::vec3(1.00f, 0.52f, 0.26f),
                           glm::vec3(1.00f, 0.97f, 0.92f),
                           glm::smoothstep(0.f, 0.30f, sunDir_.y)));
        // Пасмурное небо серое, и это касается не только неба: тем же
        // цветом красится туман вдали, и разойдись они — стык было бы
        // видно по всей линии горизонта. Поэтому тучи подмешиваются
        // ЗДЕСЬ, один раз, а не в каждом шейдере по-своему.
        const glm::vec3 overcastTint{ 0.60f, 0.62f, 0.66f };
        const glm::vec3 skyCol =
            glm::mix(skyColor_, overcastTint * (0.28f + 0.72f * day),
                     glm::clamp(cloud_, 0.f, 1.f) * 0.75f);

        const glm::vec3 skyLin = toLin(skyCol);
        const f32 skyMax = glm::max(glm::max(skyLin.r, skyLin.g),
                                    glm::max(skyLin.b, 0.001f));
        const glm::vec3 ambTint = glm::mix(glm::vec3(1.f), skyLin / skyMax, 0.55f);

        // Солнце тучи гасят, рассеянный свет — нет. Так оно и есть:
        // в пасмурный день теней не видно, а светло.
        const f32 sunThrough = 1.f - glm::clamp(cloud_, 0.f, 1.f) * 0.85f;

        u.skyColor  = glm::vec4(skyCol, day);
        u.sunLight  = glm::vec4(sunTint, day * above * sunThrough);
        u.ambLight  = glm::vec4(ambTint, glm::mix(0.14f, 0.60f, day));
        u.skyLinear = glm::vec4(skyLin, above);
        u.weather   = glm::vec4(cloud_, precip_, rainbow_, snowMix_);
        u.wind      = glm::vec4(wind_.x, wind_.y, timeSec, 0.f);
        return u;
    }

    math::Frustum frustum() const {
        return math::Frustum::fromViewProj(viewProj());
    }

    const glm::vec3& position() const { return position_; }
    const glm::vec3& sunDir() const   { return sunDir_; }

private:
    glm::vec3 position_{0};
    glm::vec3 targetPos_{0};
    glm::vec3 sunDir_{0.4f, 0.7f, 0.3f};
    f32 yaw_ = 0.f, pitch_ = -0.3f;
    f32 fov_ = 70.f;
    f32 aspect_ = 1.f;
    // Ближняя плоскость на пяти сантиметрах съедала точность буфера
    // глубины: она распределена обратно пропорционально расстоянию до
    // неё, и вдвое более дальняя плоскость даёт вдвое больше разрядов
    // на всю остальную сцену. Ближе десяти сантиметров камера всё
    // равно не подходит: её не пускают столкновения.
    f32 near_ = 0.1f, far_ = 512.f;
    f32 cloud_ = 0.f, precip_ = 0.f, rainbow_ = 0.f, snowMix_ = 0.f;
    glm::vec2 wind_{0.f};
    i32 debugShading_ = 0;
    bool debugCamera_ = false;
    u32 screenW_ = 1080, screenH_ = 1920;
    u32 surfaceRot_ = 0;
    f32 fogStart_ = 150.f, fogEnd_ = 400.f;
    glm::vec3 skyColor_{0.55f, 0.72f, 0.92f};
    f32 skyLight_  = 1.f;
    f32 timeOfDay_ = 0.3f;

    bool firstPerson_ = false;
    f32  firstEyeH_   = 1.62f;
    f32  thirdDist_   = 5.5f;
    f32  thirdOff_    = 1.10f;
    f32  bobPhase_ = 0.f, bobAmount_ = 0.f;
};

} // namespace render
