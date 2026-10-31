#!/usr/bin/env bash
# Give a verified prepared closure a stable, immutable build path. Run IDs are
# provenance, not native compile inputs. Only this CI cache's marked entries retire.
set -euo pipefail
[[ ${GITHUB_ACTIONS:-} == true ]] || { echo 'Prepared build caching is a CI operation' >&2; exit 1; }
: "${LAPLACE_BUILD:?}" "${LAPLACE_TIER0:?}"
source_dir=$(dirname -- "$LAPLACE_TIER0")
cache_root="$LAPLACE_BUILD/Laplace-Native/prepared"
[[ $LAPLACE_BUILD == /* && $LAPLACE_BUILD != / ]] || { echo 'CI build root must be an absolute dedicated directory' >&2; exit 1; }
cursor=
IFS=/ read -r -a components <<< "$cache_root"
for component in "${components[@]}"; do
  [[ -n $component ]] || continue
  [[ $component != . && $component != .. ]] || { echo 'CI build root must be normalized' >&2; exit 1; }
  cursor="$cursor/$component"
  [[ ! -L "$cursor" ]] || { echo "Prepared cache parent may not be a link: $cursor" >&2; exit 1; }
done
marker=.laplace-prepared-cache
schema=laplace.native-prepared-cache/v1
payload_digest() {
  local directory=$1 name
  local -a names=()
  while IFS= read -r -d '' name; do
    [[ $name == "$marker" ]] && continue
    [[ -f "$directory/$name" && ! -L "$directory/$name" ]] || { echo "Unsupported prepared entry: $directory/$name" >&2; return 1; }
    names+=("$name")
  done < <(find "$directory" -mindepth 1 -maxdepth 1 -printf '%f\0' | LC_ALL=C sort -z)
  ((${#names[@]})) || { echo 'Empty prepared closure' >&2; return 1; }
  (cd "$directory" && sha256sum -- "${names[@]}" | sha256sum | cut -d' ' -f1)
}
for name in manifest.json tier0.bin tier0.flags tier0.flags.layout tier0.highway; do
  [[ -f "$source_dir/$name" && ! -L "$source_dir/$name" ]] || { echo "Missing prepared input: $source_dir/$name" >&2; exit 1; }
done
[[ ! -e "$source_dir/$marker" ]] || { echo 'Source contains a reserved cache marker' >&2; exit 1; }
key=$(payload_digest "$source_dir")
mkdir -p -- "$cache_root"
[[ ! -L "$cache_root" ]] || { echo 'Prepared cache root may not be a link' >&2; exit 1; }
destination="$cache_root/$key"
pending=
trap '[[ -z "$pending" ]] || rm -rf -- "$pending"' EXIT
if [[ ! -e "$destination" ]]; then
  pending=$(mktemp -d "$cache_root/.prepared.XXXXXXXX")
  cp -a --reflink=auto --no-preserve=ownership -- "$source_dir/." "$pending/"
  printf '%s\n%s\n' "$schema" "$key" > "$pending/$marker"
  if mv -T -- "$pending" "$destination"; then pending=; fi
fi
[[ -d "$destination" && ! -L "$destination" && -f "$destination/$marker" && ! -L "$destination/$marker" ]] || { echo 'Prepared cache entry is not owned by this helper' >&2; exit 1; }
[[ $(<"$destination/$marker") == "$schema"$'\n'"$key" && $(payload_digest "$destination") == "$key" ]] || { echo 'Prepared cache differs from the verified input closure' >&2; exit 1; }
touch -- "$destination/$marker"
# The Native workflow serializes runs. Retain all configured compiler profiles'
# selected closures, plus three other generations for rollback/debugging.
declare -A protected=(["$destination"]=1)
for profile in icx-release gcc-release icx-debug; do
  cache="$LAPLACE_BUILD/Laplace-Native/$profile/CMakeCache.txt"
  [[ -f "$cache" ]] || continue
  while IFS= read -r line; do
    [[ $line == LAPLACE_TIER0:*=* ]] || continue
    path=${line#*=}
    [[ $path == "$cache_root/"*/tier0.bin ]] && protected["${path%/*}"]=1
  done < "$cache"
done
recent=0
while IFS=$'\t' read -r -d '' timestamp name; do
  candidate="$cache_root/$name"
  [[ -z ${protected[$candidate]+x} ]] || continue
  recent=$((recent + 1))
  ((recent > 3)) || continue
  rm -rf -- "$candidate"
done < <(
  for candidate in "$cache_root"/*; do
    name=${candidate##*/}
    [[ $name =~ ^[0-9a-f]{64}$ && -d "$candidate" && ! -L "$candidate" && -f "$candidate/$marker" && ! -L "$candidate/$marker" ]] || continue
    [[ $(<"$candidate/$marker") == "$schema"$'\n'"$name" ]] || continue
    printf '%s\t%s\0' "$(stat -c %Y "$candidate/$marker")" "$name"
  done | LC_ALL=C sort -z -nr
)
printf '%s\n' "$destination"
