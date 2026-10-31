# Where everything Laplace builds against and runs with is. One definition, shared by Laplace-Native, Laplace-postgres,
# and Laplace-Engine: each value comes from the environment (laplace.env) when it is set there, else from the default
# here on each configure. Explicit -D/cache or parent selections override both. Defaults are normal variables:
# caching an environment-derived default would silently pin it after the environment changed. Existing cache
# selections remain authoritative until explicitly cleared with -U. Every path is kept with forward slashes, which every platform
# accepts: one compiled into C as a string (LP_TIER0_DEFAULT) must not carry backslash escapes.
macro(laplace_path var default doc)
  if(DEFINED CACHE{${var}})
    # Type only an existing explicit cache input: CMake then normalizes an
    # untyped relative -D path against the configure command's working directory.
    set(${var} "${${var}}" CACHE PATH "${doc}")
  endif()
  if(DEFINED ${var})
    file(TO_CMAKE_PATH "${${var}}" _lp_path)
  elseif(DEFINED ENV{${var}} AND NOT "$ENV{${var}}" STREQUAL "")
    file(TO_CMAKE_PATH "$ENV{${var}}" _lp_path)
  else()
    file(TO_CMAKE_PATH "${default}" _lp_path)
  endif()
  set(${var} "${_lp_path}")
endmacro()

laplace_path(LAPLACE_SRC        "/repos/src"                      "source root: Laplace's repositories and its dependencies'")
laplace_path(LAPLACE_DEPS       "/repos/deps"                     "installed dependencies")
laplace_path(LAPLACE_TIER0      "/repos/work/tier0/tier0.bin"     "the tier-0 perf-cache")
laplace_path(LAPLACE_GRAMMARS   "/repos/build/grammars"           "compiled tree-sitter grammars")
laplace_path(LAPLACE_ICU_DIR    "${LAPLACE_DEPS}/icu78"           "ICU 78 install (Unicode 17)")
laplace_path(LAPLACE_PG_DIR     "/usr/local/pgsql"                "PostgreSQL install")
laplace_path(LAPLACE_BLAKE3_DIR "${LAPLACE_SRC}/blake3/c"         "BLAKE3 C sources")
laplace_path(LAPLACE_COREMATH   "${LAPLACE_SRC}/core-math"        "CORE-MATH sources")
laplace_path(LAPLACE_OPERATIONS "${LAPLACE_SRC}/Laplace-Operations" "Laplace-Operations: the floating-point contract and laplace-math")
laplace_path(LAPLACE_TREESITTER "${LAPLACE_SRC}/tree-sitter"      "tree-sitter runtime")
laplace_path(LAPLACE_UCD        "/vault/Data/UCD"                 "the Unicode data tier 0 is generated from")
