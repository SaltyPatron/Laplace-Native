cmake_minimum_required(VERSION 3.24)
project(LaplaceNative VERSION 0.2.0 LANGUAGES C CXX ASM)

# Laplace-Native: the library with the shared code. Laplace-Engine and Laplace-postgres build it as part of themselves
# (add_subdirectory) and keep no copy of anything in it. Every ISA is compiled in and chosen at run time.
set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)             # linked into PostgreSQL extensions
set(CMAKE_C_VISIBILITY_PRESET hidden)

include(cmake/LaplacePaths.cmake)
include(cmake/LaplaceFlags.cmake)

# BLAKE3, built with this compiler and every SIMD variant (its own runtime dispatch).
set(BUILD_SHARED_LIBS OFF)
if(CMAKE_C_COMPILER_ID STREQUAL "IntelLLVM")            # BLAKE3's CMake knows gcc/clang/MSVC, not icx: same Unix assembly
  set(BLAKE3_AMD64_ASM_SOURCES blake3_avx2_x86-64_unix.S blake3_avx512_x86-64_unix.S blake3_sse2_x86-64_unix.S blake3_sse41_x86-64_unix.S)
endif()
add_subdirectory(${LAPLACE_BLAKE3_DIR} blake3 EXCLUDE_FROM_ALL)

set(LP_SOURCES src/cpu.c src/flags.c src/utf8.c src/identity.c src/coord.c src/compose.c src/geometry.c src/follows.c src/consensus.c
               src/tier0.c src/geom4d.c src/pull.c)
set(LP_ISA_SOURCES src/isa/scan_avx2.c src/isa/scan_avx512.c src/isa/row_avx2.c src/isa/hilbert_bmi2.c)
set_source_files_properties(src/isa/scan_avx2.c src/isa/row_avx2.c src/isa/hilbert_bmi2.c PROPERTIES COMPILE_OPTIONS "${LP_ISA_V3}")
set_source_files_properties(src/isa/scan_avx512.c PROPERTIES COMPILE_OPTIONS "${LP_ISA_V4}")

foreach(kind static shared)
  if(kind STREQUAL "static")
    add_library(laplace_static STATIC ${LP_SOURCES} ${LP_ISA_SOURCES})
    set(t laplace_static)
    set_target_properties(${t} PROPERTIES OUTPUT_NAME laplace)
  else()
    add_library(laplace SHARED ${LP_SOURCES} ${LP_ISA_SOURCES})
    set(t laplace)
  endif()
  target_include_directories(${t} PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include> $<INSTALL_INTERFACE:include>
                                      PRIVATE src)
  target_compile_definitions(${t} PRIVATE LP_HAVE_AVX2 LP_HAVE_AVX512 LP_TIER0_DEFAULT="${LAPLACE_TIER0}")
  target_compile_options(${t} PRIVATE ${LP_C_FLAGS})
  target_link_libraries(${t} PRIVATE BLAKE3::blake3 m pthread)
endforeach()
target_link_options(laplace PRIVATE -Wl,--exclude-libs,ALL)

# laplace_text: the one decomposition of text (UAX #29), which needs ICU for the Unicode version tier 0 was built from.
# ICU's symbols carry its version, so it loads beside the ICU a PostgreSQL server was built with.
if(EXISTS "${LAPLACE_ICU_DIR}/include/unicode/ubrk.h")
  add_library(laplace_text STATIC src/text.c)
  target_include_directories(laplace_text PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include PRIVATE ${LAPLACE_ICU_DIR}/include)
  target_compile_options(laplace_text PRIVATE ${LP_C_FLAGS})
  target_link_directories(laplace_text PUBLIC ${LAPLACE_ICU_DIR}/lib)
  target_link_libraries(laplace_text PUBLIC laplace_static icui18n icuuc icudata)
  # ICU's libraries find each other through the run path of whatever loads them: the old-style tag covers that.
  target_link_options(laplace_text INTERFACE -Wl,--disable-new-dtags)
else()
  message(WARNING "ICU not found at ${LAPLACE_ICU_DIR}: laplace_text is not built, and neither is anything that decomposes text")
endif()

# Correctly rounded sin and cos (CORE-MATH): one right answer on every CPU and compiler, which vendor libms
# (Intel's libimf dispatches by CPU) do not give. Tier 0 is generated with them.
if(EXISTS "${LAPLACE_COREMATH}/src/binary64/sin/sin.c")
  add_library(lp_crmath STATIC ${LAPLACE_COREMATH}/src/binary64/sin/sin.c ${LAPLACE_COREMATH}/src/binary64/cos/cos.c)
  target_compile_options(lp_crmath PRIVATE -w ${LP_C_FLAGS})
endif()

# laplace_model: the model-decomposition kernels, which need MKL and OpenMP; kept out of the core library so the
# PostgreSQL extension does not depend on them.
find_package(OpenMP)
if(DEFINED ENV{MKLROOT} AND OpenMP_C_FOUND)
  add_library(laplace_model STATIC src/rowsig.c)
  target_include_directories(laplace_model PUBLIC $ENV{MKLROOT}/include ${CMAKE_CURRENT_SOURCE_DIR}/include)
  target_compile_options(laplace_model PRIVATE ${LP_C_FLAGS})
  target_link_libraries(laplace_model PUBLIC $ENV{MKLROOT}/lib/libmkl_rt.so OpenMP::OpenMP_C m)
endif()

if(PROJECT_IS_TOP_LEVEL)
  enable_testing()
  add_subdirectory(tests)
  include(GNUInstallDirs)
  install(TARGETS laplace laplace_static LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR} ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
  install(DIRECTORY include/laplace DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
endif()
