#!/usr/bin/env bash
# Prepare the exact CI candidate without discarding unchanged pinned sources.
set -euo pipefail
root=$(git rev-parse --show-toplevel)
[[ ${GITHUB_ACTIONS:-} == true && -n ${GITHUB_WORKSPACE:-} && -n ${GITHUB_SHA:-} ]] || {
  echo 'Checkout preparation requires an Actions workspace and exact revision' >&2; exit 1;
}
[[ $(realpath -- "$GITHUB_WORKSPACE") == "$root" && $(git rev-parse HEAD) == "$GITHUB_SHA" ]] || {
  echo 'CI checkout does not match the selected workspace/revision' >&2; exit 1;
}
for path in .engine .engine/Laplace-Engine .operations .operations/Laplace-Operations; do
  [[ ! -L "$root/$path" ]] || {
    echo "CI dependency path may not redirect outside its checkout: $path" >&2; exit 1;
  }
done
git -C "$root" reset --hard HEAD
# Each dependency's own checkout action still resets and cleans its exact pin.
git -C "$root" clean -ffdx -e /.engine/Laplace-Engine/ -e /.operations/Laplace-Operations/ -- .
