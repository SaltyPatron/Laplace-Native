# Compiler flags for Laplace-Native.
#
# Determinism first: no fast-math, no floating-point contraction (an FMA rounds once where a multiply and an add
# round twice, which changes bits), no excess precision. icx defaults to -fp-model=fast, so it is set explicitly.
# SIMD everywhere: the baseline is x86-64-v2, and kernels are compiled per ISA (v3: AVX2/FMA/BMI2, v4: AVX-512
# F/BW/VL/DQ, plus VNNI) and chosen at run time.

# On Windows icx takes MSVC-style options and silently ignores the GNU spellings below (-fp-model=, -ffp-contract=),
# which would leave contraction on: there each clang option is passed through /clang:, and the floating-point model
# is /fp:precise. The determinism is the same flags, spelled for the driver.
if(CMAKE_C_COMPILER_ID STREQUAL "IntelLLVM" AND CMAKE_C_SIMULATE_ID STREQUAL "MSVC")
  set(LP_C_FLAGS /clang:-Wall /clang:-Wextra /clang:-Wno-unused-parameter
      /fp:precise /clang:-ffp-contract=off /clang:-fno-fast-math /clang:-march=x86-64-v2 -Wno-overriding-option
      /D_CRT_SECURE_NO_WARNINGS)                      # the C runtime's advice to use its *_s functions instead of C17's
  set(LP_ISA_V3 /clang:-march=x86-64-v3 /clang:-mavxvnni)
  set(LP_ISA_V4 /clang:-march=x86-64-v4 /clang:-mavx512vnni)
  set(LP_OPT /clang:-O3)
elseif(CMAKE_C_COMPILER_ID STREQUAL "IntelLLVM")
  set(LP_C_FLAGS -Wall -Wextra -Wno-unused-parameter)
  list(APPEND LP_C_FLAGS -fp-model=precise -ffp-contract=off -fno-fast-math -march=x86-64-v2 -Wno-overriding-option)   # the overrides are intended
  set(LP_ISA_V3 -march=x86-64-v3 -mavxvnni)
  set(LP_ISA_V4 -march=x86-64-v4 -mavx512vnni)
elseif(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
  set(LP_C_FLAGS -Wall -Wextra -Wno-unused-parameter)
  list(APPEND LP_C_FLAGS -ffp-contract=off -fno-fast-math -march=x86-64-v2)
  if(CMAKE_C_COMPILER_ID STREQUAL "GNU")
    list(APPEND LP_C_FLAGS -fexcess-precision=standard)
  endif()
  set(LP_ISA_V3 -march=x86-64-v3)
  set(LP_ISA_V4 -march=x86-64-v4)
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
