# Golden tests: values the prototype established, which the real implementation must reproduce exactly.
foreach(name identity geometry coord follows consensus shape pull flags highway core)
  add_executable(test_${name} test_${name}.c)
  target_link_libraries(test_${name} PRIVATE laplace_static BLAKE3::blake3 m)
  target_compile_options(test_${name} PRIVATE ${LP_C_FLAGS})
  target_compile_definitions(test_${name} PRIVATE LAPLACE_TIER0="${LAPLACE_TIER0}")
  add_test(NAME ${name} COMMAND test_${name})
endforeach()

# The kernels must agree with each other: every test of a dispatched kernel (the vertex scan, the distance rows, the
# Hilbert interleave) runs again at every dispatch level.
foreach(isa scalar sse2 avx2 avx512)
  foreach(name follows core coord shape)
    add_test(NAME ${name}_${isa} COMMAND test_${name})
    set_tests_properties(${name}_${isa} PROPERTIES ENVIRONMENT "LAPLACE_ISA=${isa}")
  endforeach()
endforeach()

if(TARGET laplace_text)
  add_executable(test_text test_text.c)
  target_link_libraries(test_text PRIVATE laplace_text laplace_static BLAKE3::blake3 m)
  target_compile_options(test_text PRIVATE ${LP_C_FLAGS})
  target_compile_definitions(test_text PRIVATE LAPLACE_TIER0="${LAPLACE_TIER0}")
  set_target_properties(test_text PROPERTIES BUILD_RPATH "${LAPLACE_ICU_DIR}/lib")
  add_test(NAME text COMMAND test_text)
endif()
