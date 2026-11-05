file(MAKE_DIRECTORY "${WORK_DIR}")
file(WRITE "${WORK_DIR}/main.cpp" "extern void used(); int main() { used(); }\n")
file(WRITE "${WORK_DIR}/main-both.cpp"
  "extern void used(); extern void unused_variant(); "
  "int main() { used(); unused_variant(); }\n")
file(WRITE "${WORK_DIR}/used.cpp"
  "#ifdef ALT\nvoid unused_variant() {}\n#else\nvoid used() {}\n#endif\n")
file(WRITE "${WORK_DIR}/unused.cpp" "void unused() {}\n")
file(WRITE "${WORK_DIR}/worker-main.cpp"
  "class Worker { public: Worker(); ~Worker(); void run(); }; "
  "int main() { Worker worker; worker.run(); }\n")
file(WRITE "${WORK_DIR}/worker.cpp"
  "class Worker { public: Worker(); ~Worker(); void run(); }; "
  "Worker::Worker() {} Worker::~Worker() {} void Worker::run() {}\n")
file(WRITE "${WORK_DIR}/virtual-main.cpp"
  "struct Base { virtual void run(); }; "
  "struct Derived : Base { void run() override; }; "
  "int main() { Derived object; Base *base = &object; base->run(); }\n")
file(WRITE "${WORK_DIR}/virtual.cpp"
  "struct Base { virtual void run(); }; "
  "struct Derived : Base { void run() override; }; "
  "void Base::run() {} void Derived::run() {}\n")
file(WRITE "${WORK_DIR}/callback-main.cpp"
  "void relay(void (*)()); void target() {} int main() { relay(&target); }\n")
file(WRITE "${WORK_DIR}/callback.cpp"
  "void relay(void (*callback)()) { callback(); }\n")
set(fixture_names main main-both used unused worker-main worker
  virtual-main virtual callback-main callback)
foreach(name IN LISTS fixture_names)
  execute_process(
    COMMAND "${CLANG}" -c "${WORK_DIR}/${name}.cpp" -o "${WORK_DIR}/${name}.o"
    RESULT_VARIABLE result ERROR_VARIABLE diagnostic)
  if(NOT result EQUAL 0)
    message(FATAL_ERROR "compile ${name} failed: ${diagnostic}")
  endif()
endforeach()
execute_process(
  COMMAND "${CLANG}" -DALT=1 -c "${WORK_DIR}/used.cpp"
    -o "${WORK_DIR}/used-alt.o"
  RESULT_VARIABLE result ERROR_VARIABLE diagnostic)
if(NOT result EQUAL 0)
  message(FATAL_ERROR "compile alternate variant failed: ${diagnostic}")
endif()
execute_process(
  COMMAND "${AR}" rcs "${WORK_DIR}/libfixture.a"
    "${WORK_DIR}/used.o" "${WORK_DIR}/unused.o" "${WORK_DIR}/used-alt.o"
  RESULT_VARIABLE result ERROR_VARIABLE diagnostic)
if(NOT result EQUAL 0)
  message(FATAL_ERROR "archive failed: ${diagnostic}")
endif()
execute_process(
  COMMAND "${CLANG}" "${WORK_DIR}/main.o" "${WORK_DIR}/libfixture.a"
    "-Wl,-Map=${WORK_DIR}/linked.map" -o "${WORK_DIR}/linked"
  RESULT_VARIABLE result ERROR_VARIABLE diagnostic)
if(NOT result EQUAL 0)
  message(FATAL_ERROR "link failed: ${diagnostic}")
endif()
execute_process(
  COMMAND "${CLANG}" "${WORK_DIR}/main-both.o" "${WORK_DIR}/libfixture.a"
    "-Wl,-Map=${WORK_DIR}/linked-both.map" -o "${WORK_DIR}/linked-both"
  RESULT_VARIABLE result ERROR_VARIABLE diagnostic)
if(NOT result EQUAL 0)
  message(FATAL_ERROR "variant link failed: ${diagnostic}")
endif()
execute_process(
  COMMAND "${CLANG}" "${WORK_DIR}/main.o" "${WORK_DIR}/used.o"
    "${WORK_DIR}/unused.o" "-Wl,-Map=${WORK_DIR}/linked-direct.map"
    -o "${WORK_DIR}/linked-direct"
  RESULT_VARIABLE result ERROR_VARIABLE diagnostic)
if(NOT result EQUAL 0)
  message(FATAL_ERROR "direct-object link failed: ${diagnostic}")
endif()
execute_process(
  COMMAND "${CLANG}" "${WORK_DIR}/worker-main.o" "${WORK_DIR}/worker.o"
    "-Wl,-Map=${WORK_DIR}/worker.map" -o "${WORK_DIR}/worker-linked"
  RESULT_VARIABLE result ERROR_VARIABLE diagnostic)
if(NOT result EQUAL 0)
  message(FATAL_ERROR "C++ linked fixture failed: ${diagnostic}")
endif()
foreach(name virtual callback)
  execute_process(
    COMMAND "${CLANG}" "${WORK_DIR}/${name}-main.o" "${WORK_DIR}/${name}.o"
      "-Wl,-Map=${WORK_DIR}/${name}.map" -o "${WORK_DIR}/${name}-linked"
    RESULT_VARIABLE result ERROR_VARIABLE diagnostic)
  if(NOT result EQUAL 0)
    message(FATAL_ERROR "${name} linked fixture failed: ${diagnostic}")
  endif()
endforeach()

set(database "[\n")
foreach(name IN LISTS fixture_names)
  if(NOT name STREQUAL "main")
    string(APPEND database ",\n")
  endif()
  string(APPEND database
    "{\"directory\":\"${WORK_DIR}\",\"file\":\"${WORK_DIR}/${name}.cpp\","
    "\"arguments\":[\"${CLANG}\",\"-c\",\"${WORK_DIR}/${name}.cpp\","
    "\"-o\",\"${WORK_DIR}/${name}.o\"]}")
endforeach()
string(APPEND database
  ",\n{\"directory\":\"${WORK_DIR}\",\"file\":\"${WORK_DIR}/used.cpp\","
  "\"arguments\":[\"${CLANG}\",\"-DALT=1\",\"-c\",\"${WORK_DIR}/used.cpp\","
  "\"-o\",\"${WORK_DIR}/used-alt.o\"]}")
string(APPEND database "\n]\n")
file(WRITE "${WORK_DIR}/compile_commands.json" "${database}")

execute_process(
  COMMAND "${INVENTORY}" "${WORK_DIR}" "${WORK_DIR}/linked.map"
  OUTPUT_VARIABLE report ERROR_VARIABLE diagnostic RESULT_VARIABLE result)
if(NOT result EQUAL 0)
  message(FATAL_ERROR "inventory failed: ${diagnostic}")
endif()
string(FIND "${report}" "${WORK_DIR}/used.cpp" used_found)
string(FIND "${report}" "${WORK_DIR}/main.cpp" main_found)
string(FIND "${report}" "${WORK_DIR}/unused.cpp" unused_found)
string(FIND "${report}" "\"all_objects_mapped\": false" open_found)
if(used_found EQUAL -1 OR main_found EQUAL -1 OR
   NOT unused_found EQUAL -1 OR open_found EQUAL -1)
  message(FATAL_ERROR "inventory did not distinguish selected archive member: ${report}")
endif()

# A stale setting from an earlier release must not stop linked analysis.
execute_process(
  COMMAND "${CMAKE_COMMAND}" -E env WPGKIT_WPG_MAX_RSS_MIB=1
    "${ANALYZER}" --profile --link-map "${WORK_DIR}"
    "${WORK_DIR}/linked.map" main
  OUTPUT_VARIABLE graph ERROR_VARIABLE diagnostic RESULT_VARIABLE result)
if(NOT result EQUAL 0)
  message(FATAL_ERROR "linked call graph failed: ${diagnostic}")
endif()
file(WRITE "${WORK_DIR}/profile.json" "${graph}\n")
string(JSON components LENGTH "${graph}" profiling components)
if(NOT components EQUAL 7)
  message(FATAL_ERROR "linked analysis must profile CG/inter-CFG/PTA and DFG without symbol lookup")
endif()
foreach(index RANGE 0 6)
  string(JSON available GET "${graph}" profiling components ${index} rss_available)
  string(JSON elapsed GET "${graph}" profiling components ${index} elapsed_ns)
  if(NOT available OR elapsed LESS 1)
    message(FATAL_ERROR "linked analysis phase lacks RSS or timing measurements")
  endif()
endforeach()

function(check_single_parse report expected)
  string(JSON count LENGTH "${report}" profiling components)
  math(EXPR last "${count} - 1")
  set(parses 0)
  set(releases 0)
  foreach(index RANGE 0 ${last})
    string(JSON name GET "${report}" profiling components ${index} name)
    if(name STREQUAL "ast_parse")
      string(JSON parses GET "${report}" profiling components ${index} invocations)
    elseif(name STREQUAL "ast_release")
      string(JSON releases GET "${report}" profiling components ${index} invocations)
    endif()
  endforeach()
  if(NOT parses EQUAL expected OR NOT releases EQUAL expected)
    message(FATAL_ERROR "expected ${expected} parses/releases, got ${parses}/${releases}")
  endif()
endfunction()
check_single_parse("${graph}" 2)
string(JSON peak GET "${graph}" peak_rss_mib)
string(JSON current GET "${graph}" current_rss_mib)
if(peak LESS 1 OR current LESS 1)
  message(FATAL_ERROR "linked analysis must finish available bodies and report RSS")
endif()
string(FIND "${graph}" "${WORK_DIR}/used.cpp" linked_used)
string(FIND "${graph}" "${WORK_DIR}/unused.cpp" linked_unused)
string(FIND "${graph}" "\"name\": \"used\"" call_used)
string(FIND "${graph}" "\"name\": \"unused_variant\"" variant_unused)
string(FIND "${graph}" "\"linked_program_closed\": false" still_open)
string(JSON analyzed GET "${graph}" analyzed_linked_objects)
string(JSON caller_type TYPE "${graph}" calls 0 caller)
string(JSON first_target_type TYPE "${graph}" calls 0 targets 0)
string(JSON metadata_count LENGTH "${graph}" function_metadata)
if(linked_used EQUAL -1 OR NOT linked_unused EQUAL -1 OR
   call_used EQUAL -1 OR NOT variant_unused EQUAL -1 OR still_open EQUAL -1 OR
   NOT analyzed EQUAL 2 OR NOT caller_type STREQUAL "NUMBER" OR
   NOT first_target_type STREQUAL "NUMBER" OR metadata_count LESS 2)
  message(FATAL_ERROR "linked graph selected wrong sources or claimed closure: ${graph}")
endif()

execute_process(
  COMMAND "${ANALYZER}" --link-map "${WORK_DIR}"
    "${WORK_DIR}/linked-both.map" main
  OUTPUT_VARIABLE both_graph ERROR_VARIABLE diagnostic RESULT_VARIABLE result)
if(NOT result EQUAL 0)
  message(FATAL_ERROR "multi-variant linked graph failed: ${diagnostic}")
endif()
string(FIND "${both_graph}" "\"name\": \"used\"" both_used)
string(FIND "${both_graph}" "\"name\": \"unused_variant\"" both_variant)
string(JSON source_count LENGTH "${both_graph}" source_files)
string(JSON analyzed_both GET "${both_graph}" analyzed_linked_objects)
if(both_used EQUAL -1 OR both_variant EQUAL -1 OR NOT source_count EQUAL 2 OR
   NOT analyzed_both EQUAL 3)
  message(FATAL_ERROR "both linked variants were not analyzed: ${both_graph}")
endif()

execute_process(
  COMMAND "${ANALYZER}" --link-map "${WORK_DIR}"
    "${WORK_DIR}/worker.map" main
  OUTPUT_VARIABLE worker_graph ERROR_VARIABLE diagnostic RESULT_VARIABLE result)
if(NOT result EQUAL 0)
  message(FATAL_ERROR "linked C++ call graph failed: ${diagnostic}")
endif()
string(JSON worker_count GET "${worker_graph}" analyzed_linked_objects)
foreach(name "Worker::Worker" "Worker::~Worker" "Worker::run")
  string(FIND "${worker_graph}" "\"name\": \"${name}\"" found)
  if(found EQUAL -1)
    message(FATAL_ERROR "linked C++ target ${name} was not resolved")
  endif()
endforeach()
if(NOT worker_count EQUAL 2)
  message(FATAL_ERROR "linked C++ fixture parsed the wrong objects")
endif()

foreach(name virtual callback)
  execute_process(
    COMMAND "${ANALYZER}" --profile --link-map "${WORK_DIR}" "${WORK_DIR}/${name}.map" main
    OUTPUT_VARIABLE graph ERROR_VARIABLE diagnostic RESULT_VARIABLE result)
  if(NOT result EQUAL 0)
    message(FATAL_ERROR "${name} on-the-fly analysis failed: ${diagnostic}")
  endif()
  string(JSON analyzed GET "${graph}" analyzed_linked_objects)
  if(NOT analyzed EQUAL 2)
    message(FATAL_ERROR "${name} left an available body unprocessed")
  endif()
  check_single_parse("${graph}" 2)
  if(name STREQUAL "virtual")
    foreach(target "Base::run" "Derived::run")
      string(FIND "${graph}" "\"name\": \"${target}\"" found)
      if(found EQUAL -1)
        message(FATAL_ERROR "known override ${target} was not loaded")
      endif()
    endforeach()
  else()
    string(JSON closed GET "${graph}" closed_within_inputs)
    string(FIND "${graph}" "\"name\": \"target\"" found)
    if(NOT closed OR found EQUAL -1)
      message(FATAL_ERROR "cross-unit callback did not converge from captured facts")
    endif()
  endif()
endforeach()

# Parsing needs source commands and the saved manifest, not object-file symbols.
file(RENAME "${WORK_DIR}/unused.o" "${WORK_DIR}/unused.o.saved")
execute_process(
  COMMAND "${ANALYZER}" --profile --link-map "${WORK_DIR}"
    "${WORK_DIR}/linked-direct.map" main
  OUTPUT_VARIABLE direct_graph ERROR_VARIABLE diagnostic RESULT_VARIABLE result)
file(RENAME "${WORK_DIR}/unused.o.saved" "${WORK_DIR}/unused.o")
if(NOT result EQUAL 0)
  message(FATAL_ERROR "direct-object graph failed: ${diagnostic}")
endif()
string(JSON direct_mapped GET "${direct_graph}" mapped_linked_objects)
string(JSON direct_analyzed GET "${direct_graph}" analyzed_linked_objects)
string(FIND "${direct_graph}"
  "linked objects omitted from reachable-body pass" omitted)
string(FIND "${direct_graph}" "${WORK_DIR}/unused.cpp" unused_source)
string(FIND "${direct_graph}" "\"name\": \"unused\"" unused_reachable)
if(NOT direct_mapped EQUAL 3 OR NOT direct_analyzed EQUAL 3 OR
   NOT omitted EQUAL -1 OR unused_source EQUAL -1 OR NOT unused_reachable EQUAL -1)
  message(FATAL_ERROR "all linked units must be parsed while unreachable bodies stay inactive")
endif()
check_single_parse("${direct_graph}" 3)
