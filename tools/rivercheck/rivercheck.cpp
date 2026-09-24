#include "world/terrain.h"
#include <cstdio>
#include <cmath>
#include <vector>

namespace {

bool closeTo(const world::TerrainGenerator::RiverPoint& a,
             const world::TerrainGenerator::RiverPoint& b,
             i32 radius)
{
    const i32 dx = a.x - b.x;
    const i32 dz = a.z - b.z;
    return dx * dx + dz * dz <= radius * radius;
}

}

int main() {
    constexpr u64 SEED = 12648430ULL;
    world::TerrainGenerator terrain(SEED);

    int networks = 0;
    int branched = 0;
    int longRivers = 0;
    int variedSlopes = 0;

    for (i32 cz = -4; cz <= 4; ++cz) {
        for (i32 cx = -4; cx <= 4; ++cx) {
            const auto net = terrain.riverNetworkAtCell(cx, cz);
            if (!net || net->paths.empty()) continue;
            ++networks;

            const auto& main = net->paths.front();
            if (main.points.size() < 24) {
                std::fprintf(stderr, "rivercheck: short main river in cell %d,%d\n", cx, cz);
                return 1;
            }

            i32 positiveDrops = 0;
            i32 totalDrop = 0;
            for (usize i = 1; i < main.points.size(); ++i) {
                const i32 drop = (i32)main.points[i - 1].waterY -
                                 (i32)main.points[i].waterY;
                if (drop > 0) {
                    ++positiveDrops;
                    totalDrop += drop;
                } else if (drop < 0) {
                    std::fprintf(stderr,
                                 "rivercheck: water rises on main river cell %d,%d at point %zu\n",
                                 cx, cz, i);
                    return 1;
                }
            }

            if (totalDrop < 20) {
                std::fprintf(stderr, "rivercheck: main river has insufficient vertical drop in cell %d,%d\n", cx, cz);
                return 1;
            }
            if (positiveDrops >= 4) ++variedSlopes;
            if (main.points.size() >= 80) ++longRivers;

            for (usize pi = 1; pi < net->paths.size(); ++pi) {
                ++branched;
                const auto& branch = net->paths[pi];
                if (branch.points.size() < 16) {
                    std::fprintf(stderr, "rivercheck: short tributary in cell %d,%d\n", cx, cz);
                    return 1;
                }

                const auto& end = branch.points.back();
                bool joined = false;
                i32 nearestWater = 32767;
                for (const auto& p : main.points) {
                    if (closeTo(end, p, 24)) {
                        joined = true;
                        if ((i32)p.waterY < nearestWater) nearestWater = p.waterY;
                    }
                }
                if (!joined || (i32)end.waterY > nearestWater + 1) {
                    std::fprintf(stderr, "rivercheck: tributary does not join downhill in cell %d,%d\n", cx, cz);
                    return 1;
                }

                for (usize i = 1; i < branch.points.size(); ++i) {
                    if (branch.points[i].waterY > branch.points[i - 1].waterY) {
                        std::fprintf(stderr, "rivercheck: tributary rises in cell %d,%d\n", cx, cz);
                        return 1;
                    }
                }
            }
        }
    }

    std::printf("rivercheck: networks=%d branched=%d long=%d varied=%d\n",
                networks, branched, longRivers, variedSlopes);

    if (networks < 3 || branched < 2 || longRivers < 2 || variedSlopes < 2) {
        std::fprintf(stderr, "rivercheck: generated river sample is too sparse/simple\n");
        return 1;
    }

    // Determinism: the same cell must produce byte-equivalent river points.
    const auto a = terrain.riverNetworkAtCell(1, -2);
    const auto b = terrain.riverNetworkAtCell(1, -2);
    if ((!a) != (!b) || (a && b && a->paths.size() != b->paths.size())) {
        std::fprintf(stderr, "rivercheck: network is not deterministic\n");
        return 1;
    }
    if (a && b) {
        for (usize p = 0; p < a->paths.size(); ++p) {
            if (a->paths[p].points.size() != b->paths[p].points.size()) {
                std::fprintf(stderr, "rivercheck: path is not deterministic\n");
                return 1;
            }
            for (usize i = 0; i < a->paths[p].points.size(); ++i) {
                const auto& x = a->paths[p].points[i];
                const auto& y = b->paths[p].points[i];
                if (x.x != y.x || x.z != y.z || x.terrainY != y.terrainY ||
                    x.waterY != y.waterY || std::fabs(x.width - y.width) > 1e-6f) {
                    std::fprintf(stderr, "rivercheck: point is not deterministic\n");
                    return 1;
                }
            }
        }
    }

    std::printf("rivercheck: PASS\n");
    return 0;
}
