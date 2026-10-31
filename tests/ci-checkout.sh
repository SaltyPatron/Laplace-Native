#!/usr/bin/env bash
# Verify actual Git cleanup and Ninja invalidation across Native CI jobs.
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
scratch=$(mktemp -d "${TMPDIR:?Declare CI scratch}/native-checkout.XXXXXXXX")
trap 'rm -rf -- "$scratch"' EXIT
repository="$scratch/candidate"
origin="$scratch/dependency"
mkdir "$repository" "$origin"
for path in "$repository" "$origin"; do
  git -C "$path" init -q
  git -C "$path" config user.name 'Checkout fixture'
  git -C "$path" config user.email fixture@example.invalid
done
printf 'int value(void) { return 1; }\n' > "$origin/value.c"
git -C "$origin" add value.c
git -C "$origin" commit -qm initial
cat > "$repository/CMakeLists.txt" <<'CMAKE'
cmake_minimum_required(VERSION 3.24)
project(CheckoutFixture C)
add_executable(checkout main.c .engine/Laplace-Engine/value.c)
CMAKE
printf '#include <stdio.h>\nextern int value(void);\nint main(void) { printf("%%d\\n", value()); return 0; }\n' > "$repository/main.c"
git -C "$repository" add CMakeLists.txt main.c
git -C "$repository" commit -qm initial
mkdir "$repository/.engine" "$repository/.operations"
git clone -q "$origin" "$repository/.engine/Laplace-Engine"
git clone -q "$origin" "$repository/.operations/Laplace-Operations"
cmake -S "$repository" -B "$scratch/build" -G Ninja > "$scratch/configure.log"
cmake --build "$scratch/build" > "$scratch/first.log"
[[ $("$scratch/build/checkout") == 1 ]]
before=$(stat -c '%i %y' "$repository/.engine/Laplace-Engine/value.c" "$repository/.operations/Laplace-Operations/value.c")
mkdir "$repository/.engine/obsolete" "$repository/.operations/obsolete"
printf 'unselected source\n' > "$repository/.engine/obsolete/stray.c"
printf 'unselected configuration\n' > "$repository/.operations/obsolete/stray.c"
printf 'dirty tracked source\n' > "$repository/main.c"
export GITHUB_ACTIONS=true GITHUB_WORKSPACE="$repository"
GITHUB_SHA=$(git -C "$repository" rev-parse HEAD)
export GITHUB_SHA
prepare() { (cd "$repository" && bash "$root/scripts/ci-checkout.sh"); }
prepare
[[ ! -e "$repository/.engine/obsolete" && ! -e "$repository/.operations/obsolete" ]]
[[ $(stat -c '%i %y' "$repository/.engine/Laplace-Engine/value.c" "$repository/.operations/Laplace-Operations/value.c") == "$before" ]]
git -C "$repository" diff --exit-code
cmake --build "$scratch/build" > "$scratch/repaired.log"
prepare
for path in "$repository/.engine/Laplace-Engine" "$repository/.operations/Laplace-Operations"; do
  git -C "$path" reset --hard HEAD
  git -C "$path" clean -ffdx
done
cmake --build "$scratch/build" | tee "$scratch/second.log"
grep -Fq 'ninja: no work to do.' "$scratch/second.log"
echo 'PASS repeated job cleanup preserves pinned inputs and an unchanged build'
printf 'int value(void) { return 2; }\n' > "$origin/value.c"
git -C "$origin" commit -qam changed
git -C "$repository/.engine/Laplace-Engine" fetch -q origin
git -C "$repository/.engine/Laplace-Engine" checkout -q --detach FETCH_HEAD
cmake --build "$scratch/build" > "$scratch/changed.log"
[[ $("$scratch/build/checkout") == 2 ]]
echo 'PASS changed dependency bytes rebuild and execute the new code'
printf 'must survive refusal\n' > "$repository/refusal-witness"
if GITHUB_SHA=0000000000000000000000000000000000000000 prepare > "$scratch/refusal.log" 2>&1; then
  echo 'Wrong revision was accepted' >&2; exit 1
fi
[[ -f "$repository/refusal-witness" ]]
mv "$repository/.operations/Laplace-Operations" "$scratch/redirected"
ln -s "$scratch/redirected" "$repository/.operations/Laplace-Operations"
if prepare > "$scratch/redirect.log" 2>&1; then
  echo 'Redirected dependency path was accepted' >&2; exit 1
fi
[[ -f "$repository/refusal-witness" && -f "$scratch/redirected/value.c" ]]
echo 'PASS wrong revisions and redirected paths refuse cleanup before mutation'
