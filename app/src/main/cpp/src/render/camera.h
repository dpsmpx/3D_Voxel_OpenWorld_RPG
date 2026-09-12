/**
 * @file camera.h
 * @brief Рендер: меширование чанков, LOD, отсечение, инстансинг, камера.
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
struct CameraUbo {
    glm::mat4 viewProj;
    glm::mat4 invViewProj;
    glm::vec4 cameraPos;
    glm::vec4 screenSize;
    glm::vec4 sunDir;      ///< xyz — направление на солнце, w — освещённость неба
    glm::vec4 fogParams;   ///< start, end, время суток [0,1), время в секундах
    glm::vec4 skyColor;    ///< цвет неба и тумана текущего времени суток
};

class Camera {
public:
    void setAspect(f32 a)                 { aspect_ = a; }
    void setViewport(u32 w, u32 h)        { screenW_ = w; screenH_ = h; }
    void setSunDir(const glm::vec3& d)    { sunDir_ = glm::normalize(d); }
    void setFog(f32 start, f32 end)       { fogStart_ = start; fogEnd_ = end; }

    /// Параметры суток: цвет неба, сила небесного света и фаза дня.
    /// Задаются раз в кадр из world::DayCycle.
    void setSky(const glm::vec3& color, f32 light, f32 timeOfDay) {
        skyColor_  = color;
        skyLight_  = light;
        timeOfDay_ = timeOfDay;
    }
    const glm::vec3& skyColor() const { return skyColor_; }

    void setYawPitch(f32 y, f32 p)        { yaw_ = y; pitch_ = glm::clamp(p, -1.5f, 1.5f); }
    f32  yaw()   const { return yaw_; }
    f32  pitch() const { return pitch_; }

    /// Камера-таргет (позиция ступней игрока)
    void setTargetPosition(const glm::vec3& t) { targetPos_ = t; }
    void setFirstPerson(bool fp)               { firstPerson_ = fp; }
    void setFirstPersonEye(f32 e)              { firstEyeH_ = e; }
    void setThirdPersonDistance(f32 d)         { thirdDist_ = d; }
    void setThirdPersonHeight(f32 h)           { thirdOff_ = h; }
    void setHeadBob(f32 phase, f32 amount)     { bobPhase_ = phase; bobAmount_ = amount; }

    /// Вычислить позицию камеры с учётом коллизии. Вызывается после setTargetPosition.
    void followTarget(world::ChunkManager& world, const glm::vec3& aimDir) {
        /// Сглаженная цель (чуть выше ступней для третьего лица)
        glm::vec3 pivot = targetPos_ + glm::vec3(0, thirdOff_, 0);

        if (firstPerson_) {
            /// Лёгкая тряска при ходьбе
            f32 bobY = std::sin(bobPhase_ * 2.f) * bobAmount_;
            f32 bobX = std::cos(bobPhase_) * bobAmount_ * 0.5f;
            /// Сдвиг по осям камеры
            glm::vec3 right { std::cos(yaw_), 0.f, -std::sin(yaw_) };
            position_ = targetPos_ + glm::vec3(0, firstEyeH_, 0)
                      + glm::vec3(0, bobY, 0)
                      + right * bobX;
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
    glm::mat4 projection() const {
        auto p = glm::perspective(glm::radians(fov_), aspect_, near_, far_);
        p[1][1] *= -1.f;
        return p;
    }
    glm::mat4 viewProj() const { return projection() * view(); }

    CameraUbo toUbo(f32 timeSec) const {
        CameraUbo u;
        u.viewProj    = viewProj();
        u.invViewProj = glm::inverse(u.viewProj);
        u.cameraPos   = glm::vec4(position_, 1.f);
        u.screenSize  = glm::vec4((f32)screenW_, (f32)screenH_, 0.f, 0.f);
        u.sunDir      = glm::vec4(sunDir_, skyLight_);
        u.fogParams   = glm::vec4(fogStart_, fogEnd_, timeOfDay_, timeSec);
        u.skyColor    = glm::vec4(skyColor_, 1.f);
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
    f32 near_ = 0.05f, far_ = 500.f;
    u32 screenW_ = 1080, screenH_ = 1920;
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
