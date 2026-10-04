# Golden tests: values the prototype established, which the real implementation must reproduce exactly.
foreach(name identity geometry coord follows consensus shape s3 pull flags highway)
  add_executable(test_${name} test_${name}.c)
  target_link_libraries(test_${name} PRIVATE laplace_static BLAKE3::blake3 ${LP_SYSLIBS})
  target_compile_options(test_${name} PRIVATE ${LP_C_FLAGS})
  target_compile_definitions(test_${name} PRIVATE LAPLACE_TIER0="${LAPLACE_TIER0}")
  add_test(NAME ${name} COMMAND test_${name})
endforeach()

# The matching kernels must agree with each other: run the matching test at every dispatch level this CPU has.
foreach(isa scalar sse2 avx2)
  add_test(NAME follows_${isa} COMMAND test_follows)
  set_tests_properties(follows_${isa} PROPERTIES ENVIRONMENT "LAPLACE_ISA=${isa}")
endforeach()
# The squared-distance rows under the shape measures, likewise: Hausdorff is checked bit for bit against its definition.
foreach(isa scalar avx2)
  add_test(NAME shape_${isa} COMMAND test_shape)
  set_tests_properties(shape_${isa} PROPERTIES ENVIRONMENT "LAPLACE_ISA=${isa}")
endforeach()
# The S^3 angle kernels, likewise: the batch is checked bit for bit against lp_angle4 one point at a time.
foreach(isa scalar avx2)
  add_test(NAME s3_${isa} COMMAND test_s3)
  set_tests_properties(s3_${isa} PROPERTIES ENVIRONMENT "LAPLACE_ISA=${isa}")
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
