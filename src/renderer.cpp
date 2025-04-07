#include "renderer.hh"

#include <EASTL/vector.h>

#include <cstdint>
#include <psyqo/fixed-point.hh>
#include <psyqo/gte-kernels.hh>
#include <psyqo/gte-registers.hh>
#include <psyqo/kernel.hh>
#include <psyqo/matrix.hh>
#include <psyqo/primitives/common.hh>
#include <psyqo/primitives/triangles.hh>
#include <psyqo/soft-math.hh>
#include <psyqo/trigonometry.hh>
#include <psyqo/vector.hh>

#include "EASTL/array.h"
#include "gtemath.hh"
#include "splashpack.hh"

using namespace psyqo::fixed_point_literals;
using namespace psyqo::trig_literals;
using namespace psyqo::GTE;

psxsplash::Renderer *psxsplash::Renderer::instance = nullptr;

void psxsplash::Renderer::Init(psyqo::GPU &gpuInstance) {
    psyqo::Kernel::assert(instance == nullptr, "A second intialization of Renderer was tried");

    clear<Register::TRX, Safe>();
    clear<Register::TRY, Safe>();
    clear<Register::TRZ, Safe>();

    write<Register::OFX, Safe>(psyqo::FixedPoint<16>(160.0).raw());
    write<Register::OFY, Safe>(psyqo::FixedPoint<16>(120.0).raw());

    write<Register::H, Safe>(120);

    write<Register::ZSF3, Safe>(ORDERING_TABLE_SIZE / 3);
    write<Register::ZSF4, Safe>(ORDERING_TABLE_SIZE / 4);

    if (!instance) {
        instance = new Renderer(gpuInstance);
    }
}

void psxsplash::Renderer::SetCamera(psxsplash::Camera &camera) { m_currentCamera = &camera; }


void psxsplash::Renderer::Render(eastl::vector<GameObject *> &objects) {
    psyqo::Kernel::assert(m_currentCamera != nullptr, "PSXSPLASH: Tried to render without an active camera");

    uint8_t parity = m_gpu.getParity();

    auto &ot = m_ots[parity];
    auto &clear = m_clear[parity];
    auto &balloc = m_ballocs[parity];

    balloc.reset();
    eastl::array<psyqo::Vertex, 3> projected;
    for (auto &obj : objects) {
        psyqo::Vec3 cameraPosition, objectPosition;
        psyqo::Matrix33 finalMatrix;

        ::clear<Register::TRX, Safe>();
        ::clear<Register::TRY, Safe>();
        ::clear<Register::TRZ, Safe>();

        // Rotate the camera Translation vector by the camera rotation
        writeSafe<PseudoRegister::Rotation>(m_currentCamera->GetRotation());
        writeSafe<PseudoRegister::V0>(-m_currentCamera->GetPosition());

        Kernels::mvmva<Kernels::MX::RT, Kernels::MV::V0, Kernels::TV::TR>();
        cameraPosition = readSafe<PseudoRegister::SV>();

        // Rotate the object Translation vector by the camera rotation
        writeSafe<PseudoRegister::V0>(obj->position);
        Kernels::mvmva<Kernels::MX::RT, Kernels::MV::V0, Kernels::TV::TR>();
        objectPosition = readSafe<PseudoRegister::SV>();

        objectPosition.x += cameraPosition.x;
        objectPosition.y += cameraPosition.y;
        objectPosition.z += cameraPosition.z;

        // Combine object and camera rotations
        MatrixMultiplyGTE(m_currentCamera->GetRotation(), obj->rotation, &finalMatrix);

        psyqo::GTE::writeSafe<psyqo::GTE::PseudoRegister::Translation>(objectPosition);
        psyqo::GTE::writeSafe<psyqo::GTE::PseudoRegister::Rotation>(finalMatrix);

        for (int i = 0; i < obj->polyCount; i++) {
            Tri &tri = obj->polygons[i];
            psyqo::Vec3 result;

            writeSafe<PseudoRegister::V0>(tri.v0);
            writeSafe<PseudoRegister::V1>(tri.v1);
            writeSafe<PseudoRegister::V2>(tri.v2);

            Kernels::rtpt();
            Kernels::nclip();

            int32_t mac0 = 0;
            read<Register::MAC0>(reinterpret_cast<uint32_t *>(&mac0));
            if (mac0 <= 0) continue;

            int32_t zIndex = 0;
            uint32_t u0, u1, u2;

            read<Register::SZ1>(&u0);
            read<Register::SZ2>(&u1);
            read<Register::SZ3>(&u2);

            int32_t sz0 = (int32_t)u0;
            int32_t sz1 = (int32_t)u1;
            int32_t sz2 = (int32_t)u2;

            if ((sz0 < 1 && sz1 < 1 && sz2 < 1)) {
                continue;
            };

            zIndex = eastl::max(eastl::max(sz0, sz1), sz2);
            if (zIndex < 0 || zIndex >= ORDERING_TABLE_SIZE) continue;

            read<Register::SXY0>(&projected[0].packed);
            read<Register::SXY1>(&projected[1].packed);
            read<Register::SXY2>(&projected[2].packed);

            recursiveSubdivideAndRender(tri, projected, zIndex, 1);
        }
    }
    m_gpu.getNextClear(clear.primitive, m_clearcolor);
    m_gpu.chain(clear);
    m_gpu.chain(ot);
}

void psxsplash::Renderer::RenderNavmeshPreview(psxsplash::Navmesh navmesh, bool isOnMesh) {
    uint8_t parity = m_gpu.getParity();
    eastl::array<psyqo::Vertex, 3> projected;

    auto &ot = m_ots[parity];
    auto &clear = m_clear[parity];
    auto &balloc = m_ballocs[parity];
    balloc.reset();

    psyqo::Vec3 cameraPosition;

    ::clear<Register::TRX, Safe>();
    ::clear<Register::TRY, Safe>();
    ::clear<Register::TRZ, Safe>();

    // Rotate the camera Translation vector by the camera rotation
    writeSafe<PseudoRegister::Rotation>(m_currentCamera->GetRotation());
    writeSafe<PseudoRegister::V0>(m_currentCamera->GetPosition());

    Kernels::mvmva<Kernels::MX::RT, Kernels::MV::V0, Kernels::TV::TR>();
    cameraPosition = readSafe<PseudoRegister::SV>();

    write<Register::TRX, Safe>(-cameraPosition.x.raw());
    write<Register::TRY, Safe>(-cameraPosition.y.raw());
    write<Register::TRZ, Safe>(-cameraPosition.z.raw());

    psyqo::GTE::writeSafe<psyqo::GTE::PseudoRegister::Rotation>(m_currentCamera->GetRotation());

    for (int i = 0; i < navmesh.triangleCount; i++) {
        NavMeshTri &tri = navmesh.polygons[i];
        psyqo::Vec3 result;

        writeSafe<PseudoRegister::V0>(tri.v0);
        writeSafe<PseudoRegister::V1>(tri.v1);
        writeSafe<PseudoRegister::V2>(tri.v2);

        Kernels::rtpt();
        Kernels::nclip();

        int32_t mac0 = 0;
        read<Register::MAC0>(reinterpret_cast<uint32_t *>(&mac0));
        if (mac0 <= 0) continue;

        int32_t zIndex = 0;
        uint32_t u0, u1, u2;
        read<Register::SZ0>(&u0);
        read<Register::SZ1>(&u1);
        read<Register::SZ2>(&u2);

        int32_t sz0 = *reinterpret_cast<int32_t *>(&u0);
        int32_t sz1 = *reinterpret_cast<int32_t *>(&u1);
        int32_t sz2 = *reinterpret_cast<int32_t *>(&u2);

        zIndex = eastl::max(eastl::max(sz0, sz1), sz2);
        if (zIndex < 0 || zIndex >= ORDERING_TABLE_SIZE) continue;

        auto &prim = balloc.allocateFragment<psyqo::Prim::Triangle>();

        prim.primitive.pointA = projected[0];
        prim.primitive.pointB = projected[1];
        prim.primitive.pointC = projected[2];

        psyqo::Color heightColor;

        if (isOnMesh) {
            heightColor.r = 0;
            heightColor.g = ((tri.v0.y.raw() + tri.v1.y.raw() + tri.v2.y.raw()) / 3) * 100 % 256;
            heightColor.b = 0;
        } else {
            heightColor.r = ((tri.v0.y.raw() + tri.v1.y.raw() + tri.v2.y.raw()) / 3) * 100 % 256;
            heightColor.g = 0;
            heightColor.b = 0;
        }

        prim.primitive.setColor(heightColor);
        prim.primitive.setOpaque();
        ot.insert(prim, zIndex);
    }
    m_gpu.getNextClear(clear.primitive, m_clearcolor);
    m_gpu.chain(clear);
    m_gpu.chain(ot);
}

void psxsplash::Renderer::VramUpload(const uint16_t *imageData, int16_t posX, int16_t posY, int16_t width,
                                     int16_t height) {
    psyqo::Rect uploadRect{.a = {.x = posX, .y = posY}, .b = {width, height}};
    m_gpu.uploadToVRAM(imageData, uploadRect);
}

psyqo::Color averageColor(const psyqo::Color &a, const psyqo::Color &b) {
    return psyqo::Color{static_cast<uint8_t>((a.r + b.r) >> 1), static_cast<uint8_t>((a.g + b.g) >> 1),
                        static_cast<uint8_t>((a.b + b.b) >> 1)};
}

void psxsplash::Renderer::recursiveSubdivideAndRender(Tri &tri, eastl::array<psyqo::Vertex, 3> &projected, int zIndex,
                                                      int maxIterations) {
    uint16_t minX = eastl::min({projected[0].x, projected[1].x, projected[2].x});
    uint16_t maxX = eastl::max({projected[0].x, projected[1].x, projected[2].x});
    uint16_t minY = eastl::min({projected[0].y, projected[1].y, projected[2].y});
    uint16_t maxY = eastl::max({projected[0].y, projected[1].y, projected[2].y});
    uint16_t width = maxX - minX;
    uint16_t height = maxY - minY;

    bool leavingScreenSpace = false;
    if (projected[0].x < -100 || projected[0].y < -100 || projected[1].x < -100 || projected[1].y < -100 ||
        projected[2].x < -100 || projected[2].y < -100 || width > 420 || height > 356) {
        leavingScreenSpace = true;
    }

    if (maxIterations == 0 || ((width < 512 && height < 256 && !leavingScreenSpace))) {
        auto &balloc = m_ballocs[m_gpu.getParity()];
        auto &prim = balloc.allocateFragment<psyqo::Prim::GouraudTexturedTriangle>();

        prim.primitive.pointA = projected[0];
        prim.primitive.pointB = projected[1];
        prim.primitive.pointC = projected[2];

        prim.primitive.uvA = tri.uvA;
        prim.primitive.uvB = tri.uvB;
        prim.primitive.uvC = tri.uvC;  // uvC remains UVCoordsPadded.
        prim.primitive.tpage = tri.tpage;
        psyqo::PrimPieces::ClutIndex clut(tri.clutX, tri.clutY);
        prim.primitive.clutIndex = clut;
        prim.primitive.setColorA(tri.colorA);
        prim.primitive.setColorB(tri.colorB);
        prim.primitive.setColorC(tri.colorC);
        prim.primitive.setOpaque();

        m_ots[m_gpu.getParity()].insert(prim, zIndex);
        return;
    }

    // Subdivide the triangle
    auto distanceSq = [](const psyqo::Vertex &a, const psyqo::Vertex &b) -> uint32_t {
        int dx = a.x - b.x;
        int dy = a.y - b.y;
        return dx * dx + dy * dy;
    };

    uint32_t d0 = distanceSq(projected[0], projected[1]);
    uint32_t d1 = distanceSq(projected[1], projected[2]);
    uint32_t d2 = distanceSq(projected[2], projected[0]);

    int i, j, k;
    if (d0 >= d1 && d0 >= d2) {
        i = 0;
        j = 1;
        k = 2;
    } else if (d1 >= d0 && d1 >= d2) {
        i = 1;
        j = 2;
        k = 0;
    } else {
        i = 2;
        j = 0;
        k = 1;
    }

    auto getUVu = [&](int idx) -> uint8_t {
        if (idx == 0) return tri.uvA.u;
        if (idx == 1) return tri.uvB.u;
        return tri.uvC.u;
    };

    auto getUVv = [&](int idx) -> uint8_t {
        if (idx == 0) return tri.uvA.v;
        if (idx == 1) return tri.uvB.v;
        return tri.uvC.v;
    };

    auto getColor = [&](int idx) -> psyqo::Color {
        if (idx == 0) return tri.colorA;
        if (idx == 1) return tri.colorB;
        return tri.colorC;
    };

    psyqo::Vertex mid;
    mid.x = (projected[i].x + projected[j].x) >> 1;
    mid.y = (projected[i].y + projected[j].y) >> 1;

    uint8_t newU = (getUVu(i) + getUVu(j)) / 2;
    uint8_t newV = (getUVv(i) + getUVv(j)) / 2;

    psyqo::Color newColor = averageColor(getColor(i), getColor(j));

    eastl::array<psyqo::Vertex, 3> projA, projB;
    projA[0] = projected[i];
    projA[1] = mid;
    projA[2] = projected[k];

    projB[0] = mid;
    projB[1] = projected[j];
    projB[2] = projected[k];

    Tri triA, triB;

    triA.uvA = {getUVu(i), getUVv(i)};
    triA.uvB = {newU, newV};
    triA.uvC = {getUVu(k), getUVv(k)};

    triA.colorA = getColor(i);
    triA.colorB = newColor;
    triA.colorC = getColor(k);

    /*triA.colorA = {.r = 255};
    triA.colorB = {.r = 255};
    triA.colorC = {.r = 255};*/

    triA.tpage = tri.tpage;
    triA.clutX = tri.clutX;
    triA.clutY = tri.clutY;
    triA.normal = tri.normal;

    triB.uvA = {newU, newV};
    triB.uvB = {getUVu(j), getUVv(j)};
    triB.uvC = {getUVu(k), getUVv(k)};

    triB.colorA = newColor;
    triB.colorB = getColor(j);
    triB.colorC = getColor(k);

    /*triB.colorA = {.g = 255};
    triB.colorB = {.g = 255};
    triB.colorC = {.g = 255};*/

    triB.tpage = tri.tpage;
    triB.clutX = tri.clutX;
    triB.clutY = tri.clutY;
    triB.normal = tri.normal;

    recursiveSubdivideAndRender(triA, projA, zIndex, maxIterations - 1);
    recursiveSubdivideAndRender(triB, projB, zIndex, maxIterations - 1);
}