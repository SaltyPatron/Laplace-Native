include_guard(GLOBAL)

# Native command-line and filesystem input use the same UTF-8 contract as the
# content kernels. The process manifest configures the Windows CRT before main;
# changing console encoding after argv conversion cannot recover lost bytes.
# https://learn.microsoft.com/windows/apps/design/globalizing/use-utf8-code-page
function(laplace_utf8_executable target)
  if(WIN32)
    get_target_property(kind ${target} TYPE)
    if(NOT kind STREQUAL "EXECUTABLE")
      message(FATAL_ERROR "laplace_utf8_executable requires an executable target")
    endif()
    target_sources(${target} PRIVATE "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/WindowsUtf8.manifest")
  endif()
endfunction()
