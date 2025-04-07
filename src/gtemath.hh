#pragma once
#include <psyqo/matrix.hh>

namespace psxsplash {
void MatrixMultiplyGTE(const psyqo::Matrix33 &matA, const psyqo::Matrix33 &matB, psyqo::Matrix33 *result);
};
