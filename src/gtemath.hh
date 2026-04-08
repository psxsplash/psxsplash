#pragma once

#include <psyqo/matrix.hh>

namespace psxsplash {
void MatrixMultiplyGTE(const psyqo::Matrix33 &matA, const psyqo::Matrix33 &matB, psyqo::Matrix33 *result);

/// Transpose a 3x3 matrix (swap rows and columns).
inline psyqo::Matrix33 transposeMatrix33(const psyqo::Matrix33 &m) {
    psyqo::Matrix33 r;
    r.vs[0].x = m.vs[0].x; r.vs[0].y = m.vs[1].x; r.vs[0].z = m.vs[2].x;
    r.vs[1].x = m.vs[0].y; r.vs[1].y = m.vs[1].y; r.vs[1].z = m.vs[2].y;
    r.vs[2].x = m.vs[0].z; r.vs[2].y = m.vs[1].z; r.vs[2].z = m.vs[2].z;
    return r;
}
}  // namespace psxsplash
