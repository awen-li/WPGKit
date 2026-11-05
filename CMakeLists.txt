cmake_minimum_required(VERSION 3.24)
project(WPGKit VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(_wpgkit_tmp "${CMAKE_CURRENT_SOURCE_DIR}/.work-tmp/tmp")
file(MAKE_DIRECTORY "${_wpgkit_tmp}")
foreach(variable TMPDIR TMP TEMP)
  set(ENV{${variable}} "${_wpgkit_tmp}")
endforeach()
foreach(launcher CMAKE_CXX_COMPILER_LAUNCHER CMAKE_CXX_LINKER_LAUNCHER)
  set(_launcher "${CMAKE_COMMAND}" -E env
    "TMPDIR=${_wpgkit_tmp}" "TMP=${_wpgkit_tmp}" "TEMP=${_wpgkit_tmp}" --)
  if(${launcher})
    list(APPEND _launcher ${${launcher}})
  endif()
  set(${launcher} "${_launcher}")
endforeach()

include(cmake/WPGKitDependencies.cmake)
if(NOT TARGET WPGKit::Dependencies)
  message(FATAL_ERROR "WPGKit requires LLVM/Clang 18.1.3 development packages")
endif()
include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

# PIC objects are compiled once and reused by both deliverables.
add_library(wpgkit_objects OBJECT
  src/Profiling.cpp
  src/wpg/Andersen.cpp
  src/wpg/CallGraphPTA.cpp
  src/wpg/DataFlowBuilder.cpp
  src/wpg/DataFlowGraph.cpp
  src/wpg/InterCFG.cpp
  src/wpg/LinkMapInventory.cpp
  src/wpg/LocalDataFlow.cpp
  src/wpg/ProgramInputs.cpp
  src/clang/ClangCFGLifter.cpp
  src/clang/ClangDataFlow.cpp
  src/external/ExternalEffects.cpp
  src/external/ExternalModules.cpp)
set_target_properties(wpgkit_objects PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_compile_features(wpgkit_objects PRIVATE cxx_std_20)
target_include_directories(wpgkit_objects PRIVATE include)
target_link_libraries(wpgkit_objects PRIVATE WPGKit::Dependencies)

add_library(wpgkit_static STATIC $<TARGET_OBJECTS:wpgkit_objects>)
add_library(wpgkit_shared SHARED $<TARGET_OBJECTS:wpgkit_objects>)
foreach(target wpgkit_static wpgkit_shared)
  set_target_properties(${target} PROPERTIES OUTPUT_NAME wpgkit)
  target_compile_features(${target} PUBLIC cxx_std_20)
  target_include_directories(${target} PUBLIC
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
    "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>")
  target_link_libraries(${target} PUBLIC WPGKit::Dependencies)
endforeach()
set_target_properties(wpgkit_static PROPERTIES EXPORT_NAME Static)
set_target_properties(wpgkit_shared PROPERTIES EXPORT_NAME Shared
  VERSION ${PROJECT_VERSION} SOVERSION ${PROJECT_VERSION_MAJOR}
  INSTALL_RPATH_USE_LINK_PATH TRUE)
add_library(WPGKit::Static ALIAS wpgkit_static)
add_library(WPGKit::Shared ALIAS wpgkit_shared)
add_library(wpgkit_analysis INTERFACE)
target_link_libraries(wpgkit_analysis INTERFACE WPGKit::Shared)
set_target_properties(wpgkit_analysis PROPERTIES EXPORT_NAME Analysis)
add_library(WPGKit::Analysis ALIAS wpgkit_analysis)

file(RELATIVE_PATH _wpgkit_bin_to_lib
  "${CMAKE_INSTALL_FULL_BINDIR}" "${CMAKE_INSTALL_FULL_LIBDIR}")
foreach(tool analyze link-inventory)
  add_executable(wpgkit-${tool} tools/wpgkit-${tool}.cpp)
  target_link_libraries(wpgkit-${tool} PRIVATE WPGKit::Analysis)
  set_target_properties(wpgkit-${tool} PROPERTIES
    INSTALL_RPATH "$ORIGIN/${_wpgkit_bin_to_lib}")
endforeach()

install(TARGETS wpgkit_static wpgkit_shared wpgkit_analysis
  EXPORT WPGKitTargets
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(TARGETS wpgkit-analyze wpgkit-link-inventory
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(EXPORT WPGKitTargets NAMESPACE WPGKit::
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/WPGKit)
configure_package_config_file(cmake/WPGKitConfig.cmake.in
  "${CMAKE_CURRENT_BINARY_DIR}/WPGKitConfig.cmake"
  INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/WPGKit)
write_basic_package_version_file("${CMAKE_CURRENT_BINARY_DIR}/WPGKitConfigVersion.cmake"
  VERSION ${PROJECT_VERSION} COMPATIBILITY SameMinorVersion)
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/WPGKitConfig.cmake"
  "${CMAKE_CURRENT_BINARY_DIR}/WPGKitConfigVersion.cmake"
  cmake/WPGKitDependencies.cmake
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/WPGKit)
install(FILES LICENSE NOTICE DESTINATION ${CMAKE_INSTALL_DATADIR}/licenses/WPGKit)

include(CTest)
if(BUILD_TESTING)
  add_subdirectory(tests)
endif()
