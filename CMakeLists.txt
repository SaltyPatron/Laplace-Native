cmake_minimum_required(VERSION 3.24)
project(LaplaceNative VERSION 0.2.0 LANGUAGES C CXX ASM)

# Laplace-Native: the library with the shared code, installed as the CMake package LaplaceNative
# (find_package(LaplaceNative CONFIG REQUIRED), targets Laplace::*). Every ISA is compiled in and chosen at run time.
set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)             # linked into PostgreSQL extensions
set(CMAKE_C_VISIBILITY_PRESET hidden)

include(cmake/LaplacePaths.cmake)
include(cmake/LaplaceFlags.cmake)

# laplace-math: CORE-MATH's correctly rounded functions under the libm names, built from the pinned CORE-MATH checkout
# under the same contract, and linked whole ahead of any libm by everything that links Native. One right answer on
# every CPU and compiler, which vendor libms (Intel's libimf dispatches by CPU) do not give. Tier 0 is generated with
# it, and so is the gcc build that checks results do not depend on the compiler. lp_crmath is its former name.
if(LP_FP_COMPILE)
  if(NOT EXISTS "${LAPLACE_COREMATH}/src/binary64/cos/cos.c")
    message(FATAL_ERROR "LAPLACE_COREMATH: no CORE-MATH checkout at '${LAPLACE_COREMATH}'")
  endif()
  # The standalone math project exposes CORE_MATH as an optional cache input.
  # Its empty cache placeholder must not pin Native's environment-derived path.
  if(NOT CORE_MATH)
    set(CORE_MATH "${LAPLACE_COREMATH}")
  endif()
  add_subdirectory(${LAPLACE_OPERATIONS}/math laplace-math EXCLUDE_FROM_ALL)
  add_library(lp_crmath ALIAS laplace_math_link)
endif()

# BLAKE3, built with this compiler and every SIMD variant (its own runtime dispatch).
set(BUILD_SHARED_LIBS OFF)
if(CMAKE_C_COMPILER_ID STREQUAL "IntelLLVM" AND NOT WIN32)   # BLAKE3's CMake knows gcc/clang/MSVC, not icx: same Unix assembly
                                                              # (on Windows icx is MSVC to it, and takes its MASM sources)
  set(BLAKE3_AMD64_ASM_SOURCES blake3_avx2_x86-64_unix.S blake3_avx512_x86-64_unix.S blake3_sse2_x86-64_unix.S blake3_sse41_x86-64_unix.S)
endif()
# Built and installed with Native when Native is the project (BLAKE3's own package, blake3-config.cmake, lands in the
# same prefix and LaplaceNativeConfig finds it); inside another build it is only what Native links.
if(PROJECT_IS_TOP_LEVEL)
  add_subdirectory(${LAPLACE_BLAKE3_DIR} blake3)
  # BLAKE3 installs its pkg-config file from the top build directory but writes it in its own (1.8.3).
  file(COPY_FILE ${CMAKE_CURRENT_BINARY_DIR}/blake3/libblake3.pc ${CMAKE_BINARY_DIR}/libblake3.pc ONLY_IF_DIFFERENT)
else()
  add_subdirectory(${LAPLACE_BLAKE3_DIR} blake3 EXCLUDE_FROM_ALL)
endif()

# The registries' law, generated from the manifests (Sequence 6.10): the manifest is the single input, and a change
# to it regenerates the header at the next configure.
set(LP_GENERATED ${CMAKE_CURRENT_BINARY_DIR}/generated)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/manifest/trust_classes.toml)
file(STRINGS ${CMAKE_CURRENT_SOURCE_DIR}/manifest/trust_classes.toml LP_TC_LINES REGEX "^[A-Za-z][A-Za-z0-9]* *= *[0-9.]+")
set(LP_TRUST_CLASS_ROWS "")
foreach(line IN LISTS LP_TC_LINES)
  string(REGEX MATCH "^([A-Za-z][A-Za-z0-9]*) *= *([0-9.]+)" _ "${line}")
  string(APPEND LP_TRUST_CLASS_ROWS "    { \"${CMAKE_MATCH_1}\", ${CMAKE_MATCH_2} },\n")
endforeach()
if(LP_TRUST_CLASS_ROWS STREQUAL "")
  message(FATAL_ERROR "manifest/trust_classes.toml declares no class")
endif()
configure_file(src/consensus/trust_class_law.h.in ${LP_GENERATED}/trust_class_law.h @ONLY)

set(LP_SOURCES src/core/platform.c src/core/mem.c src/core/map.c src/core/wire.c src/core/mapped.c src/geom/path.c
               src/core/cpu.c src/text/flags.c src/text/utf8.c src/core/identity.c src/core/sequence.c src/core/dag.c
               src/geom/coord.c src/core/compose.c src/geom/geometry.c src/geom/follows.c src/consensus/consensus.c
               src/codecs/tier0.c src/codecs/tier0_map.c src/geom/geom4d.c src/geom/s3.c src/geom/pull.c
               src/text/highway.c src/core/perfcache.c src/consensus/trust.c src/core/dot.c src/core/realize.c
               src/core/bench.c src/codecs/json.c src/text/decimal.c src/codecs/tensor.c src/core/envelope.c)
set(LP_ISA_V3_SOURCES src/isa/scan_avx2.c src/isa/row_avx2.c src/isa/s3_avx2.c src/isa/hilbert_bmi2.c src/isa/dot_avx2.c src/isa/tensor_avx2.c)
set(LP_ISA_V4_SOURCES src/isa/scan_avx512.c src/isa/row_avx512.c src/isa/dot_avx512.c)
set(LP_ISA_SOURCES ${LP_ISA_V3_SOURCES} ${LP_ISA_V4_SOURCES} src/isa/dot_avxvnni.c src/isa/dot_avx512vnni.c)
set_source_files_properties(${LP_ISA_V3_SOURCES} PROPERTIES COMPILE_OPTIONS "${LP_ISA_V3}")
set_source_files_properties(${LP_ISA_V4_SOURCES} PROPERTIES COMPILE_OPTIONS "${LP_ISA_V4}")
set_source_files_properties(src/isa/dot_avxvnni.c PROPERTIES COMPILE_OPTIONS "${LP_ISA_V3};${LP_ISA_AVXVNNI}")
set_source_files_properties(src/isa/dot_avx512vnni.c PROPERTIES COMPILE_OPTIONS "${LP_ISA_V4};${LP_ISA_VNNI512}")

# Both library forms use the same PIC sources, definitions and FP contract.
# Compile them once; consumers still select the existing static/shared targets.
add_library(laplace_objects OBJECT ${LP_SOURCES} ${LP_ISA_SOURCES})
target_include_directories(laplace_objects PRIVATE include src src/core ${LP_GENERATED})
target_compile_definitions(laplace_objects PRIVATE LP_HAVE_AVX2 LP_HAVE_AVX512 LP_HAVE_AVXVNNI LP_HAVE_AVX512VNNI LP_TIER0_DEFAULT="${LAPLACE_TIER0}")
target_compile_options(laplace_objects PRIVATE ${LP_C_FLAGS})
target_link_libraries(laplace_objects PRIVATE BLAKE3::blake3 ${LP_SYSLIBS})
if(TARGET laplace::math)
  target_link_libraries(laplace_objects PRIVATE laplace::math)
endif()

foreach(kind static shared)
  if(kind STREQUAL "static")
    add_library(laplace_static STATIC $<TARGET_OBJECTS:laplace_objects>)
    set(t laplace_static)
    if(NOT WIN32)                                     # on Windows the DLL's import library is laplace.lib
      set_target_properties(${t} PROPERTIES OUTPUT_NAME laplace)
    endif()
  else()
    add_library(laplace SHARED $<TARGET_OBJECTS:laplace_objects>)
    set(t laplace)
  endif()
  target_include_directories(${t} PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include> $<INSTALL_INTERFACE:include>)
  target_link_libraries(${t} PRIVATE BLAKE3::blake3 ${LP_SYSLIBS})
  if(TARGET laplace::math)                          # everything that links Native takes its libm calls from laplace-math
    target_link_libraries(${t} PUBLIC laplace::math)
  endif()
endforeach()
if(NOT WIN32)
  target_link_options(laplace PRIVATE -Wl,--exclude-libs,ALL)
endif()

# laplace_text: the one decomposition of text (UAX #29), which needs ICU for the Unicode version tier 0 was built from.
# ICU's symbols carry its version, so it loads beside the ICU a PostgreSQL server was built with.
if(EXISTS "${LAPLACE_ICU_DIR}/include/unicode/ubrk.h")
  add_library(laplace_text STATIC src/text/text.c src/text/ids.c)
  target_include_directories(laplace_text PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include> $<INSTALL_INTERFACE:include>
                                          PRIVATE ${LAPLACE_ICU_DIR}/include)
  target_compile_options(laplace_text PRIVATE ${LP_C_FLAGS})
  if(WIN32)                                         # ICU's Windows release: lib64, linked by full path so LIB cannot shadow it
    target_link_libraries(laplace_text PUBLIC laplace_static
      "${LAPLACE_ICU_DIR}/lib64/icuin.lib" "${LAPLACE_ICU_DIR}/lib64/icuuc.lib" "${LAPLACE_ICU_DIR}/lib64/icudt.lib")
  else()
    target_link_directories(laplace_text PUBLIC ${LAPLACE_ICU_DIR}/lib)
    target_link_libraries(laplace_text PUBLIC laplace_static icui18n icuuc icudata)
    # ICU's libraries find each other through the run path of whatever loads them: the old-style tag covers that.
    target_link_options(laplace_text INTERFACE -Wl,--disable-new-dtags)
  endif()
else()
  message(WARNING "ICU not found at ${LAPLACE_ICU_DIR}: laplace_text is not built, and neither is anything that decomposes text")
endif()

# laplace_model: the model-decomposition kernels, which need MKL and OpenMP; kept out of the core library so the
# PostgreSQL extension does not depend on them. MKL through its own CMake package (on CMAKE_PREFIX_PATH in the oneAPI
# dev shell on both platforms), as the single dynamic library mkl_rt; reproducible mode is pinned in rowsig.c.
find_package(OpenMP)
set(MKL_LINK sdl)
find_package(MKL CONFIG QUIET)
if(MKL_FOUND AND OpenMP_C_FOUND)
  add_library(laplace_model STATIC src/linalg/rowsig.c src/linalg/knn.c)
  target_include_directories(laplace_model PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include> $<INSTALL_INTERFACE:include>)
  target_compile_options(laplace_model PRIVATE ${LP_C_FLAGS})
  target_link_libraries(laplace_model PUBLIC MKL::MKL OpenMP::OpenMP_C ${LP_SYSLIBS})
else()
  message(STATUS "laplace_model not built: MKL ${MKL_FOUND}, OpenMP ${OpenMP_C_FOUND} (run from the oneAPI dev shell)")
endif()

# laplace_linalg: eigenmaps, Procrustes and Gram-Schmidt over Eigen and Spectra (headers only), kept out of the core
# library like laplace_model. Eigen's packet math fuses with FMA under x86-64-v3 and its product blocking follows the
# cache sizes cpuid reports, so it is compiled scalar with fixed blocking: the results are the scalar ones everywhere.
laplace_path(LAPLACE_EIGEN_DIR "" "Eigen 3.4 checkout")
laplace_path(LAPLACE_SPECTRA_DIR "" "Spectra 1.x checkout")
file(TO_CMAKE_PATH "${LAPLACE_EIGEN_DIR}" LP_EIGEN)
file(TO_CMAKE_PATH "${LAPLACE_SPECTRA_DIR}" LP_SPECTRA)
function(lp_linalg_library name)                    # extra arguments: compile options after the contract's
  add_library(${name} STATIC ${CMAKE_CURRENT_FUNCTION_LIST_DIR}/src/linalg/linalg.cpp)
  set_target_properties(${name} PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON CXX_VISIBILITY_PRESET hidden)
  target_include_directories(${name} PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_FUNCTION_LIST_DIR}/include> $<INSTALL_INTERFACE:include>)
  target_include_directories(${name} SYSTEM PRIVATE ${LP_EIGEN} ${LP_SPECTRA}/include)
  target_compile_definitions(${name} PRIVATE EIGEN_DONT_VECTORIZE EIGEN_NO_CPUID EIGEN_DONT_PARALLELIZE)
  target_compile_options(${name} PRIVATE ${LP_C_FLAGS} ${ARGN})
  target_link_libraries(${name} PUBLIC ${LP_SYSLIBS})
endfunction()
if(EXISTS "${LP_EIGEN}/Eigen/Core" AND EXISTS "${LP_SPECTRA}/include/Spectra/SymEigsSolver.h")
  lp_linalg_library(laplace_linalg)
else()
  message(STATUS "laplace_linalg not built: set LAPLACE_EIGEN_DIR and LAPLACE_SPECTRA_DIR")
endif()

# The package's names, the same in a build that includes Native and in one that finds it installed.
# Laplace::os: isolated OS/POSIX compatibility layer (Windows POSIX shims + musl regex live strictly on this
# target and NEVER pollute laplace_static / Laplace-postgres).
set(LP_OS_OBJECTS "")
if(WIN32)
  set(_lp_musl_default "")
  if(DEFINED ENV{LAPLACE_MUSL} AND EXISTS "$ENV{LAPLACE_MUSL}/src/regex/regcomp.c")
    set(_lp_musl_default "$ENV{LAPLACE_MUSL}")
  elseif(DEFINED ENV{LAPLACE_DEPSRC} AND EXISTS "$ENV{LAPLACE_DEPSRC}/musl/src/regex/regcomp.c")
    set(_lp_musl_default "$ENV{LAPLACE_DEPSRC}/musl")
  elseif(EXISTS "D:/Libraries/src/musl/src/regex/regcomp.c")
    set(_lp_musl_default "D:/Libraries/src/musl")
  endif()
  set(LAPLACE_MUSL "${_lp_musl_default}" CACHE PATH "musl sources (POSIX regex on Windows)")
  if(NOT EXISTS "${LAPLACE_MUSL}/src/regex/regcomp.c")
    message(FATAL_ERROR "LAPLACE_MUSL: no musl sources at '${LAPLACE_MUSL}'")
  endif()
  add_library(lp_posix_regex OBJECT
    ${LAPLACE_MUSL}/src/regex/regcomp.c
    ${LAPLACE_MUSL}/src/regex/regexec.c
    ${LAPLACE_MUSL}/src/regex/tre-mem.c
    ${LAPLACE_MUSL}/src/regex/regerror.c)
  target_include_directories(lp_posix_regex BEFORE PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include/laplace/win
    ${CMAKE_CURRENT_SOURCE_DIR}/include/laplace/win/musl)
  target_compile_options(lp_posix_regex PRIVATE ${LP_C_FLAGS} /w)
  target_compile_definitions(lp_posix_regex PRIVATE restrict=__restrict hidden= RE_DUP_MAX=255 CHARCLASS_NAME_MAX=14)
  list(APPEND LP_OS_OBJECTS $<TARGET_OBJECTS:lp_posix_regex>)
endif()

add_library(laplace_os STATIC src/os/os.c ${LP_OS_OBJECTS})
target_include_directories(laplace_os PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
  $<$<PLATFORM_ID:Windows>:$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include/laplace/win>>
  $<$<PLATFORM_ID:Windows>:$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}/laplace/win>>)
target_compile_options(laplace_os PRIVATE ${LP_C_FLAGS})
if(WIN32)
  target_compile_definitions(laplace_os PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()
target_link_libraries(laplace_os PUBLIC ${LP_SYSLIBS} $<$<NOT:$<PLATFORM_ID:Windows>>:${CMAKE_DL_LIBS}>)

add_library(Laplace::laplace ALIAS laplace)
add_library(Laplace::laplace_static ALIAS laplace_static)
set(LP_EXPORTED laplace laplace_static)
foreach(t text model linalg os)
  if(TARGET laplace_${t})
    add_library(Laplace::${t} ALIAS laplace_${t})
    set_target_properties(laplace_${t} PROPERTIES EXPORT_NAME ${t})
    list(APPEND LP_EXPORTED laplace_${t})
  endif()
endforeach()
if(TARGET laplace_math_link)
  add_library(Laplace::math ALIAS laplace_math_link)
  set_target_properties(laplace_math_link PROPERTIES EXPORT_NAME math)
  set_target_properties(laplace_math PROPERTIES EXPORT_NAME math_archive)
  list(APPEND LP_EXPORTED laplace_math_link laplace_math)
endif()

# What a consumer's reading contract records of Native: its sources, headers and manifests, line endings normalized,
# each framed by its relative path. The installed package carries it as LaplaceNative_SOURCE_ID in place of the tree.
set(LP_SOURCE_CONTRACT "")
file(GLOB_RECURSE LP_CONTRACT_INPUTS CONFIGURE_DEPENDS RELATIVE "${CMAKE_CURRENT_SOURCE_DIR}"
     "${CMAKE_CURRENT_SOURCE_DIR}/src/*.c" "${CMAKE_CURRENT_SOURCE_DIR}/src/*.h" "${CMAKE_CURRENT_SOURCE_DIR}/src/*.inc"
     "${CMAKE_CURRENT_SOURCE_DIR}/include/*.h" "${CMAKE_CURRENT_SOURCE_DIR}/manifest/*.tsv"
     "${CMAKE_CURRENT_SOURCE_DIR}/manifest/*.toml")
list(SORT LP_CONTRACT_INPUTS)
foreach(input IN LISTS LP_CONTRACT_INPUTS)
  file(READ "${CMAKE_CURRENT_SOURCE_DIR}/${input}" content)
  string(REPLACE "\r\n" "\n" content "${content}")
  string(SHA256 digest "${content}")
  string(LENGTH "${input}" length)
  string(APPEND LP_SOURCE_CONTRACT "${length}:${input}:${digest}\n")
endforeach()
string(SHA256 LaplaceNative_SOURCE_ID "${LP_SOURCE_CONTRACT}")
if(NOT PROJECT_IS_TOP_LEVEL)
  # A build that includes Native (the monorepo, until it consumes packages) finds it the same way a standalone one
  # does: find_package(LaplaceNative CONFIG REQUIRED) reads this build-tree config, whose targets are the ALIASes above.
  file(CONFIGURE OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/package/LaplaceNativeConfig.cmake CONTENT [=[
# LaplaceNative inside a build that includes Laplace-Native: the targets are that build's own (Laplace::* ALIASes).
set(LaplaceNative_SOURCE_ID "@LaplaceNative_SOURCE_ID@")
include("@CMAKE_CURRENT_SOURCE_DIR@/cmake/LaplacePaths.cmake")
include("@CMAKE_CURRENT_SOURCE_DIR@/cmake/LaplaceFlags.cmake")
include("@CMAKE_CURRENT_SOURCE_DIR@/cmake/LaplaceWindowsUtf8.cmake")
]=] @ONLY)
  # Not forced: a build given an installed package (-DLaplaceNative_DIR) keeps it.
  set(LaplaceNative_DIR ${CMAKE_CURRENT_BINARY_DIR}/package CACHE PATH "LaplaceNative package (this build's own Native)")
endif()

if(PROJECT_IS_TOP_LEVEL)
  add_executable(laplace_tier0 tools/tier0.c)
  set_target_properties(laplace_tier0 PROPERTIES OUTPUT_NAME laplace-tier0 EXPORT_NAME tier0)
  target_compile_options(laplace_tier0 PRIVATE ${LP_C_FLAGS})
  target_link_libraries(laplace_tier0 PRIVATE laplace_static)
  option(LAPLACE_NATIVE_TESTS "Build and register Native's tests (off for a package-only build)" ON)
  if(LAPLACE_NATIVE_TESTS)
    enable_testing()
    add_subdirectory(tests)
  endif()

  # The C# binding (tools/csharp): generated from the public headers' AST, never written by hand (target
  # csharp_bindings); checked against this library's own layout and calls (test csharp_abi); packed as the NuGet
  # package Laplace.Native.Interop with this platform's native library (target csharp_package).
  find_program(LP_NODE node)
  find_program(LP_DOTNET dotnet)
  if(LP_NODE AND CMAKE_C_COMPILER_ID MATCHES "IntelLLVM|Clang")
    set(LP_CSHARP ${CMAKE_CURRENT_BINARY_DIR}/csharp)
    file(GLOB LP_PUBLIC_HEADERS CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/include/laplace/*.h)
    add_custom_command(OUTPUT ${LP_CSHARP}/LaplaceNative.g.cs ${LP_CSHARP}/Abi.g.cs ${LP_CSHARP}/abi.g.c
      COMMAND ${CMAKE_COMMAND} -DCC=${CMAKE_C_COMPILER} -DNODE=${LP_NODE} -DSOURCE=${CMAKE_CURRENT_SOURCE_DIR}
              -DOUT=${LP_CSHARP} -P ${CMAKE_CURRENT_SOURCE_DIR}/tools/csharp/generate.cmake
      DEPENDS ${LP_PUBLIC_HEADERS} tools/csharp/api.c tools/csharp/generate.mjs tools/csharp/generate.cmake
      COMMENT "C# binding from the public headers" VERBATIM)
    add_custom_target(csharp_bindings ALL DEPENDS ${LP_CSHARP}/LaplaceNative.g.cs)
    add_executable(csharp_abi_c ${LP_CSHARP}/abi.g.c)
    target_link_libraries(csharp_abi_c PRIVATE laplace_static ${LP_SYSLIBS})
    if(LP_DOTNET AND LAPLACE_NATIVE_TESTS)
      add_test(NAME csharp_abi COMMAND ${CMAKE_COMMAND} -DABI=$<TARGET_FILE:csharp_abi_c> -DDOTNET=${LP_DOTNET}
        -DSOURCE=${CMAKE_CURRENT_SOURCE_DIR} -DGENERATED=${LP_CSHARP} -DLIBDIR=$<TARGET_FILE_DIR:laplace>
        -DWORK=${CMAKE_CURRENT_BINARY_DIR}/csharp-test -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/csharp.cmake)
    endif()
    if(LP_DOTNET)
      if(WIN32)
        set(LP_RID win-x64)
      else()
        set(LP_RID linux-x64)
      endif()
      # The package version: this project's, or (CI) 0.0.0-<UTC date>.<commit> until a release is cut.
      set(LAPLACE_PACKAGE_VERSION "${PROJECT_VERSION}" CACHE STRING "Laplace.Native.Interop package version")
      add_custom_target(csharp_package
        COMMAND ${LP_DOTNET} pack ${CMAKE_CURRENT_SOURCE_DIR}/bindings/csharp/Laplace.Native.Interop.csproj -c Release
                --artifacts-path ${CMAKE_CURRENT_BINARY_DIR}/csharp-package -o ${CMAKE_CURRENT_BINARY_DIR}/nuget
                -p:LaplaceNativeGenerated=${LP_CSHARP} -p:LaplaceNativeVersion=${LAPLACE_PACKAGE_VERSION}
                -p:LaplaceNativeRuntime=$<TARGET_FILE:laplace> -p:LaplaceNativeRid=${LP_RID}
        DEPENDS csharp_bindings laplace VERBATIM)
    endif()
  endif()

  # The package: libraries, headers, the tier-0 tool, and the CMake that builds against them under the same
  # floating-point contract (LaplaceFlags with the contract Native was built under, LaplacePaths, the UTF-8 manifest).
  include(GNUInstallDirs)
  include(CMakePackageConfigHelpers)
  set(LP_PACKAGE_DIR ${CMAKE_INSTALL_LIBDIR}/cmake/LaplaceNative)
  install(TARGETS ${LP_EXPORTED} laplace_tier0 EXPORT LaplaceNativeTargets
          LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR} ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
          RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
  install(DIRECTORY include/laplace DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
  install(EXPORT LaplaceNativeTargets NAMESPACE Laplace:: DESTINATION ${LP_PACKAGE_DIR})
  if(TARGET laplace_model)
    set(LP_HAVE_MODEL ON)
  else()
    set(LP_HAVE_MODEL OFF)
  endif()
  configure_package_config_file(cmake/LaplaceNativeConfig.cmake.in ${CMAKE_CURRENT_BINARY_DIR}/LaplaceNativeConfig.cmake
                                INSTALL_DESTINATION ${LP_PACKAGE_DIR})
  write_basic_package_version_file(${CMAKE_CURRENT_BINARY_DIR}/LaplaceNativeConfigVersion.cmake COMPATIBILITY ExactVersion)
  install(FILES ${CMAKE_CURRENT_BINARY_DIR}/LaplaceNativeConfig.cmake ${CMAKE_CURRENT_BINARY_DIR}/LaplaceNativeConfigVersion.cmake
                cmake/LaplacePaths.cmake cmake/LaplaceFlags.cmake cmake/LaplaceWindowsUtf8.cmake cmake/WindowsUtf8.manifest
          DESTINATION ${LP_PACKAGE_DIR})
  install(FILES ${LAPLACE_OPERATIONS}/toolchain/flags.cmake ${LAPLACE_OPERATIONS}/toolchain/determinism.tsv
          DESTINATION ${LP_PACKAGE_DIR}/toolchain)
endif()
