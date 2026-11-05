function(wpgkit_test name source)
  add_executable(wpgkit_${name}_test ${source})
  target_link_libraries(wpgkit_${name}_test PRIVATE WPGKit::Analysis)
  add_test(NAME wpgkit.${name} COMMAND wpgkit_${name}_test)
  set_tests_properties(wpgkit.${name} PROPERTIES TIMEOUT 60
    ENVIRONMENT "TMPDIR=${_wpgkit_tmp};TMP=${_wpgkit_tmp};TEMP=${_wpgkit_tmp}")
endfunction()
wpgkit_test(profiling ProfilingTest.cpp)
wpgkit_test(graph_metadata GraphMetadataTest.cpp)
wpgkit_test(andersen AndersenTest.cpp)
wpgkit_test(owned_cfg OwnedCFGTest.cpp)
wpgkit_test(inter_cfg InterCFGTest.cpp)
wpgkit_test(data_flow DataFlowTest.cpp)
wpgkit_test(cfg_pointer_flow CFGPointerFlowTest.cpp)
wpgkit_test(call_graph_pta CallGraphPTATest.cpp)
wpgkit_test(incremental_cgpta IncrementalCGPTATest.cpp)
wpgkit_test(external_modules ExternalModulesTest.cpp)
wpgkit_test(empty_external_models EmptyExternalModelsTest.cpp)
wpgkit_test(function_object_model FunctionObjectModelTest.cpp)
wpgkit_test(external_pointer_summary ExternalPointerSummaryTest.cpp)
wpgkit_test(native_library_model NativeLibraryModelTest.cpp)
wpgkit_test(scalar_lifetime ScalarLifetimeTest.cpp)
wpgkit_test(pointer_completeness PointerCompletenessTest.cpp)
wpgkit_test(external_refinement ExternalModelRefinementTest.cpp)
wpgkit_test(runtime_summary RuntimeSummaryTest.cpp)
wpgkit_test(fprintf_model FprintfModelTest.cpp)

add_executable(wpgkit_files_test WPGFilesTest.cpp)
target_link_libraries(wpgkit_files_test PRIVATE WPGKit::Analysis)
add_test(NAME wpgkit.files COMMAND "${CMAKE_COMMAND}"
  "-DTESTER=$<TARGET_FILE:wpgkit_files_test>"
  "-DANALYZER=$<TARGET_FILE:wpgkit-analyze>"
  "-DWORK_DIR=${CMAKE_CURRENT_BINARY_DIR}/files"
  "-DCLANG=${LLVM_TOOLS_BINARY_DIR}/clang++"
  -P "${CMAKE_CURRENT_SOURCE_DIR}/WPGFilesTest.cmake")
add_test(NAME wpgkit.empty_external_models_inputs COMMAND "${CMAKE_COMMAND}"
  "-DTESTER=$<TARGET_FILE:wpgkit_empty_external_models_test>"
  "-DWORK_DIR=${CMAKE_CURRENT_BINARY_DIR}/empty-models"
  "-DCLANG=${LLVM_TOOLS_BINARY_DIR}/clang++"
  -P "${CMAKE_CURRENT_SOURCE_DIR}/EmptyExternalModelsTest.cmake")
add_test(NAME wpgkit.link_map COMMAND "${CMAKE_COMMAND}"
  "-DINVENTORY=$<TARGET_FILE:wpgkit-link-inventory>"
  "-DANALYZER=$<TARGET_FILE:wpgkit-analyze>"
  "-DWORK_DIR=${CMAKE_CURRENT_BINARY_DIR}/link-map"
  "-DCLANG=${LLVM_TOOLS_BINARY_DIR}/clang++"
  "-DAR=${LLVM_TOOLS_BINARY_DIR}/llvm-ar"
  -P "${CMAKE_CURRENT_SOURCE_DIR}/LinkMapTest.cmake")
add_test(NAME wpgkit.installed_clients COMMAND "${CMAKE_COMMAND}"
  "-DBUILD_DIR=${PROJECT_BINARY_DIR}"
  "-DCLIENT_DIR=${PROJECT_SOURCE_DIR}/examples/client"
  "-DWORK_DIR=${CMAKE_CURRENT_BINARY_DIR}/installed-clients"
  "-DCLANG=${LLVM_TOOLS_BINARY_DIR}/clang++"
  "-DCXX=${CMAKE_CXX_COMPILER}" "-DCC=${CMAKE_C_COMPILER}"
  "-DLLVM_DIR=${LLVM_DIR}"
  "-DCLANG_INCLUDE=${WPGKIT_CLANG_INCLUDE_DIR}"
  "-DCONFIG=$<CONFIG>"
  -P "${CMAKE_CURRENT_SOURCE_DIR}/InstalledClients.cmake")
set_tests_properties(wpgkit.files wpgkit.empty_external_models_inputs
  wpgkit.link_map wpgkit.installed_clients PROPERTIES TIMEOUT 120
  ENVIRONMENT "TMPDIR=${_wpgkit_tmp};TMP=${_wpgkit_tmp};TEMP=${_wpgkit_tmp}")
