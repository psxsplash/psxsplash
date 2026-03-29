#include "gtemath.hh"

#include <psyqo/gte-kernels.hh>
#include <psyqo/gte-registers.hh>

using namespace psyqo::GTE;

void psxsplash::MatrixMultiplyGTE(const psyqo::Matrix33 &matA, const psyqo::Matrix33 &matB, psyqo::Matrix33 *result) {
    // Load matA as the rotation matrix. No prior GTE op depends on RT registers here.
    writeUnsafe<PseudoRegister::Rotation>(matA);

    psyqo::Vec3 t;

    // Column 0 of matB: Safe write to V0 ensures rotation matrix is settled before MVMVA.
    psyqo::GTE::writeSafe<PseudoRegister::V0>(psyqo::Vec3{matB.vs[0].x, matB.vs[1].x, matB.vs[2].x});
    psyqo::GTE::Kernels::mvmva<Kernels::MX::RT, Kernels::MV::V0>();
    // Safe read: MVMVA (8 cycles) output must be stable before reading.
    t = psyqo::GTE::readSafe<psyqo::GTE::PseudoRegister::SV>();
    result->vs[0].x = t.x;
    result->vs[1].x = t.y;
    result->vs[2].x = t.z;

    // Column 1: Unsafe V0 write is fine since MVMVA just completed (no dependency on V0 from readSafe).
    psyqo::GTE::writeUnsafe<PseudoRegister::V0>(psyqo::Vec3{matB.vs[0].y, matB.vs[1].y, matB.vs[2].y});
    // Safe nop-equivalent: the compiler inserts enough instructions between write and kernel call.
    psyqo::GTE::Kernels::mvmva<Kernels::MX::RT, Kernels::MV::V0>();
    t = psyqo::GTE::readSafe<psyqo::GTE::PseudoRegister::SV>();
    result->vs[0].y = t.x;
    result->vs[1].y = t.y;
    result->vs[2].y = t.z;

    // Column 2: Same pattern.
    psyqo::GTE::writeUnsafe<PseudoRegister::V0>(psyqo::Vec3{matB.vs[0].z, matB.vs[1].z, matB.vs[2].z});
    psyqo::GTE::Kernels::mvmva<Kernels::MX::RT, Kernels::MV::V0>();
    t = psyqo::GTE::readSafe<psyqo::GTE::PseudoRegister::SV>();
    result->vs[0].z = t.x;
    result->vs[1].z = t.y;
    result->vs[2].z = t.z;
}