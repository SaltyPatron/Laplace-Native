# Golden tests: values the prototype established, which the real implementation must reproduce exactly.
foreach(name identity geometry coord follows consensus shape s3 pull flags highway core kernels)
  add_executable(test_${name} test_${name}.c)
  target_link_libraries(test_${name} PRIVATE laplace_static BLAKE3::blake3 ${LP_SYSLIBS})
  target_compile_options(test_${name} PRIVATE ${LP_C_FLAGS})
  target_compile_definitions(test_${name} PRIVATE LAPLACE_TIER0="${LAPLACE_TIER0}")
  add_test(NAME ${name} COMMAND test_${name})
endforeach()

# The kernels must agree with each other at every dispatch level: every test of a dispatched kernel (the vertex scan,
# the distance rows under the shape measures with Hausdorff checked bit for bit against its definition, the S^3 angle
# batch checked against lp_angle4 one point at a time, the Hilbert interleave, the int8 dot) runs again at every level.
# A level this CPU lacks is skipped (exit 77) by the tests that check for it. The AVX-512 levels run under Intel SDE as
# a Sapphire Rapids when LAPLACE_SDE names its sde executable (cache variable or environment), and natively, or
# skipped, otherwise.
set(LAPLACE_SDE "" CACHE FILEPATH "Intel SDE's sde executable, to run the AVX-512 dispatch levels")
set(lp_sde "${LAPLACE_SDE}")
if(NOT lp_sde AND DEFINED ENV{LAPLACE_SDE})
  set(lp_sde "$ENV{LAPLACE_SDE}")
endif()
if(lp_sde AND NOT EXISTS "${lp_sde}")
  message(FATAL_ERROR "LAPLACE_SDE: no file at '${lp_sde}'")
endif()
foreach(name follows core coord shape s3 kernels)
  foreach(isa scalar sse2 avx2 avxvnni avx512 avx512vnni)
    if(isa MATCHES "^avx512" AND lp_sde)
      add_test(NAME ${name}_${isa} COMMAND ${lp_sde} -spr -- $<TARGET_FILE:test_${name}>)
    else()
      add_test(NAME ${name}_${isa} COMMAND test_${name})
    endif()
    set_tests_properties(${name}_${isa} PROPERTIES ENVIRONMENT "LAPLACE_ISA=${isa}" SKIP_RETURN_CODE 77)
  endforeach()
endforeach()

if(TARGET laplace_text)
  add_executable(test_text test_text.c)
  target_link_libraries(test_text PRIVATE laplace_text laplace_static BLAKE3::blake3 ${LP_SYSLIBS})
  target_compile_options(test_text PRIVATE ${LP_C_FLAGS})
  target_compile_definitions(test_text PRIVATE LAPLACE_TIER0="${LAPLACE_TIER0}")
  set_target_properties(test_text PROPERTIES BUILD_RPATH "${LAPLACE_ICU_DIR}/lib")
  add_test(NAME text COMMAND test_text)
  if(WIN32)                                           # ICU's DLLs, found on the path at run time
    set_tests_properties(text PROPERTIES ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${LAPLACE_ICU_DIR}/bin64")
  endif()
endif()

# rowsig needs MKL: its hash is golden across machines, which MKL gives only in reproducible mode.
if(TARGET laplace_model)
  add_executable(test_rowsig test_rowsig.c)
  target_link_libraries(test_rowsig PRIVATE laplace_model laplace_static BLAKE3::blake3 ${LP_SYSLIBS})
  target_compile_options(test_rowsig PRIVATE ${LP_C_FLAGS})
  add_test(NAME rowsig COMMAND test_rowsig)
endif()
