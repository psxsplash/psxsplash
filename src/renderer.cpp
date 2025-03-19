#include <cstdint>

#include "psyqo/primitives/triangles.hh"

#include "gameobject.hh"
#include "psyqo/soft-math.hh"
#include "renderer.hh"

void psxsplash::Renderer::initialize() {
    psyqo::GTE::clear<psyqo::GTE::Register::TRX, psyqo::GTE::Unsafe>();
    psyqo::GTE::clear<psyqo::GTE::Register::TRY, psyqo::GTE::Unsafe>();
    psyqo::GTE::clear<psyqo::GTE::Register::TRZ, psyqo::GTE::Unsafe>();

    psyqo::GTE::write<psyqo::GTE::Register::OFX, psyqo::GTE::Unsafe>(
        psyqo::FixedPoint<16>(160.0).raw());
    psyqo::GTE::write<psyqo::GTE::Register::OFY, psyqo::GTE::Unsafe>(
        psyqo::FixedPoint<16>(120.0).raw());

    psyqo::GTE::write<psyqo::GTE::Register::H, psyqo::GTE::Unsafe>(120);

    psyqo::GTE::write<psyqo::GTE::Register::ZSF3, psyqo::GTE::Unsafe>(
        ORDERING_TABLE_SIZE / 3);
    psyqo::GTE::write<psyqo::GTE::Register::ZSF4, psyqo::GTE::Unsafe>(
        ORDERING_TABLE_SIZE / 4);
}

void psxsplash::Renderer::render(eastl::array<GameObject> &objects) {
    uint8_t parity = m_gpu.getParity();

    auto &ot = m_ots[parity];
    auto &clear = m_clear[parity];
    auto &balloc = m_ballocs[parity];

    eastl::array<psyqo::Vertex, 3> projected;

    m_gpu.getNextClear(clear.primitive, m_clearcolor);
    m_gpu.chain(clear);

    balloc.reset();

    for (auto &obj : objects) {

        auto &mesh = obj.m_mesh;
        auto &texture = mesh.m_texture;

        psyqo::GTE::write<psyqo::GTE::Register::TRX, psyqo::GTE::Unsafe>(
            obj.m_pos.x.raw());
        psyqo::GTE::write<psyqo::GTE::Register::TRY, psyqo::GTE::Unsafe>(
            obj.m_pos.y.raw());
        psyqo::GTE::write<psyqo::GTE::Register::TRZ, psyqo::GTE::Unsafe>(
            obj.m_pos.z.raw());

        auto transform = psyqo::SoftMath::generateRotationMatrix33(
            obj.m_rot[0], psyqo::SoftMath::Axis::X, m_trig);
        auto rot = psyqo::SoftMath::generateRotationMatrix33(
            obj.m_rot[1], psyqo::SoftMath::Axis::Y, m_trig);

        psyqo::SoftMath::multiplyMatrix33(transform, rot, &transform);

        auto rotZ = psyqo::SoftMath::generateRotationMatrix33(
            obj.m_rot[2], psyqo::SoftMath::Axis::Z, m_trig);

        psyqo::SoftMath::multiplyMatrix33(transform, rotZ, &transform);

        psyqo::GTE::writeUnsafe<psyqo::GTE::PseudoRegister::Rotation>(
            transform);

        for (auto &tri : mesh.m_polygons) {

            psyqo::GTE::writeUnsafe<psyqo::GTE::PseudoRegister::V0>(tri.v0);
            psyqo::GTE::writeUnsafe<psyqo::GTE::PseudoRegister::V1>(tri.v1);
            psyqo::GTE::writeUnsafe<psyqo::GTE::PseudoRegister::V2>(tri.v2);

            psyqo::GTE::Kernels::rtpt();
            psyqo::GTE::Kernels::nclip();

            int32_t mac0 = 0;
            psyqo::GTE::read<psyqo::GTE::Register::MAC0>(
                reinterpret_cast<uint32_t *>(&mac0));
            if (mac0 <= 0)
                continue;

            psyqo::GTE::Kernels::avsz4();
            int32_t zIndex = 0;
            psyqo::GTE::read<psyqo::GTE::Register::OTZ>(
                reinterpret_cast<uint32_t *>(&zIndex));
            if (zIndex < 0 || zIndex >= ORDERING_TABLE_SIZE)
                continue;

            psyqo::GTE::read<psyqo::GTE::Register::SXY0>(&projected[1].packed);
            psyqo::GTE::read<psyqo::GTE::Register::SXY1>(&projected[2].packed);
            psyqo::GTE::read<psyqo::GTE::Register::SXY2>(&projected[3].packed);

            auto &prim = balloc.allocate<psyqo::Fragments::SimpleFragment<
                psyqo::Prim::TexturedTriangle>>();

            prim.primitive.pointA = projected[0];
            prim.primitive.pointB = projected[1];
            prim.primitive.pointC = projected[2];
            prim.primitive.uvA = tri.uvA;
            prim.primitive.uvB = tri.uvB;
            prim.primitive.uvC = tri.uvC;
            prim.primitive.tpage = texture.m_tpage;

            ot.insert(prim, zIndex);
        }
    }

    m_gpu.chain(ot);
}