cmake_minimum_required(VERSION 3.24)
project(WPGKitClient LANGUAGES C CXX)
find_package(WPGKit 0.1 CONFIG REQUIRED)
# The same client validates both exported library variants.
foreach(kind Static Shared Analysis)
  string(TOLOWER "${kind}" suffix)
  add_executable(client-${suffix} main.cpp)
  target_link_libraries(client-${suffix} PRIVATE WPGKit::${kind})
endforeach()
