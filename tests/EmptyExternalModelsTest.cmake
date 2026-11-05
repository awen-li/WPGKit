file(MAKE_DIRECTORY "${WORK_DIR}")
file(WRITE "${WORK_DIR}/entry.cpp"
  "int main(int argc, char **) { return __builtin_expect(argc, 0); }\n")
file(WRITE "${WORK_DIR}/compile_commands.json"
  "[{\"directory\":\"${WORK_DIR}\",\"file\":\"${WORK_DIR}/entry.cpp\","
  "\"arguments\":[\"${CLANG}\",\"-std=c++20\",\"-c\",\"${WORK_DIR}/entry.cpp\","
  "\"-o\",\"${WORK_DIR}/entry.o\"]}]")
execute_process(COMMAND "${CLANG}" -std=c++20 -c "${WORK_DIR}/entry.cpp"
  -o "${WORK_DIR}/entry.o" RESULT_VARIABLE status ERROR_VARIABLE diagnostic)
if(NOT status EQUAL 0)
  message(FATAL_ERROR "fixture compilation failed: ${diagnostic}")
endif()
execute_process(COMMAND "${CLANG}" "${WORK_DIR}/entry.o"
  "-Wl,-Map=${WORK_DIR}/linked.map" -o "${WORK_DIR}/linked"
  RESULT_VARIABLE status ERROR_VARIABLE diagnostic)
if(NOT status EQUAL 0)
  message(FATAL_ERROR "fixture linking failed: ${diagnostic}")
endif()

execute_process(COMMAND "${TESTER}" "${WORK_DIR}"
  RESULT_VARIABLE status OUTPUT_VARIABLE output ERROR_VARIABLE diagnostic)
if(NOT status EQUAL 0)
  message(FATAL_ERROR "empty-catalog input regression failed: ${output}\n${diagnostic}")
endif()
