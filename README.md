# Laplace-Native

The shared native library of Laplace: identity, fixed-point coordinates, geometry packing, Hilbert order, trajectory matching, and consensus. Laplace-postgres and Laplace-Engine call it. Every function is deterministic across CPUs and compilers, and every SIMD level is compiled in and chosen at run time.

## Build

```sh
source /repos/src/toolchain.env          # Intel oneAPI: icx, MKL, TBB
cmake --preset icx-release && cmake --build --preset icx-release && ctest --preset icx-release
cmake --preset gcc-release && cmake --build --preset gcc-release && ctest --preset gcc-release   # determinism cross-check
```

CMake 3.24 or newer is needed for icx's C++ features. Build trees go to `/repos/build/Laplace-Native/<preset>`.

## Tests

The golden tests reproduce what the Laplace prototype established:

- every codepoint's ID and Hilbert value against the tier-0 table
- the wall
- packing round-trips
- continuation matching at every dispatch level (`LAPLACE_ISA=scalar|sse2|avx2`)
- Glickman's worked Glicko-2 example
