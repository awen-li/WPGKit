function(run)
  execute_process(COMMAND ${ARGV} RESULT_VARIABLE status
    OUTPUT_VARIABLE output ERROR_VARIABLE diagnostic)
  if(NOT status EQUAL 0)
    message(FATAL_ERROR "${ARGV}\n${output}\n${diagnostic}")
  endif()
  set(last_output "${output}" PARENT_SCOPE)
endfunction()
file(MAKE_DIRECTORY "${WORK_DIR}")
# Remove old installed headers so they cannot mask packaging errors.
file(REMOVE_RECURSE "${WORK_DIR}/stage" "${WORK_DIR}/relocated")
run("${CMAKE_COMMAND}" --install "${BUILD_DIR}" --config "${CONFIG}"
  --prefix "${WORK_DIR}/stage")
# Consume a relocated installation, never the source/build tree's headers.
file(MAKE_DIRECTORY "${WORK_DIR}/relocated")
file(COPY "${WORK_DIR}/stage/" DESTINATION "${WORK_DIR}/relocated")
file(REMOVE_RECURSE "${WORK_DIR}/stage")
run("${CMAKE_COMMAND}" -S "${CLIENT_DIR}" -B "${WORK_DIR}/client-build"
  "-DCMAKE_PREFIX_PATH=${WORK_DIR}/relocated" "-DLLVM_DIR=${LLVM_DIR}"
  "-DWPGKIT_CLANG_INCLUDE_DIR=${CLANG_INCLUDE}"
  "-DCMAKE_CXX_COMPILER=${CXX}" "-DCMAKE_C_COMPILER=${CC}"
  "-DCMAKE_BUILD_TYPE=${CONFIG}")
run("${CMAKE_COMMAND}" --build "${WORK_DIR}/client-build" --config "${CONFIG}" -j 2)
file(WRITE "${WORK_DIR}/fixture.cpp"
  "void target() {} void alternate() {} void relay(void (*f)()) { f(); }\n"
  "struct Callbacks { void (*left)(); void (*right)(); };\n"
  "void entry() { Callbacks callbacks{target, alternate}; relay(callbacks.left); }\n")
file(WRITE "${WORK_DIR}/compile_commands.json"
  "[{\"directory\":\"${WORK_DIR}\",\"file\":\"${WORK_DIR}/fixture.cpp\","
  "\"arguments\":[\"${CLANG}\",\"-std=c++20\",\"-fPIC\",\"-c\",\"${WORK_DIR}/fixture.cpp\","
  "\"-o\",\"${WORK_DIR}/fixture.o\"]}]")
run("${CLANG}" -std=c++20 -fPIC -c "${WORK_DIR}/fixture.cpp"
  -o "${WORK_DIR}/fixture.o")
run("${CLANG}" -shared "${WORK_DIR}/fixture.o"
  "-Wl,-Map=${WORK_DIR}/linked.map" -o "${WORK_DIR}/libfixture.so")
foreach(kind static shared analysis)
  run("${WORK_DIR}/client-build/client-${kind}" "${WORK_DIR}" "${WORK_DIR}/fixture.cpp")
  if(kind STREQUAL "static")
    set(expected "${last_output}")
  elseif(NOT last_output STREQUAL expected)
    message(FATAL_ERROR "Installed library variants produced different results")
  endif()
endforeach()
run("${WORK_DIR}/relocated/bin/wpgkit-analyze" "${WORK_DIR}" entry "${WORK_DIR}/fixture.cpp")
