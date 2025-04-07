#pragma once

#include "psyqo/gte-registers.hh"

namespace psxsplash {

class NavMeshTri final {
  public:
    psyqo::Vec3 v0, v1, v2;
};

class Navmesh final {
  public:
    union {
        NavMeshTri* polygons;
        uint32_t polygonsOffset;
    };
    uint16_t triangleCount;
    uint16_t reserved;
};

psyqo::Vec3 ComputeNavmeshPosition(psyqo::Vec3& position, Navmesh& navmesh, psyqo::FixedPoint<12> pheight);

}  // namespace psxsplash