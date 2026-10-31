#!/usr/bin/env bash
set -euo pipefail
root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
scratch=$(mktemp -d "${TMPDIR:?CI scratch required}/prepared-inputs.XXXXXXXX")
trap 'rm -rf -- "$scratch"' EXIT
export GITHUB_ACTIONS=true LAPLACE_BUILD="$scratch/build"
mkdir -p "$scratch/run-one" "$scratch/run-two"
for name in manifest.json tier0.bin tier0.flags tier0.flags.layout tier0.highway tier0.highway.nodes; do
  printf 'verified fixture %s\n' "$name" > "$scratch/run-one/$name"
done
cp -a "$scratch/run-one/." "$scratch/run-two/"
select_inputs() { LAPLACE_TIER0="$1/tier0.bin" bash "$root/scripts/ci-prepared-inputs.sh"; }
first=$(select_inputs "$scratch/run-one")
before=$(stat -c '%i %y' "$first/tier0.bin")
second=$(select_inputs "$scratch/run-two")
[[ $first == "$second" && $(stat -c '%i %y' "$first/tier0.bin") == "$before" ]]
cmp "$scratch/run-two/tier0.highway.nodes" "$second/tier0.highway.nodes"
echo 'PASS different run directories reuse one unchanged complete closure'
mkdir "$scratch/project"
cat > "$scratch/project/CMakeLists.txt" <<'CMAKE'
cmake_minimum_required(VERSION 3.24)
project(PreparedFixture C)
add_executable(prepared main.c)
target_compile_definitions(prepared PRIVATE PREPARED_INPUT="${TIER0}")
CMAKE
printf '#include <stdio.h>\nint main(void) { puts(PREPARED_INPUT); return 0; }\n' > "$scratch/project/main.c"
cmake -S "$scratch/project" -B "$scratch/compile" -G Ninja "-DTIER0=$first/tier0.bin" > "$scratch/configure-one.log"
cmake --build "$scratch/compile" > "$scratch/build-one.log"
cmake -S "$scratch/project" -B "$scratch/compile" "-DTIER0=$second/tier0.bin" > "$scratch/configure-two.log"
cmake --build "$scratch/compile" > "$scratch/build-two.log"
grep -Fq 'ninja: no work to do.' "$scratch/build-two.log"
echo 'PASS another run ID preserves native compile inputs and a no-work build'
printf 'different sidecar\n' > "$scratch/run-two/tier0.highway.nodes"
changed=$(select_inputs "$scratch/run-two")
[[ $changed != "$first" ]]
cmp "$scratch/run-two/tier0.highway.nodes" "$changed/tier0.highway.nodes"
echo 'PASS sidecar content is part of the prepared identity'
printf 'corrupt\n' > "$changed/tier0.bin"
if select_inputs "$scratch/run-two" > "$scratch/refusal.log" 2>&1; then echo 'Corrupted cache accepted' >&2; exit 1; fi
rm -rf -- "$changed"
echo 'PASS a mismatched cache is refused'
mkdir -p "$LAPLACE_BUILD/Laplace-Native/gcc-release" "$LAPLACE_BUILD/Laplace-Native/prepared/unowned"
printf 'LAPLACE_TIER0:PATH=%s/tier0.bin\n' "$first" > "$LAPLACE_BUILD/Laplace-Native/gcc-release/CMakeCache.txt"
for generation in 1 2 3 4 5 6; do
  printf 'generation %s\n' "$generation" > "$scratch/run-two/tier0.bin"
  select_inputs "$scratch/run-two" > "$scratch/selection"
done
[[ -f "$first/tier0.bin" && -d "$LAPLACE_BUILD/Laplace-Native/prepared/unowned" ]]
count=$(find "$LAPLACE_BUILD/Laplace-Native/prepared" -mindepth 1 -maxdepth 1 -type d ! -name unowned | wc -l)
[[ $count -le 5 ]]
echo 'PASS configured compiler inputs and unowned directories survive bounded retention'
