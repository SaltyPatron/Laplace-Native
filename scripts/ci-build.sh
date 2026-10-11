#!/usr/bin/env bash
set -euo pipefail
preset=${1:?CMake preset is required}
case "$preset" in icx-release|gcc-release) ;; *) echo "Unsupported CI preset: $preset" >&2; exit 2 ;; esac
: "${LAPLACE_BUILD:?}" "${LAPLACE_OPERATIONS:?}"
tsv=$LAPLACE_OPERATIONS/dependencies.tsv
mkdir -p "$LAPLACE_BUILD/deps-src"
for name in eigen spectra; do
  row=$(awk -F'\t' -v n="$name" '$1 == n && $7 == "headers"' "$tsv")
  [ -n "$row" ] || { echo "$name: missing from the dependency manifest" >&2; exit 1; }
  origin=$(echo "$row" | cut -f4); pin=$(echo "$row" | cut -f6); dir=$LAPLACE_BUILD/deps-src/$name
  [ -d "$dir/.git" ] || git clone -q --filter=blob:none --no-checkout "$origin" "$dir"
  git -C "$dir" fetch -q origin "$pin"
  git -C "$dir" -c advice.detachedHead=false checkout -q "$pin"
  test "$(git -C "$dir" rev-parse HEAD)" = "$pin"
  export "LAPLACE_$(echo "$name" | tr a-z A-Z)_DIR=$dir"
done
if [[ "$preset" = icx-* ]]; then
  # Intel's environment script uses unset optional variables.
  set +u
  . /opt/intel/oneapi/setvars.sh >/dev/null 2>&1
  set -u
fi
export OMP_NUM_THREADS=${OMP_NUM_THREADS:-$(nproc)} MKL_NUM_THREADS=${MKL_NUM_THREADS:-$(nproc)}
: "${LAPLACE_TIER0:?The verification job must select its prepared inputs}"
cmake --preset "$preset" -DLAPLACE_TIER0="$LAPLACE_TIER0"
cmake --build --preset "$preset"
