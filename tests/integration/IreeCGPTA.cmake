if(NOT DEFINED IREE_ROOT OR NOT DEFINED WORK_DIR OR
   NOT DEFINED ANALYZER OR NOT DEFINED CLANG OR NOT DEFINED LLVM_INCLUDE)
  message(FATAL_ERROR "IREE_ROOT, WORK_DIR, ANALYZER, CLANG, and LLVM_INCLUDE are required")
endif()
execute_process(COMMAND git -C "${IREE_ROOT}" rev-parse HEAD
  RESULT_VARIABLE git_status OUTPUT_VARIABLE revision OUTPUT_STRIP_TRAILING_WHITESPACE)
if(NOT git_status EQUAL 0 OR
   NOT revision STREQUAL "1b60b62ed4b3f2a3c95f42b397889444c7df318c")
  message(FATAL_ERROR "IREE checkout must be candidate-20240724.964 at 1b60b62")
endif()

set(source_dir "${IREE_ROOT}/compiler/src")
set(string_source "${source_dir}/iree/compiler/Utils/StringUtils.cpp")
set(option_source "${source_dir}/iree/compiler/Utils/OptionUtils.cpp")
set(conversion_source "${source_dir}/iree/compiler/Utils/ConversionUtils.cpp")
foreach(source string_source option_source conversion_source)
  if(NOT EXISTS "${${source}}")
    message(FATAL_ERROR "missing IREE source: ${${source}}")
  endif()
endforeach()
file(MAKE_DIRECTORY "${WORK_DIR}")
file(WRITE "${WORK_DIR}/compile_commands.json"
  "[{\"directory\":\"${IREE_ROOT}\",\"file\":\"${string_source}\",\"arguments\":[\"${CLANG}\",\"-std=c++17\",\"-I${source_dir}\",\"-I${LLVM_INCLUDE}\",\"-c\",\"${string_source}\"]},\n"
  "{\"directory\":\"${IREE_ROOT}\",\"file\":\"${option_source}\",\"arguments\":[\"${CLANG}\",\"-std=c++17\",\"-I${source_dir}\",\"-I${LLVM_INCLUDE}\",\"-c\",\"${option_source}\"]},\n"
  "{\"directory\":\"${IREE_ROOT}\",\"file\":\"${conversion_source}\",\"arguments\":[\"${CLANG}\",\"-std=c++17\",\"-I${source_dir}\",\"-I${LLVM_INCLUDE}\",\"-c\",\"${conversion_source}\"]}]")
execute_process(COMMAND "${ANALYZER}" "${WORK_DIR}"
  mlir::iree_compiler::verifyAllOperationsAreLegal
  "${string_source}" "${option_source}" "${conversion_source}"
  RESULT_VARIABLE status OUTPUT_VARIABLE report ERROR_VARIABLE diagnostic)
if(NOT status EQUAL 0)
  message(FATAL_ERROR "IREE CG/PTA analysis failed: ${diagnostic}")
endif()
file(WRITE "${WORK_DIR}/iree-report.json" "${report}")
string(JSON input_count LENGTH "${report}" source_files)
string(JSON call_count LENGTH "${report}" calls)
string(JSON linked_program GET "${report}" linked_program_closed)
string(JSON within_inputs GET "${report}" closed_within_inputs)
string(FIND "${report}" "mlir::iree_compiler::emitLegalizationErrors" helper_position)
string(FIND "${report}" "\"unresolved\": true" unresolved_position)
if(NOT input_count EQUAL 3 OR call_count LESS 1 OR linked_program OR
   within_inputs OR helper_position EQUAL -1 OR unresolved_position EQUAL -1)
  message(FATAL_ERROR "IREE report lost direct edges or open-coverage evidence")
endif()
message(STATUS "IREE CG/PTA: ${call_count} call sites; linked closure remains unproven")
