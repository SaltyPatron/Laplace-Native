/* BMI2 Hilbert interleave: one pdep per axis deposits its 16 bits into every fourth bit. */
#include "../internal.h"
#include <immintrin.h>

uint64_t lp_hilbert4_interleave_bmi2(const uint64_t X[4]){
    const uint64_t lane = 0x1111111111111111ull;
    return _pdep_u64(X[0], lane << 3) | _pdep_u64(X[1], lane << 2) | _pdep_u64(X[2], lane << 1) | _pdep_u64(X[3], lane);
}
