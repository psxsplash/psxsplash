#include "gtemath.hh"

#include <psyqo/gte-kernels.hh>
#include <psyqo/gte-registers.hh>

using namespace psyqo::GTE;

void psxsplash::matrixMultiplyGTE(const psyqo::Matrix33 &matA, const psyqo::Matrix33 &matB, psyqo::Matrix33 *result) {
    writeSafe<PseudoRegister::Rotation>(matA);

    psyqo::Vec3 t;

    psyqo::GTE::writeSafe<PseudoRegister::V0>(psyqo::Vec3{matB.vs[0].x, matB.vs[1].x, matB.vs[2].x});

    psyqo::GTE::Kernels::mvmva<Kernels::MX::RT, Kernels::MV::V0>();

    t = psyqo::GTE::readSafe<psyqo::GTE::PseudoRegister::SV>();
    result->vs[0].x = t.x;
    result->vs[1].x = t.y;
    result->vs[2].x = t.z;

    psyqo::GTE::writeSafe<PseudoRegister::V0>(psyqo::Vec3{matB.vs[0].y, matB.vs[1].y, matB.vs[2].y});

    psyqo::GTE::Kernels::mvmva<Kernels::MX::RT, Kernels::MV::V0>();
    t = psyqo::GTE::readSafe<psyqo::GTE::PseudoRegister::SV>();
    result->vs[0].y = t.x;
    result->vs[1].y = t.y;
    result->vs[2].y = t.z;

    psyqo::GTE::writeSafe<PseudoRegister::V0>(psyqo::Vec3{matB.vs[0].z, matB.vs[1].z, matB.vs[2].z});

    psyqo::GTE::Kernels::mvmva<Kernels::MX::RT, Kernels::MV::V0>();
    t = psyqo::GTE::readSafe<psyqo::GTE::PseudoRegister::SV>();
    result->vs[0].z = t.x;
    result->vs[1].z = t.y;
    result->vs[2].z = t.z;
}