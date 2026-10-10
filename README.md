# Laplace-Native

The library with the shared code of Laplace: identity, UTF-8, tier 0, fixed-point coordinates, composition, the decomposition of text (UAX #29), geometry packing, Hilbert order, trajectory matching, shape measures (Fréchet, Fréchet with outliers skipped, DTW, EDR), consensus (Glicko-2 with signed trust), and the pull's search kernel. Laplace-postgres and Laplace-Engine build it as part of themselves and keep no copy of anything in it. Every function is deterministic across CPUs and compilers, and every SIMD level is compiled in and chosen at run time.

| Library | Holds | Needs |
| --- | --- | --- |
| `laplace` (static and shared) | everything but the three below | BLAKE3 |
| `laplace_text` | the decomposition of text | ICU 78 (Unicode 17) |
| `laplace_model` | the model-decomposition kernel | MKL, OpenMP |
| `laplace_linalg` | Laplacian eigenmaps, Procrustes, Gram-Schmidt (`laplace/linalg.h`) | Eigen 3.4, Spectra 1.x (`LAPLACE_EIGEN_DIR`, `LAPLACE_SPECTRA_DIR`) |

The programs are in [Laplace-Engine](https://github.com/SaltyPatron/Laplace-Engine). The documentation is [Laplace-Wiki](https://github.com/SaltyPatron/Laplace-Wiki), published at <https://saltypatron.github.io/Laplace-Wiki/>.

## Build

```sh
source ../Laplace-Engine/laplace.env
cmake --preset icx-release && cmake --build --preset icx-release && ctest --preset icx-release
cmake --preset gcc-release && cmake --build --preset gcc-release && ctest --preset gcc-release   # determinism cross-check
```

`cmake/LaplacePaths.cmake` is the one definition of where Laplace's dependencies and data are; each value comes from the environment when it is set there.

## Checks

CI first builds and tests with Intel and GCC on the local verification host. On main, it then acquires an
independent lease and starts the configured cloud worker, executes the registered dispatch tests on AVX-512
hardware, and rejects missing, skipped or failed tests. The exact prepared Tier-0, flags and Highway inputs that
passed the local tests travel with the run and are checked against their SHA-256 manifest on cloud. The hardware
comparison therefore does not depend on which knowledge artifacts happen to be installed on that worker.
Deployment to the component installation is dispatched
only after both stages pass. The lease is released even on failure; the worker's installed idle timer stops it
when no other operation is active. JUnit reports and the cloud CPU description are retained with each run.

To repeat this verification without deploying a revision, run
`gh workflow run laplace.yml --repo SaltyPatron/Laplace-Native --ref main`, then
`gh run list --repo SaltyPatron/Laplace-Native`. The cloud target is declared by repository variables installed
through Operations' `cloud/configure-ci.ps1`; it is independent of the public application's production host.

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
