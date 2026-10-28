# Laplace-Native

The library with the shared code of Laplace: identity, UTF-8, tier 0, fixed-point coordinates, composition, the decomposition of text (UAX #29), geometry packing, Hilbert order, trajectory matching, shape measures (Fréchet, Fréchet with outliers skipped, DTW, EDR), consensus (Glicko-2 with signed trust), and the pull's search kernel. Laplace-postgres and Laplace-Engine build it as part of themselves and keep no copy of anything in it. Every function is deterministic across CPUs and compilers, and every SIMD level is compiled in and chosen at run time.

| Library | Holds | Needs |
| --- | --- | --- |
| Windows process bytes | `cmake/LaplaceWindowsUtf8.cmake`: `laplace_utf8_executable` | Executable target; UTF-8 arguments and narrow filesystem input before C startup |
| `laplace` (static and shared) | everything but the three below | BLAKE3 |
| `laplace_text` | the decomposition of text | ICU 78 (Unicode 17) |
| `laplace_model` | the model-decomposition kernel | MKL, OpenMP |
| `laplace_linalg` | Laplacian eigenmaps, Procrustes, Gram-Schmidt (`laplace/linalg.h`) | Eigen 3.4, Spectra 1.x (`LAPLACE_EIGEN_DIR`, `LAPLACE_SPECTRA_DIR`) |

The programs are in [Laplace-Engine](https://github.com/SaltyPatron/Laplace-Engine). The documentation is [Laplace-Wiki](https://github.com/SaltyPatron/Laplace-Wiki), published at <https://saltypatron.github.io/Laplace-Wiki/>.

## Shared interfaces, from codepoints upward

These are dependency layers, not additional semantic tiers. Recipes still own
the domain's tier boundaries. Public contracts are in `include/laplace/`;
Engine, PostgreSQL and monorepo adapters call these owners.

| Layer | Shared interface | What varies at the caller |
| --- | --- | --- |
| Unicode floor | `lp_utf8_next`, `lp_id_codepoint`, `lp_tier0_at/find` | UTF-8 input or a scalar/ID; the validated mapped cache |
| Ordered identity | `lp_id_compose`, `lp_id_compose_runs` | Contiguous children or a run reader; exact expanded count |
| Ordered occurrence pairs | `lp_id_run_pairs` | Compressed positive runs, offset range, multiplicity callback; no codec or vocabulary policy |
| DAG dependency order | `lp_dag_levels` | Indexed child reader and edge distance; ordinary depth or barriers between transaction owners |
| Placement and composition | `lp_ref_compose`, `lp_ref_compose_runs`, `lp_coord_mean`, `lp_hilbert4` | Child references, positive repeat counts, admitted tier |
| Text boundaries | `lp_text_new`, `lp_text_id`, text decomposition | Thread-owned ICU context, input bytes, composition sink |
| Physicality | `lp_path`, `lp_vertex`, `lp_path_decode/expand`, `lp_ewkb_runs_spare` | Borrowed carrier, output capacity, occurrence annotations |
| Sets and working memory | `lp_idmap`, `lp_strmap`, `lp_vec`, `lp_buf`, `lp_allocator` | Value size, caller context, allocator and lifetime |
| Native calculations | Path matching, shape measures, `lp_frontier`, consensus/series | Explicit operands and policy; CPU dispatch stays inside the kernel |
| Host adapters | PostgreSQL fmgr/SPI, Engine libpq/COPY, monorepo C ABI | Storage access, authorization, cancellation, transport and result shape |

Windows executables embed one shared [UTF-8 process manifest](https://learn.microsoft.com/windows/apps/design/globalizing/use-utf8-code-page).
The setting applies before the CRT converts arguments. Console encoding changes
inside `main` cannot recover characters already replaced at startup. Libraries
inherit their host's process contract.

Identity takes ordered child IDs and repeats only. Coordinates use exact integer
accumulation; occurrence annotations and tier do not salt identity. Singleton
composition is the child. `lp_id_compose_runs` reads one `(ID, repeat count)` at
a time, verifies the exact expanded count and end of input, and hashes with
bounded stack storage. It never allocates an expanded child array. Native
reference composition and the monorepo `hash128_merkle_runs` adapter use this
same implementation. Empty ID sequences retain `lp_id_compose`'s empty hash;
an empty `lp_ref` remains the existing absent-reference value, not an entity.

Carrier views borrow their input. Copy anything retained beyond that input's
lifetime. Read a compressed vertex with `lp_path_decode`; expand runs only when
the consumer actually requires occurrences. Maps deduplicate content IDs while
paths preserve ordered occurrences. A set result and an occurrence result are
different contracts even when they share a kernel.

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
