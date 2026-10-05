# Compiler flags for Laplace-Native.
#
# Determinism first. With icx the floating-point flags are the contract every Laplace build shares, read from
# Laplace-Operations (toolchain/determinism.tsv): precise FP, no contraction (an FMA rounds once where a multiply and
# an add round twice), no fast math, no build-time folding of libm calls, no Intel math libraries, and the ISA level
# both machines have (x86-64-v3); laplace-math (CORE-MATH under the libm names) answers every libm call.
# SIMD everywhere: kernels are compiled per ISA (v3: AVX2/FMA/BMI2, v4: AVX-512 F/BW/VL/DQ) and chosen at run time.
# VNNI is its own flag, given only to the VNNI kernels: a v3 or v4 file the compiler might otherwise vectorize with
# vpdpwssd would fault on hart-server's Broadwell-E (v3, no AVX-VNNI) or a Skylake-SP (v4, no VNNI).
# LP_ISA_BASE (x86-64: SSE2, no FMA) is the reference a scalar result is checked against.
if(CMAKE_C_COMPILER_ID MATCHES "^(IntelLLVM|GNU|Clang)$")
  if(NOT EXISTS "${LAPLACE_OPERATIONS}/toolchain/flags.cmake")
    message(FATAL_ERROR "LAPLACE_OPERATIONS: no floating-point contract at '${LAPLACE_OPERATIONS}/toolchain'")
  endif()
  include("${LAPLACE_OPERATIONS}/toolchain/flags.cmake")
  laplace_fp_flags(LP_FP_COMPILE LP_FP_LINK)
endif()

# On Windows icx takes MSVC-style options and silently ignores GNU spellings: there each clang option is passed
# through /clang: (the contract is already spelled for the driver).
if(CMAKE_C_COMPILER_ID STREQUAL "IntelLLVM" AND CMAKE_C_SIMULATE_ID STREQUAL "MSVC")
  set(LP_C_FLAGS /clang:-Wall /clang:-Wextra /clang:-Wno-unused-parameter ${LP_FP_COMPILE}
      /D_CRT_SECURE_NO_WARNINGS)                      # the C runtime's advice to use its *_s functions instead of C17's
  set(LP_ISA_BASE /clang:-march=x86-64)
  set(LP_ISA_V3 /clang:-march=x86-64-v3)
  set(LP_ISA_V4 /clang:-march=x86-64-v4)
  set(LP_ISA_AVXVNNI /clang:-mavxvnni)
  set(LP_ISA_VNNI512 /clang:-mavx512vnni)
  set(LP_OPT /clang:-O3)
elseif(CMAKE_C_COMPILER_ID STREQUAL "IntelLLVM")
  set(LP_C_FLAGS -Wall -Wextra -Wno-unused-parameter ${LP_FP_COMPILE})
  set(LP_ISA_BASE -march=x86-64)
  set(LP_ISA_V3 -march=x86-64-v3)
  set(LP_ISA_V4 -march=x86-64-v4)
  set(LP_ISA_AVXVNNI -mavxvnni)
  set(LP_ISA_VNNI512 -mavx512vnni)
elseif(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
  set(LP_C_FLAGS -Wall -Wextra -Wno-unused-parameter ${LP_FP_COMPILE})
  set(LP_ISA_BASE -march=x86-64)
  set(LP_ISA_V3 -march=x86-64-v3)
  set(LP_ISA_V4 -march=x86-64-v4)
  set(LP_ISA_AVXVNNI -mavxvnni)
  set(LP_ISA_VNNI512 -mavx512vnni)
else()
  message(FATAL_ERROR "Laplace-Native builds with icx, gcc, or clang (got ${CMAKE_C_COMPILER_ID})")
endif()

if(NOT DEFINED LP_OPT)
  set(LP_OPT -O3)
endif()
if(CMAKE_BUILD_TYPE STREQUAL "Release" OR CMAKE_BUILD_TYPE STREQUAL "RelWithDebInfo")
  list(APPEND LP_C_FLAGS ${LP_OPT})
endif()

# The C runtime's maths and threads are separate libraries on POSIX and part of the runtime on Windows.
if(WIN32)
  set(LP_SYSLIBS "")
else()
  set(LP_SYSLIBS m pthread)
endif()
