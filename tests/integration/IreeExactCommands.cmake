execute_process(COMMAND git -C "${IREE_ROOT}" rev-parse HEAD
  RESULT_VARIABLE git_status OUTPUT_VARIABLE revision
  OUTPUT_STRIP_TRAILING_WHITESPACE)
if(NOT git_status EQUAL 0 OR
   NOT revision STREQUAL "1b60b62ed4b3f2a3c95f42b397889444c7df318c")
  message(FATAL_ERROR "IREE checkout must be candidate-20240724.964 at 1b60b62")
endif()
if(NOT EXISTS "${IREE_BUILD}/tools/iree-compile" OR
   NOT EXISTS "${IREE_BUILD}/compile_commands.json")
  message(FATAL_ERROR "the linked IREE build and compilation database are required")
endif()
set(source_dir "${IREE_ROOT}/compiler/src/iree/compiler/Utils")
file(MAKE_DIRECTORY "${WORK_DIR}")
execute_process(COMMAND "${ANALYZER}" "${IREE_BUILD}"
    mlir::iree_compiler::verifyAllOperationsAreLegal
    "${source_dir}/StringUtils.cpp"
    "${source_dir}/OptionUtils.cpp"
    "${source_dir}/ConversionUtils.cpp"
  RESULT_VARIABLE status OUTPUT_VARIABLE report ERROR_VARIABLE diagnostic)
if(NOT status EQUAL 0)
  message(FATAL_ERROR "exact-command IREE analysis failed: ${diagnostic}")
endif()
file(WRITE "${WORK_DIR}/iree-exact-report.json" "${report}")
string(JSON sources LENGTH "${report}" source_files)
string(JSON reachable LENGTH "${report}" reachable_functions)
string(JSON calls LENGTH "${report}" calls)
string(JSON closed GET "${report}" closed_within_inputs)
string(JSON linked GET "${report}" linked_program_closed)
if(NOT sources EQUAL 3 OR reachable LESS 100 OR calls LESS 100 OR
   closed OR linked)
  message(FATAL_ERROR "exact-command IREE report concealed the open boundary")
endif()
message(STATUS "IREE exact commands: ${reachable} reachable bodies, ${calls} calls")
