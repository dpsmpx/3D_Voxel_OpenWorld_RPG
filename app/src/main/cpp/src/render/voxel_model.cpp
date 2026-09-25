/**
 * @file voxel_model.cpp
 * @brief Рендер: воксельные модели из мелких вокселей — меш и значок.
 */
#include "voxel_model.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

namespace render {

namespace {

/// 0xRRGGBBAA → байты R, G, B, A в памяти (см. packInstanceColor).
u32 toGpu(u32 rgba) {
    const u32 r = (rgba >> 24) & 0xFFu, g = (rgba >> 16) & 0xFFu;
    const u32 b = (rgba >>  8) & 0xFFu, a =  rgba        & 0xFFu;
    return r | (g << 8) | (b << 16) | (a << 24);
}

} // namespace

usize VoxelModel::solidCount() const {
    usize n = 0;
    for (u32 c : cells) n += (c & 0xFFu) ? 1u : 0u;
    return n;
}

VoxelMesh meshVoxelModel(const VoxelModel& m) {
    VoxelMesh out;
    const i32 dims[3] = { m.sx, m.sy, m.sz };
    if (m.sx <= 0 || m.sy <= 0 || m.sz <= 0) return out;

    const f32 vs = m.voxelSize;
    const glm::vec3 offset{ -0.5f * (f32)m.sx * vs, 0.f, -0.5f * (f32)m.sz * vs };
    std::vector<u32> mask;

    for (int d = 0; d < 3; ++d) {
        const int u = (d + 1) % 3, v = (d + 2) % 3;
        const i32 nu = dims[u], nv = dims[v];
        mask.assign((usize)(nu * nv), 0u);

        for (int sign = -1; sign <= 1; sign += 2) {
            for (i32 s = 0; s < dims[d]; ++s) {
                // Грань видна, если за ней по направлению нормали пусто.
                for (i32 j = 0; j < nv; ++j)
                    for (i32 i = 0; i < nu; ++i) {
                        i32 p[3];
                        p[d] = s; p[u] = i; p[v] = j;
                        const u32 c = m.at(p[0], p[1], p[2]);
                        u32 val = 0;
                        if (c & 0xFFu) {
                            p[d] += sign;
                            if (!m.solid(p[0], p[1], p[2])) val = c;
                        }
                        mask[(usize)(j * nu + i)] = val;
                    }

                // Жадное слияние: прямоугольники одного цвета.
                for (i32 j = 0; j < nv; ++j)
                    for (i32 i = 0; i < nu;) {
                        const u32 c = mask[(usize)(j * nu + i)];
                        if (!c) { ++i; continue; }
                        i32 w = 1;
                        while (i + w < nu && mask[(usize)(j * nu + i + w)] == c) ++w;
                        i32 h = 1;
                        for (bool ok = true; j + h < nv && ok;) {
                            for (i32 k = 0; k < w; ++k)
                                if (mask[(usize)((j + h) * nu + i + k)] != c) { ok = false; break; }
                            if (ok) ++h;
                        }

                        glm::vec3 o{0.f}, eu{0.f}, ev{0.f}, n{0.f};
                        o[d] = (f32)(sign > 0 ? s + 1 : s);
                        o[u] = (f32)i;
                        o[v] = (f32)j;
                        eu[u] = (f32)w;
                        ev[v] = (f32)h;
                        n[d]  = (f32)sign;
                        // Против часовой снаружи: cross(du, dv) = нормаль.
                        // e_u × e_v = e_d, поэтому для обратной грани
                        // оси меняются местами.
                        const glm::vec3 du = sign > 0 ? eu : ev;
                        const glm::vec3 dv = sign > 0 ? ev : eu;

                        const u32 base = (u32)out.vertices.size();
                        const glm::vec3 corners[4] = { o, o + du, o + du + dv, o + dv };
                        const u32 gpu = toGpu(c);
                        for (const glm::vec3& q : corners)
                            out.vertices.push_back({ q * vs + offset, gpu, n });
                        const u32 idx[6] = { base, base + 1, base + 2, base, base + 2, base + 3 };
                        out.indices.insert(out.indices.end(), idx, idx + 6);

                        for (i32 y = 0; y < h; ++y)
                            for (i32 x = 0; x < w; ++x)
                                mask[(usize)((j + y) * nu + i + x)] = 0u;
                        i += w;
                    }
            }
        }
    }

    out.boundsMin = offset;
    out.boundsMax = offset + glm::vec3((f32)m.sx, (f32)m.sy, (f32)m.sz) * vs;
    return out;
}

void renderVoxelIcon(const VoxelModel& m, u32 size, u8* rgba, u32 strideBytes) {
    for (u32 y = 0; y < size; ++y)
        std::memset(rgba + (usize)y * strideBytes, 0, (usize)size * 4u);

    const VoxelMesh mesh = meshVoxelModel(m);
    if (mesh.indices.empty() || size == 0) return;

    // ---- Вид ----
    //
    // Сперва поворот вокруг вертикали, потом наклон: верх модели
    // уходит к зрителю, и взгляд падает на неё сверху. В кадре X
    // вправо, Y вниз; глубина — к зрителю.
    const f32 cy = std::cos(m.iconYaw),   syw = std::sin(m.iconYaw);
    const f32 cp = std::cos(m.iconPitch), sp  = std::sin(m.iconPitch);
    auto toView = [&](const glm::vec3& p) {
        const f32 x1 = cy * p.x + syw * p.z;
        const f32 z1 = -syw * p.x + cy * p.z;
        const f32 y2 = cp * p.y - sp * z1;
        const f32 z2 = sp * p.y + cp * z1;
        return glm::vec3(x1, y2, z2);
    };

    // Свет слева сверху, чуть со стороны зрителя.
    const glm::vec3 L = glm::normalize(glm::vec3(-0.45f, 0.8f, 0.55f));

    // Двукратная выборка: края сглаживаются усреднением.
    constexpr u32 SS = 2;
    const u32 W = size * SS;

    std::vector<glm::vec3> pv(mesh.vertices.size());
    glm::vec2 lo{ 1e9f }, hi{ -1e9f };
    for (usize i = 0; i < mesh.vertices.size(); ++i) {
        pv[i] = toView(mesh.vertices[i].pos);
        lo = glm::min(lo, glm::vec2(pv[i].x, -pv[i].y));
        hi = glm::max(hi, glm::vec2(pv[i].x, -pv[i].y));
    }
    // Поля по пикселю итоговой картинки с каждой стороны — под контур.
    const f32 pad = (f32)(2 * SS);
    const glm::vec2 ext = glm::max(hi - lo, glm::vec2(1e-6f));
    const f32 scale = ((f32)W - 2.f * pad) / std::max(ext.x, ext.y);
    const glm::vec2 centre = (lo + hi) * 0.5f;
    auto toPixel = [&](const glm::vec3& v) {
        return glm::vec2((v.x - centre.x) * scale + (f32)W * 0.5f,
                         (-v.y - centre.y) * scale + (f32)W * 0.5f);
    };

    std::vector<f32> depth((usize)W * W, -1e30f);
    std::vector<u8>  big((usize)W * W * 4, 0);

    for (usize t = 0; t + 2 < mesh.indices.size(); t += 3) {
        const VoxelModelVertex& A = mesh.vertices[mesh.indices[t]];
        const glm::vec3 nView = toView(A.normal);
        if (nView.z <= 1e-4f) continue;   // отвёрнута от зрителя

        const glm::vec3 va = pv[mesh.indices[t]];
        const glm::vec3 vb = pv[mesh.indices[t + 1]];
        const glm::vec3 vc = pv[mesh.indices[t + 2]];
        const glm::vec2 a = toPixel(va), b = toPixel(vb), c = toPixel(vc);

        const f32 area = (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
        if (std::fabs(area) < 1e-8f) continue;

        const f32 shade = 0.42f + 0.62f * std::max(0.f, glm::dot(nView, L));
        const u32 g = A.colorGpu;
        const f32 cr = (f32)( g        & 0xFFu), cg = (f32)((g >>  8) & 0xFFu);
        const f32 cb = (f32)((g >> 16) & 0xFFu);
        const u8 r8 = (u8)std::min(255.f, cr * shade);
        const u8 g8 = (u8)std::min(255.f, cg * shade);
        const u8 b8 = (u8)std::min(255.f, cb * shade);

        const i32 x0 = std::max(0, (i32)std::floor(std::min({ a.x, b.x, c.x })));
        const i32 x1 = std::min((i32)W - 1, (i32)std::ceil(std::max({ a.x, b.x, c.x })));
        const i32 y0 = std::max(0, (i32)std::floor(std::min({ a.y, b.y, c.y })));
        const i32 y1 = std::min((i32)W - 1, (i32)std::ceil(std::max({ a.y, b.y, c.y })));
        for (i32 y = y0; y <= y1; ++y)
            for (i32 x = x0; x <= x1; ++x) {
                const f32 px = (f32)x + 0.5f, py = (f32)y + 0.5f;
                const f32 w0 = ((b.x - px) * (c.y - py) - (b.y - py) * (c.x - px)) / area;
                const f32 w1 = ((c.x - px) * (a.y - py) - (c.y - py) * (a.x - px)) / area;
                const f32 w2 = 1.f - w0 - w1;
                if (w0 < 0.f || w1 < 0.f || w2 < 0.f) continue;
                const f32 z = w0 * va.z + w1 * vb.z + w2 * vc.z;
                const usize k = (usize)y * W + (usize)x;
                if (z <= depth[k]) continue;
                depth[k] = z;
                big[k * 4 + 0] = r8;
                big[k * 4 + 1] = g8;
                big[k * 4 + 2] = b8;
                big[k * 4 + 3] = 255;
            }
    }

    // ---- Уменьшение с усреднением ----
    std::vector<u8> small((usize)size * size * 4, 0);
    for (u32 y = 0; y < size; ++y)
        for (u32 x = 0; x < size; ++x) {
            u32 sr = 0, sg = 0, sb = 0, sa = 0;
            for (u32 dy = 0; dy < SS; ++dy)
                for (u32 dx = 0; dx < SS; ++dx) {
                    const usize k = ((usize)(y * SS + dy) * W + (x * SS + dx)) * 4;
                    const u32 al = big[k + 3];
                    sr += big[k + 0] * al; sg += big[k + 1] * al; sb += big[k + 2] * al;
                    sa += al;
                }
            u8* o = &small[((usize)y * size + x) * 4];
            if (sa == 0) continue;
            o[0] = (u8)(sr / sa);
            o[1] = (u8)(sg / sa);
            o[2] = (u8)(sb / sa);
            o[3] = (u8)(sa / (SS * SS));
        }

    // ---- Контур ----
    //
    // Тёмная кайма по силуэту: светлый значок на светлой подложке и
    // тёмный на тёмной без неё растворяются.
    constexpr u8 OUTLINE[3] = { 22, 18, 26 };
    constexpr f32 OUTLINE_A = 0.75f;
    for (u32 y = 0; y < size; ++y)
        for (u32 x = 0; x < size; ++x) {
            const u8* s = &small[((usize)y * size + x) * 4];
            u8* o = rgba + (usize)y * strideBytes + (usize)x * 4;
            bool edge = false;
            if (s[3] < 250) {
                const i32 nx[4] = { (i32)x - 1, (i32)x + 1, (i32)x, (i32)x };
                const i32 ny[4] = { (i32)y, (i32)y, (i32)y - 1, (i32)y + 1 };
                for (int k = 0; k < 4 && !edge; ++k) {
                    if (nx[k] < 0 || ny[k] < 0 || nx[k] >= (i32)size || ny[k] >= (i32)size) continue;
                    edge = small[((usize)ny[k] * size + (usize)nx[k]) * 4 + 3] >= 200;
                }
            }
            if (!edge) { std::memcpy(o, s, 4); continue; }
            // Значок поверх каймы.
            const f32 sa = (f32)s[3] / 255.f;
            const f32 outA = sa + OUTLINE_A * (1.f - sa);
            for (int ch = 0; ch < 3; ++ch) {
                const f32 v = ((f32)s[ch] * sa + (f32)OUTLINE[ch] * OUTLINE_A * (1.f - sa)) /
                              std::max(outA, 1e-4f);
                o[ch] = (u8)std::min(255.f, v + 0.5f);
            }
            o[3] = (u8)std::min(255.f, outA * 255.f + 0.5f);
        }
}

} // namespace render
