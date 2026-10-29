# Applies PATCH to the working directory unless it is applied already, so a
# FetchContent PATCH_COMMAND can run again on every configure.
#   cmake -DPATCH=<file> -P apply-patch.cmake
execute_process(COMMAND git apply --reverse --check --ignore-whitespace ${PATCH}
                RESULT_VARIABLE applied OUTPUT_QUIET ERROR_QUIET)
if(NOT applied EQUAL 0)
  execute_process(COMMAND git apply --ignore-whitespace ${PATCH} RESULT_VARIABLE result)
  if(NOT result EQUAL 0)
    message(FATAL_ERROR "cannot apply ${PATCH}")
  endif()
endif()
