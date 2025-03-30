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

#include "gtemath.hh"

using namespace psyqo::fixed_point_literals;
using namespace psyqo::trig_literals;
using namespace psyqo::GTE;

psxsplash::Renderer *psxsplash::Renderer::instance = nullptr;

void psxsplash::Renderer::init(psyqo::GPU &gpuInstance) {
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

void psxsplash::Renderer::setCamera(psxsplash::Camera &camera) { m_currentCamera = &camera; }

void psxsplash::Renderer::render(eastl::vector<GameObject *> &objects) {
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
        writeSafe<PseudoRegister::Rotation>(m_currentCamera->getRotation());
        writeSafe<PseudoRegister::V0>(m_currentCamera->getPosition());

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
        matrixMultiplyGTE(m_currentCamera->getRotation(), obj->rotation, &finalMatrix);

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
            uint32_t sz0, sz1, sz2;
            read<Register::SZ0>(&sz0);
            read<Register::SZ1>(&sz1);
            read<Register::SZ2>(&sz2);

            zIndex = eastl::max(eastl::max(sz0, sz1), sz2);
            if (zIndex < 0 || zIndex >= ORDERING_TABLE_SIZE) continue;

            read<Register::SXY0>(&projected[0].packed);
            read<Register::SXY1>(&projected[1].packed);
            read<Register::SXY2>(&projected[2].packed);

            iterativeSubdivideAndRender(tri, projected, zIndex, 3);
        }
    }
    m_gpu.getNextClear(clear.primitive, m_clearcolor);
    m_gpu.chain(clear);
    m_gpu.chain(ot);
}

static inline psyqo::Color averageColor(const psyqo::Color &c1, const psyqo::Color &c2) {
    uint8_t r = (c1.r + c2.r) >> 1;
    uint8_t g = (c1.g + c2.g) >> 1;
    uint8_t b = (c1.b + c2.b) >> 1;
    psyqo::Color c;

    c.r = r;
    c.g = g;
    c.b = b;

    return c;
}


// Temporary subdivision code. I'm told this is terrible.
void psxsplash::Renderer::iterativeSubdivideAndRender(const Tri &initialTri,
                                                      const eastl::array<psyqo::Vertex, 3> &initialProj, int zIndex,
                                                      int maxIterations) {
    struct Subdiv {
        Tri tri;
        eastl::array<psyqo::Vertex, 3> proj;
        int iterations;
    };

    // Reserve space knowing the max subdivisions (for maxIterations=3, max elements are small)
    eastl::vector<Subdiv> stack;
    stack.reserve(16);
    stack.push_back({initialTri, initialProj, maxIterations});

    while (!stack.empty()) {
        Subdiv s = stack.back();
        stack.pop_back();

        uint16_t minX = eastl::min({s.proj[0].x, s.proj[1].x, s.proj[2].x});
        uint16_t maxX = eastl::max({s.proj[0].x, s.proj[1].x, s.proj[2].x});
        uint16_t minY = eastl::min({s.proj[0].y, s.proj[1].y, s.proj[2].y});
        uint16_t maxY = eastl::max({s.proj[0].y, s.proj[1].y, s.proj[2].y});
        uint16_t width = maxX - minX;
        uint16_t height = maxY - minY;

        // Base case: small enough or no iterations left.
        if (s.iterations == 0 || (width < 2048 && height < 1024)) {
            auto &balloc = m_ballocs[m_gpu.getParity()];
            auto &prim = balloc.allocateFragment<psyqo::Prim::GouraudTexturedTriangle>();

            prim.primitive.pointA = s.proj[0];
            prim.primitive.pointB = s.proj[1];
            prim.primitive.pointC = s.proj[2];
            prim.primitive.uvA = s.tri.uvA;
            prim.primitive.uvB = s.tri.uvB;
            prim.primitive.uvC = s.tri.uvC;
            prim.primitive.tpage = s.tri.tpage;
            psyqo::PrimPieces::ClutIndex clut(s.tri.clutX, s.tri.clutY);
            prim.primitive.clutIndex = clut;
            prim.primitive.setColorA(s.tri.colorA);
            prim.primitive.setColorB(s.tri.colorB);
            prim.primitive.setColorC(s.tri.colorC);
            prim.primitive.setOpaque();

            m_ots[m_gpu.getParity()].insert(prim, zIndex);
            continue;
        }

        // Compute midpoint between projected[0] and projected[1].
        psyqo::Vertex mid;
        mid.x = (s.proj[0].x + s.proj[1].x) >> 1;
        mid.y = (s.proj[0].y + s.proj[1].y) >> 1;

        // Interpolate UV and color.
        psyqo::PrimPieces::UVCoords newUV;
        newUV.u = (s.tri.uvA.u + s.tri.uvB.u) / 2;
        newUV.v = (s.tri.uvA.v + s.tri.uvB.v) / 2;
        psyqo::Color newColor = averageColor(s.tri.colorA, s.tri.colorB);

        // Prepare new projected vertices.
        eastl::array<psyqo::Vertex, 3> projA = {s.proj[0], mid, s.proj[2]};
        eastl::array<psyqo::Vertex, 3> projB = {mid, s.proj[1], s.proj[2]};

        // Construct new Tris
        Tri triA = s.tri;
        triA.uvB = newUV;
        triA.colorB = newColor;

        Tri triB = s.tri;
        triB.uvA = newUV;
        triB.colorA = newColor;

        // Push new subdivisions on stack.
        stack.push_back({triA, projA, s.iterations - 1});
        stack.push_back({triB, projB, s.iterations - 1});
    }
}

void psxsplash::Renderer::vramUpload(const uint16_t *imageData, int16_t posX, int16_t posY, int16_t width,
                                     int16_t height) {
    psyqo::Rect uploadRect{.a = {.x = posX, .y = posY}, .b = {width, height}};
    m_gpu.uploadToVRAM(imageData, uploadRect);
}