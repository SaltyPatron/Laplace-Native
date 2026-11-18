# The C# binding generated: the public headers' AST (the C compiler, clang-based), then tools/csharp/generate.mjs.
#   cmake -DCC=<clang or icx> -DNODE=<node> -DSOURCE=<Native source> -DOUT=<directory> -P generate.cmake
file(MAKE_DIRECTORY "${OUT}")
execute_process(COMMAND "${CC}" -Xclang -ast-dump=json -fsyntax-only -x c "-I${SOURCE}/include" "${SOURCE}/tools/csharp/api.c"
                OUTPUT_FILE "${OUT}/ast.json" ERROR_VARIABLE err RESULT_VARIABLE rc)
if(rc)
  message(FATAL_ERROR "the headers' AST: ${err}")
endif()
execute_process(COMMAND "${NODE}" "${SOURCE}/tools/csharp/generate.mjs" "${OUT}/ast.json" "${OUT}" RESULT_VARIABLE rc)
file(REMOVE "${OUT}/ast.json")
if(rc)
  message(FATAL_ERROR "tools/csharp/generate.mjs failed")
endif()
