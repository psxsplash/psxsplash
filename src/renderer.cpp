#include "renderer.hh"

#include <EASTL/vector.h>

#include <cstdint>
#include <psyqo/fixed-point.hh>
#include <psyqo/gte-kernels.hh>
#include <psyqo/gte-registers.hh>
#include <psyqo/kernel.hh>
#include <psyqo/primitives/common.hh>
#include <psyqo/primitives/triangles.hh>
#include <psyqo/vector.hh>

using namespace psyqo::fixed_point_literals;
using namespace psyqo::trig_literals;

psxsplash::Renderer *psxsplash::Renderer::instance = nullptr;

void psxsplash::Renderer::init(psyqo::GPU &gpuInstance) {
    psyqo::Kernel::assert(instance == nullptr, "A second intialization of Renderer was tried");

    psyqo::GTE::clear<psyqo::GTE::Register::TRX, psyqo::GTE::Unsafe>();
    psyqo::GTE::clear<psyqo::GTE::Register::TRY, psyqo::GTE::Unsafe>();
    psyqo::GTE::clear<psyqo::GTE::Register::TRZ, psyqo::GTE::Unsafe>();

    psyqo::GTE::write<psyqo::GTE::Register::OFX, psyqo::GTE::Unsafe>(psyqo::FixedPoint<16>(160.0).raw());
    psyqo::GTE::write<psyqo::GTE::Register::OFY, psyqo::GTE::Unsafe>(psyqo::FixedPoint<16>(120.0).raw());

    psyqo::GTE::write<psyqo::GTE::Register::H, psyqo::GTE::Unsafe>(120);

    psyqo::GTE::write<psyqo::GTE::Register::ZSF3, psyqo::GTE::Unsafe>(ORDERING_TABLE_SIZE / 3);
    psyqo::GTE::write<psyqo::GTE::Register::ZSF4, psyqo::GTE::Unsafe>(ORDERING_TABLE_SIZE / 4);

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

    eastl::array<psyqo::Vertex, 3> projected;

    m_gpu.getNextClear(clear.primitive, m_clearcolor);
    m_gpu.chain(clear);

    balloc.reset();

    for (auto &obj : objects) {
        for (int i = 0; i < obj->polyCount; i++) {
            Tri &tri = obj->polygons[i];
            psyqo::Vec3 result;

            psyqo::GTE::writeUnsafe<psyqo::GTE::PseudoRegister::Rotation>(obj->rotation);
            psyqo::GTE::write<psyqo::GTE::Register::TRX, psyqo::GTE::Unsafe>(obj->position.x.raw() +
                                                                             m_currentCamera->getPosition().x.raw());
            psyqo::GTE::write<psyqo::GTE::Register::TRY, psyqo::GTE::Unsafe>(obj->position.y.raw() +
                                                                             m_currentCamera->getPosition().y.raw());
            psyqo::GTE::write<psyqo::GTE::Register::TRZ, psyqo::GTE::Safe>(obj->position.z.raw() +
                                                                           m_currentCamera->getPosition().z.raw());

            psyqo::GTE::writeSafe<psyqo::GTE::PseudoRegister::V0>(tri.v0);
            psyqo::GTE::writeSafe<psyqo::GTE::PseudoRegister::V1>(tri.v1);
            psyqo::GTE::writeSafe<psyqo::GTE::PseudoRegister::V2>(tri.v2);

            psyqo::GTE::Kernels::mvmva<psyqo::GTE::Kernels::MX::RT, psyqo::GTE::Kernels::MV::V0,
                                       psyqo::GTE::Kernels::TV::TR>();
            result = psyqo::GTE::readSafe<psyqo::GTE::PseudoRegister::SV>();
            psyqo::GTE::writeSafe<psyqo::GTE::PseudoRegister::V0>(result);

            psyqo::GTE::Kernels::mvmva<psyqo::GTE::Kernels::MX::RT, psyqo::GTE::Kernels::MV::V1,
                                       psyqo::GTE::Kernels::TV::TR>();
            result = psyqo::GTE::readSafe<psyqo::GTE::PseudoRegister::SV>();
            psyqo::GTE::writeSafe<psyqo::GTE::PseudoRegister::V1>(result);

            psyqo::GTE::Kernels::mvmva<psyqo::GTE::Kernels::MX::RT, psyqo::GTE::Kernels::MV::V2,
                                       psyqo::GTE::Kernels::TV::TR>();
            result = psyqo::GTE::readSafe<psyqo::GTE::PseudoRegister::SV>();
            psyqo::GTE::writeSafe<psyqo::GTE::PseudoRegister::V2>(result);

            psyqo::GTE::writeUnsafe<psyqo::GTE::PseudoRegister::Rotation>(m_currentCamera->getRotation());
            psyqo::GTE::clear<psyqo::GTE::Register::TRX, psyqo::GTE::Unsafe>();
            psyqo::GTE::clear<psyqo::GTE::Register::TRY, psyqo::GTE::Unsafe>();
            psyqo::GTE::clear<psyqo::GTE::Register::TRZ, psyqo::GTE::Unsafe>();

            psyqo::GTE::Kernels::rtpt();
            psyqo::GTE::Kernels::nclip();

            int32_t mac0 = 0;
            psyqo::GTE::read<psyqo::GTE::Register::MAC0>(reinterpret_cast<uint32_t *>(&mac0));
            if (mac0 <= 0) continue;

            int32_t zIndex = 0;
            uint32_t sz0, sz1, sz2;
            psyqo::GTE::read<psyqo::GTE::Register::SZ0>(&sz0);
            psyqo::GTE::read<psyqo::GTE::Register::SZ1>(&sz1);
            psyqo::GTE::read<psyqo::GTE::Register::SZ2>(&sz2);

            zIndex = eastl::max(eastl::max(sz0, sz1), sz2);

            // psyqo::GTE::read<psyqo::GTE::Register::OTZ>(reinterpret_cast<uint32_t *>(&zIndex));

            if (zIndex < 0 || zIndex >= ORDERING_TABLE_SIZE) continue;

            psyqo::GTE::read<psyqo::GTE::Register::SXY0>(&projected[0].packed);
            psyqo::GTE::read<psyqo::GTE::Register::SXY1>(&projected[1].packed);
            psyqo::GTE::read<psyqo::GTE::Register::SXY2>(&projected[2].packed);

            auto &prim = balloc.allocateFragment<psyqo::Prim::GouraudTexturedTriangle>();

            psyqo::PrimPieces::ClutIndex clut(obj->clutX, obj->clutY);

            prim.primitive.pointA = projected[0];
            prim.primitive.pointB = projected[1];
            prim.primitive.pointC = projected[2];
            prim.primitive.uvA = tri.uvA;
            prim.primitive.uvB = tri.uvB;
            prim.primitive.uvC = tri.uvC;
            prim.primitive.tpage = obj->texture;
            prim.primitive.clutIndex = clut;
            prim.primitive.setColorA(tri.colorA);
            prim.primitive.setColorB(tri.colorB);
            prim.primitive.setColorC(tri.colorC);

            prim.primitive.setOpaque();

            ot.insert(prim, zIndex);
        }
    }

    m_gpu.chain(ot);
}

void psxsplash::Renderer::vramUpload(const uint16_t *imageData, int16_t posX, int16_t posY, int16_t width,
                                     int16_t height) {
    psyqo::Rect uploadRect{.a = {.x = posX, .y = posY}, .b = {width, height}};
    m_gpu.uploadToVRAM(imageData, uploadRect);
}