cmake_minimum_required(VERSION 3.24)
project(LaplaceNative VERSION 0.1.0 LANGUAGES C CXX ASM)

# Laplace-Native: the shared native library. Every ISA is compiled in and chosen at run time; nothing is disabled.
set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)             # linked into PostgreSQL extensions
set(CMAKE_C_VISIBILITY_PRESET hidden)

set(LAPLACE_BLAKE3_DIR "/repos/src/blake3/c" CACHE PATH "BLAKE3 C sources")
set(LAPLACE_TIER0 "/repos/src/Laplace-Prototype/tier0/tier0.bin" CACHE FILEPATH "tier-0 table for the golden tests")

include(cmake/LaplaceFlags.cmake)

# BLAKE3, built with this compiler and every SIMD variant (its own runtime dispatch).
set(BUILD_SHARED_LIBS OFF)
if(CMAKE_C_COMPILER_ID STREQUAL "IntelLLVM")            # BLAKE3's CMake knows gcc/clang/MSVC, not icx: same Unix assembly
  set(BLAKE3_AMD64_ASM_SOURCES blake3_avx2_x86-64_unix.S blake3_avx512_x86-64_unix.S blake3_sse2_x86-64_unix.S blake3_sse41_x86-64_unix.S)
endif()
add_subdirectory(${LAPLACE_BLAKE3_DIR} blake3 EXCLUDE_FROM_ALL)

set(LP_SOURCES src/cpu.c src/identity.c src/coord.c src/geometry.c src/follows.c src/consensus.c src/tier0.c src/geom4d.c)
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
  target_compile_definitions(${t} PRIVATE LP_HAVE_AVX2 LP_HAVE_AVX512)
  target_compile_options(${t} PRIVATE ${LP_C_FLAGS})
  target_link_libraries(${t} PRIVATE BLAKE3::blake3 m)
endforeach()
target_link_options(laplace PRIVATE -Wl,--exclude-libs,ALL)

enable_testing()
add_subdirectory(tests)

include(GNUInstallDirs)
install(TARGETS laplace laplace_static LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR} ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
install(DIRECTORY include/laplace DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

# Observability: laplace-bench measures every operation, each SIMD kernel next to its scalar form.
add_executable(laplace-bench tools/laplace_bench.c)
target_include_directories(laplace-bench PRIVATE src)
target_link_libraries(laplace-bench PRIVATE laplace_static BLAKE3::blake3 m)
target_compile_options(laplace-bench PRIVATE ${LP_C_FLAGS})
install(TARGETS laplace-bench RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

# Correctly rounded sin and cos (CORE-MATH): one right answer on every CPU and compiler, which vendor libms
# (Intel's libimf dispatches by CPU) do not give.
set(LAPLACE_COREMATH "/repos/src/core-math" CACHE PATH "CORE-MATH sources")
add_library(lp_crmath STATIC ${LAPLACE_COREMATH}/src/binary64/sin/sin.c ${LAPLACE_COREMATH}/src/binary64/cos/cos.c)
target_compile_options(lp_crmath PRIVATE -w ${LP_C_FLAGS})

# laplace-tier0: tier 0 generated natively from the Unicode data.
add_executable(laplace-tier0 tools/laplace_tier0.c)
target_link_libraries(laplace-tier0 PRIVATE laplace_static lp_crmath BLAKE3::blake3 m)
target_compile_options(laplace-tier0 PRIVATE ${LP_C_FLAGS})
install(TARGETS laplace-tier0 RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

# laplace-ingest: text into the DAG, streamed into PostgreSQL by binary COPY. Needs ICU (Unicode 17) and libpq.
set(LAPLACE_ICU_DIR "/repos/src/Laplace-Prototype/lib/icu78" CACHE PATH "ICU 78 install (Unicode 17)")
set(LAPLACE_PG_DIR "/usr/local/pgsql" CACHE PATH "PostgreSQL install with libpq")
if(EXISTS "${LAPLACE_ICU_DIR}/include/unicode/ubrk.h" AND EXISTS "${LAPLACE_PG_DIR}/include/libpq-fe.h")
  set(LAPLACE_TREESITTER "/repos/src/tree-sitter" CACHE PATH "tree-sitter runtime")
  set(LAPLACE_TS_JSON "/vault/Data/TreeSitter/tree-sitter-json" CACHE PATH "tree-sitter JSON grammar")
  add_library(ts_json STATIC ${LAPLACE_TREESITTER}/lib/src/lib.c ${LAPLACE_TS_JSON}/src/parser.c)
  target_include_directories(ts_json PUBLIC ${LAPLACE_TREESITTER}/lib/include PRIVATE ${LAPLACE_TREESITTER}/lib/src ${LAPLACE_TREESITTER}/lib/src/wasm ${LAPLACE_TS_JSON}/src)
  target_compile_options(ts_json PRIVATE -w)
  set(LAPLACE_TS_XML "/vault/Data/TreeSitter/tree-sitter-xml" CACHE PATH "tree-sitter XML grammar")
  add_library(ts_xml STATIC ${LAPLACE_TS_XML}/xml/src/parser.c ${LAPLACE_TS_XML}/xml/src/scanner.c)
  target_include_directories(ts_xml PRIVATE ${LAPLACE_TREESITTER}/lib/include ${LAPLACE_TS_XML}/xml/src)
  target_compile_options(ts_xml PRIVATE -w)
  add_executable(laplace-ingest tools/laplace_ingest.c)
  target_include_directories(laplace-ingest PRIVATE ${LAPLACE_ICU_DIR}/include ${LAPLACE_PG_DIR}/include)
  target_link_directories(laplace-ingest PRIVATE ${LAPLACE_ICU_DIR}/lib ${LAPLACE_PG_DIR}/lib)
  target_link_libraries(laplace-ingest PRIVATE laplace_static BLAKE3::blake3 ts_json ts_xml icui18n icuuc icudata pq m)
  target_compile_options(laplace-ingest PRIVATE ${LP_C_FLAGS})
  set_target_properties(laplace-ingest PROPERTIES BUILD_RPATH "${LAPLACE_ICU_DIR}/lib;${LAPLACE_PG_DIR}/lib"
                                                  INSTALL_RPATH "${LAPLACE_ICU_DIR}/lib;${LAPLACE_PG_DIR}/lib")
  install(TARGETS laplace-ingest RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()

# laplace_model: the model-decomposition kernels, which need MKL and OpenMP; kept out of the core library so the
# PostgreSQL extension does not depend on them.
find_package(OpenMP)
if(DEFINED ENV{MKLROOT} AND OpenMP_C_FOUND)
  add_library(laplace_model STATIC src/rowsig.c)
  target_include_directories(laplace_model PUBLIC $ENV{MKLROOT}/include ${CMAKE_CURRENT_SOURCE_DIR}/include)
  target_compile_options(laplace_model PRIVATE ${LP_C_FLAGS})
  target_link_libraries(laplace_model PUBLIC $ENV{MKLROOT}/lib/libmkl_rt.so OpenMP::OpenMP_C m)
  add_executable(laplace-model tools/laplace_model.c)
  target_compile_options(laplace-model PRIVATE ${LP_C_FLAGS} -mf16c)
  target_link_libraries(laplace-model PRIVATE laplace_model laplace_static BLAKE3::blake3)
  set_target_properties(laplace-model PROPERTIES BUILD_RPATH "$ENV{MKLROOT}/lib")
endif()
