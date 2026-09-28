# Compiler flags for Laplace-Native.
#
# Determinism first: no fast-math, no floating-point contraction (an FMA rounds once where a multiply and an add
# round twice, which changes bits), no excess precision. icx defaults to -fp-model=fast, so it is set explicitly.
# SIMD everywhere: the baseline is x86-64-v2, and kernels are compiled per ISA (v3: AVX2/FMA/BMI2, v4: AVX-512
# F/BW/VL/DQ, plus VNNI) and chosen at run time.

set(LP_C_FLAGS -Wall -Wextra -Wno-unused-parameter)

if(CMAKE_C_COMPILER_ID STREQUAL "IntelLLVM")
  list(APPEND LP_C_FLAGS -fp-model=precise -ffp-contract=off -fno-fast-math -march=x86-64-v2 -Wno-overriding-option)   # the overrides are intended
  set(LP_ISA_V3 -march=x86-64-v3 -mavxvnni)
  set(LP_ISA_V4 -march=x86-64-v4 -mavx512vnni)
elseif(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
  list(APPEND LP_C_FLAGS -ffp-contract=off -fno-fast-math -march=x86-64-v2)
  if(CMAKE_C_COMPILER_ID STREQUAL "GNU")
    list(APPEND LP_C_FLAGS -fexcess-precision=standard)
  endif()
  set(LP_ISA_V3 -march=x86-64-v3)
  set(LP_ISA_V4 -march=x86-64-v4)
else()
  message(FATAL_ERROR "Laplace-Native builds with icx, gcc, or clang (got ${CMAKE_C_COMPILER_ID})")
endif()

if(CMAKE_BUILD_TYPE STREQUAL "Release" OR CMAKE_BUILD_TYPE STREQUAL "RelWithDebInfo")
  list(APPEND LP_C_FLAGS -O3)
endif()
