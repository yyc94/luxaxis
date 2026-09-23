cmake_minimum_required(VERSION 3.25)

project(luxaxis-hyprland VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(LUXAXIS_BUILD_PLUGIN "Build the Hyprland plugin" ON)
option(LUXAXIS_BUILD_TESTS "Build core unit tests" ON)

include(FetchContent)
find_package(tomlplusplus 3.4 CONFIG QUIET)
if(NOT tomlplusplus_FOUND)
  FetchContent_Declare(
    tomlplusplus
    URL https://github.com/marzer/tomlplusplus/archive/refs/tags/v3.4.0.tar.gz
    URL_HASH SHA256=8517f65938a4faae9ccf8ebb36631a38c1cadfb5efa85d9a72e15b9e97d25155
  )
  FetchContent_MakeAvailable(tomlplusplus)
endif()

find_package(Threads REQUIRED)

add_library(luxaxis_core STATIC src/core/config.cpp src/core/engine.cpp src/core/image_cache.cpp src/core/spotlight.cpp)
target_include_directories(luxaxis_core PUBLIC include)
target_link_libraries(luxaxis_core PRIVATE tomlplusplus::tomlplusplus Threads::Threads)
target_compile_options(luxaxis_core PRIVATE -Wall -Wextra -Wpedantic)
set_target_properties(luxaxis_core PROPERTIES POSITION_INDEPENDENT_CODE ON)

if(LUXAXIS_BUILD_PLUGIN)
  find_package(PkgConfig REQUIRED)
  pkg_check_modules(HYPRLAND REQUIRED IMPORTED_TARGET hyprland=0.56.2)

  add_library(luxaxis MODULE src/plugin.cpp)
  target_link_libraries(luxaxis PRIVATE PkgConfig::HYPRLAND)
  target_compile_options(luxaxis PRIVATE -Wall -Wextra -Wpedantic)
  set_target_properties(
    luxaxis
    PROPERTIES
      PREFIX ""
      CXX_VISIBILITY_PRESET hidden
      VISIBILITY_INLINES_HIDDEN YES
  )
endif()

include(CTest)

if(BUILD_TESTING AND LUXAXIS_BUILD_TESTS)
  foreach(test_name IN ITEMS config engine image_cache spotlight)
    add_executable(luxaxis_${test_name}_tests tests/${test_name}_test.cpp)
    target_link_libraries(luxaxis_${test_name}_tests PRIVATE luxaxis_core)
    target_compile_options(luxaxis_${test_name}_tests PRIVATE -Wall -Wextra -Wpedantic)
    add_test(NAME luxaxis_${test_name}_tests COMMAND luxaxis_${test_name}_tests)
  endforeach()
endif()
