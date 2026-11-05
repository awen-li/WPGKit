file(MAKE_DIRECTORY "${WORK_DIR}/include")
file(WRITE "${WORK_DIR}/include/api.h" [=[
#pragma once
void relay(void (*callback)());
inline void target() {}
]=])
file(WRITE "${WORK_DIR}/entry.cpp" [=[
#include "api.h"
#if WPGKIT_WPG_TEST != 1
#error compilation database flags were lost
#endif
void entry() { relay(&target); }
]=])
file(WRITE "${WORK_DIR}/helper.cpp" [=[
#include "api.h"
void relay(void (*callback)()) { callback(); }
]=])
file(WRITE "${WORK_DIR}/compile_commands.json"
  "[{\"directory\":\"${WORK_DIR}\",\"file\":\"${WORK_DIR}/entry.cpp\",\"arguments\":[\"${CLANG}\",\"-std=c++20\",\"-DWPGKIT_WPG_TEST=1\",\"-Iinclude\",\"-c\",\"entry.cpp\"]},\n"
  "{\"directory\":\"${WORK_DIR}\",\"file\":\"${WORK_DIR}/helper.cpp\",\"arguments\":[\"${CLANG}\",\"-std=c++20\",\"-Iinclude\",\"-c\",\"helper.cpp\"]}]")
execute_process(COMMAND "${TESTER}" "${WORK_DIR}" "${WORK_DIR}/entry.cpp"
  "${WORK_DIR}/helper.cpp" valid
  RESULT_VARIABLE status OUTPUT_VARIABLE output ERROR_VARIABLE diagnostic)
if(NOT status EQUAL 0)
  message(FATAL_ERROR "WPG file loading failed: ${output}\n${diagnostic}")
endif()
execute_process(COMMAND "${ANALYZER}" "${WORK_DIR}" entry
  "${WORK_DIR}/entry.cpp" "${WORK_DIR}/helper.cpp"
  RESULT_VARIABLE status OUTPUT_VARIABLE report ERROR_VARIABLE diagnostic)
if(NOT status EQUAL 0)
  message(FATAL_ERROR "WPG analyzer failed: ${diagnostic}")
endif()
string(JSON within_inputs GET "${report}" closed_within_inputs)
string(JSON linked_program GET "${report}" linked_program_closed)
string(JSON call_count LENGTH "${report}" calls)
if(NOT within_inputs OR linked_program OR NOT call_count EQUAL 2)
  message(FATAL_ERROR "WPG report lost call or coverage information: ${report}")
endif()
execute_process(COMMAND "${ANALYZER}" --profile "${WORK_DIR}" entry
  "${WORK_DIR}/entry.cpp" "${WORK_DIR}/helper.cpp"
  RESULT_VARIABLE status OUTPUT_VARIABLE profiled ERROR_VARIABLE diagnostic)
if(NOT status EQUAL 0)
  message(FATAL_ERROR "profiled WPG analyzer failed: ${diagnostic}")
endif()
string(JSON unchanged REMOVE "${profiled}" profiling)
string(JSON baseline GET "${report}")
# Resource measurements vary between processes; compare graph/work fields.
foreach(field current_rss_mib peak_rss_mib)
  string(JSON unchanged REMOVE "${unchanged}" ${field})
  string(JSON baseline REMOVE "${baseline}" ${field})
endforeach()
if(NOT unchanged STREQUAL baseline)
  message(FATAL_ERROR "profiling changed the graph result")
endif()
string(JSON components LENGTH "${profiled}" profiling components)
if(NOT components EQUAL 7)
  message(FATAL_ERROR "file analysis must profile CG/inter-CFG/PTA and DFG construction")
endif()
foreach(index RANGE 0 6)
  string(JSON elapsed GET "${profiled}" profiling components ${index} elapsed_ns)
  string(JSON invocations GET "${profiled}" profiling components ${index} invocations)
  if(elapsed LESS 1 OR invocations LESS 1)
    message(FATAL_ERROR "profiled phase was not measured")
  endif()
endforeach()
string(JSON dfg_nodes GET "${report}" data_flow node_count)
string(JSON dfg_edges GET "${report}" data_flow edge_count)
if(dfg_nodes LESS 1 OR dfg_edges LESS 1)
  message(FATAL_ERROR "file analysis must return an owned DFG")
endif()
string(JSON inter_calls GET "${report}" inter_cfg call_connections)
string(JSON inter_returns GET "${report}" inter_cfg return_connections)
string(JSON inter_gaps GET "${report}" inter_cfg gap_count)
if(NOT inter_calls EQUAL 2 OR NOT inter_returns EQUAL 2 OR NOT inter_gaps EQUAL 0)
  message(FATAL_ERROR "file runner lost inter-CFG call/return connections")
endif()
file(WRITE "${WORK_DIR}/helper.cpp" "invalid syntax;\n")
execute_process(COMMAND "${TESTER}" "${WORK_DIR}" "${WORK_DIR}/entry.cpp"
  "${WORK_DIR}/helper.cpp" invalid
  RESULT_VARIABLE status OUTPUT_VARIABLE output ERROR_VARIABLE diagnostic)
if(NOT status EQUAL 0)
  message(FATAL_ERROR "WPG accepted invalid source: ${output}\n${diagnostic}")
endif()
execute_process(COMMAND "${ANALYZER}" --profile "${WORK_DIR}" entry
  "${WORK_DIR}/entry.cpp" "${WORK_DIR}/helper.cpp"
  RESULT_VARIABLE status OUTPUT_VARIABLE profiled ERROR_VARIABLE diagnostic)
string(JSON failed GET "${profiled}" analysis_failed)
string(JSON phase GET "${profiled}" profiling components 0 name)
string(JSON count GET "${profiled}" profiling components 0 invocations)
if(NOT status EQUAL 1 OR NOT failed OR NOT phase STREQUAL "analysis" OR
   NOT count EQUAL 1)
  message(FATAL_ERROR "failed analysis did not close and report its profile")
endif()
