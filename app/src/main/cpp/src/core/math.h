/**
 * @file math.h
 * @brief Ядро движка: базовые типы, математика, планировщик задач, аллокаторы.
 */
#pragma once
#include "types.h"
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
#include <cmath>
#include <limits>

namespace math {

struct AABB {
    glm::vec3 min{ std::numeric_limits<float>::max()};
    glm::vec3 max{-std::numeric_limits<float>::max()};

    void expand(const glm::vec3& p) {
        min = glm::min(min, p);
        max = glm::max(max, p);
    }
    void expand(const AABB& o) {
        min = glm::min(min, o.min);
        max = glm::max(max, o.max);
    }
    glm::vec3 center() const { return (min + max) * 0.5f; }
    glm::vec3 extents() const { return (max - min) * 0.5f; }
    bool contains(const glm::vec3& p) const {
        return p.x >= min.x && p.x <= max.x
            && p.y >= min.y && p.y <= max.y
            && p.z >= min.z && p.z <= max.z;
    }
    bool intersects(const AABB& o) const {
        return !(o.min.x > max.x || o.max.x < min.x
              || o.min.y > max.y || o.max.y < min.y
              || o.min.z > max.z || o.max.z < min.z);
    }
};

struct Plane {
    glm::vec3 n{0};
    float d = 0;
    float distance(const glm::vec3& p) const { return glm::dot(n, p) + d; }
};

struct Frustum {
    Plane planes[6]; // left, right, bottom, top, near, far

    static Frustum fromViewProj(const glm::mat4& vp) {
        Frustum f;
        // Gribb-Hartmann извлечение плоскостей
        auto row = [&](int i){ return glm::vec4(vp[0][i], vp[1][i], vp[2][i], vp[3][i]); };
        glm::vec4 r0 = row(0), r1 = row(1), r2 = row(2), r3 = row(3);
        /// Vulkan: глубина клипа в [0, 1] (GLM_FORCE_DEPTH_ZERO_TO_ONE),
        /// поэтому ближняя плоскость — это просто строка r2, а не r3 + r2
        /// как в OpenGL-соглашении с диапазоном [-1, 1].
        glm::vec4 planes[6] = {
            r3 + r0, // left
            r3 - r0, // right
            r3 + r1, // bottom
            r3 - r1, // top
            r2,      // near
            r3 - r2, // far
        };
        for (int i = 0; i < 6; ++i) {
            glm::vec3 n(planes[i]);
            float len = glm::length(n);
            f.planes[i].n = n / len;
            f.planes[i].d = planes[i].w / len;
        }
        return f;
    }

    bool intersectsAABB(const AABB& b) const {
        for (auto& p : planes) {
            glm::vec3 pos = b.min;
            if (p.n.x >= 0) pos.x = b.max.x;
            if (p.n.y >= 0) pos.y = b.max.y;
            if (p.n.z >= 0) pos.z = b.max.z;
            if (p.distance(pos) < 0) return false;
        }
        return true;
    }
};

inline glm::vec3 safeNormalize(const glm::vec3& v) {
    float len = glm::length(v);
    return len > 1e-6f ? v / len : glm::vec3(0,0,0);
}

} // namespace math
