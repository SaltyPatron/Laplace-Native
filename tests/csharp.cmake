# The C# binding against the library: C's layout (csharp_abi_c) read by tests/csharp/AbiCheck, which compares it with
# C#'s and calls through the library.
#   -DABI=<csharp_abi_c> -DDOTNET=<dotnet> -DSOURCE=<Native source> -DGENERATED=<dir> -DLIBDIR=<laplace's dir> -DWORK=<dir>
file(MAKE_DIRECTORY "${WORK}")
execute_process(COMMAND "${ABI}" OUTPUT_FILE "${WORK}/c-layout.json" RESULT_VARIABLE rc)
if(rc)
  message(FATAL_ERROR "csharp_abi_c failed: ${rc}")
endif()
if(WIN32)
  set(ENV{PATH} "${LIBDIR};$ENV{PATH}")
else()
  set(ENV{LD_LIBRARY_PATH} "${LIBDIR}:$ENV{LD_LIBRARY_PATH}")
endif()
execute_process(COMMAND "${DOTNET}" run --project "${SOURCE}/tests/csharp/AbiCheck" -c Release
                        "--artifacts-path" "${WORK}/artifacts" "-p:LaplaceNativeGenerated=${GENERATED}"
                        -- "${WORK}/c-layout.json"
                RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE out)
message("${out}")
if(rc)
  message(FATAL_ERROR "the C# binding disagrees with the library")
endif()
