# Laplace-Native

The library with the shared code of Laplace: identity, UTF-8, tier 0, fixed-point coordinates, composition, the decomposition of text (UAX #29), geometry packing, Hilbert order, trajectory matching, shape measures (Fréchet, Fréchet with outliers skipped, DTW, EDR), consensus (Glicko-2 with signed trust), and the pull's search kernel. Laplace-postgres and Laplace-Engine build it as part of themselves and keep no copy of anything in it. Every function is deterministic across CPUs and compilers, and every SIMD level is compiled in and chosen at run time.

| Library | Holds | Needs |
| --- | --- | --- |
| `laplace` (static and shared) | everything but the two below | BLAKE3 |
| `laplace_text` | the decomposition of text | ICU 78 (Unicode 17) |
| `laplace_model` | the model-decomposition kernel | MKL, OpenMP |

The programs are in [Laplace-Engine](https://github.com/SaltyPatron/Laplace-Engine). The documentation is [Laplace-Wiki](https://github.com/SaltyPatron/Laplace-Wiki), published at <https://saltypatron.github.io/Laplace-Wiki/>.

## Build

```sh
source ../Laplace-Engine/laplace.env
cmake --preset icx-release && cmake --build --preset icx-release && ctest --preset icx-release
cmake --preset gcc-release && cmake --build --preset gcc-release && ctest --preset gcc-release   # determinism cross-check
```

`cmake/LaplacePaths.cmake` is the one definition of where Laplace's dependencies and data are; each value comes from the environment when it is set there.

## Checks

The checks reproduce what the prototype established and what the documentation states:

- every codepoint's ID and Hilbert value against the tier-0 table
- the wall
- packing round-trips
- continuation matching, the shape measures' distance rows and the int8 dot, equal to the scalar path at every dispatch
  level (`LAPLACE_ISA=scalar|sse2|avx2|avxvnni|avx512|avx512vnni`); a level the CPU lacks is skipped, and the AVX-512
  levels run under Intel SDE (`-spr`) when `LAPLACE_SDE` names its `sde` executable
- Glickman's worked Glicko-2 example
- `[[S,h,e,r,l,o,c,k], ' ', [H,o,l,m,e,s]]`, and a word filling every tier above it
- what each shape measure does with a stray vertex and with a repeat
- the cost of a claim from its standing
