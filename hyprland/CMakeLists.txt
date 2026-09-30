cmake_minimum_required(VERSION 3.25)

project(luxaxis-hyprland VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(LUXAXIS_BUILD_PLUGIN "Build the Hyprland plugin" ON)
option(LUXAXIS_BUILD_TESTS "Build core unit tests" ON)
option(LUXAXIS_BUILD_GPU_TESTS "Run GLES shaders in a surfaceless EGL context" ON)
option(LUXAXIS_BUILD_LIFECYCLE_TEST "Test loading in the installed Hyprland without a GPU" OFF)

include(CTest)

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

add_library(luxaxis_core STATIC src/core/config.cpp src/core/engine.cpp src/core/image_cache.cpp src/core/spotlight.cpp src/core/transition.cpp)
target_include_directories(luxaxis_core PUBLIC include)
target_link_libraries(luxaxis_core PRIVATE tomlplusplus::tomlplusplus Threads::Threads)
target_compile_options(luxaxis_core PRIVATE -Wall -Wextra -Wpedantic)
set_target_properties(luxaxis_core PROPERTIES POSITION_INDEPENDENT_CODE ON)

if(LUXAXIS_BUILD_PLUGIN OR (BUILD_TESTING AND LUXAXIS_BUILD_TESTS AND LUXAXIS_BUILD_GPU_TESTS))
  find_package(PkgConfig REQUIRED)
  pkg_check_modules(GLES REQUIRED IMPORTED_TARGET glesv2)
  add_library(luxaxis_gpu STATIC src/gpu_renderer.cpp)
  target_include_directories(luxaxis_gpu PUBLIC include)
  target_link_libraries(luxaxis_gpu PUBLIC luxaxis_core PkgConfig::GLES)
  target_compile_options(luxaxis_gpu PRIVATE -Wall -Wextra -Wpedantic)
  set_target_properties(luxaxis_gpu PROPERTIES POSITION_INDEPENDENT_CODE ON)
endif()

if(LUXAXIS_BUILD_PLUGIN)
  find_package(PkgConfig REQUIRED)
  pkg_check_modules(HYPRLAND REQUIRED IMPORTED_TARGET hyprland=0.56.2)

  add_library(luxaxis MODULE src/plugin.cpp src/hyprland_adapter.cpp)
  target_include_directories(luxaxis PRIVATE include)
  target_link_libraries(luxaxis PRIVATE luxaxis_gpu PkgConfig::HYPRLAND)
  target_compile_options(luxaxis PRIVATE -Wall -Wextra -Wpedantic)
  if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    target_compile_options(luxaxis PRIVATE -fno-gnu-unique)
  endif()
  # Hyprland declares compositor globals as inline variables in its headers.
  # They must bind to the host's objects instead of private, empty copies.
  set_target_properties(
    luxaxis
    PROPERTIES
      PREFIX ""
      CXX_VISIBILITY_PRESET default
      VISIBILITY_INLINES_HIDDEN NO
  )
endif()

if(BUILD_TESTING AND LUXAXIS_BUILD_TESTS)
  add_executable(luxaxis_hyprpm_manifest_tests tests/hyprpm_manifest_test.cpp)
  target_link_libraries(luxaxis_hyprpm_manifest_tests PRIVATE tomlplusplus::tomlplusplus)
  target_compile_options(luxaxis_hyprpm_manifest_tests PRIVATE -Wall -Wextra -Wpedantic)
  add_test(NAME luxaxis_hyprpm_manifest_tests
    COMMAND luxaxis_hyprpm_manifest_tests ${CMAKE_CURRENT_SOURCE_DIR}/../hyprpm.toml)

  foreach(test_name IN ITEMS config engine image_cache spotlight transition)
    add_executable(luxaxis_${test_name}_tests tests/${test_name}_test.cpp)
    target_link_libraries(luxaxis_${test_name}_tests PRIVATE luxaxis_core)
    target_compile_options(luxaxis_${test_name}_tests PRIVATE -Wall -Wextra -Wpedantic)
    if(test_name STREQUAL "config")
      add_test(NAME luxaxis_config_tests
        COMMAND luxaxis_config_tests ${CMAKE_CURRENT_SOURCE_DIR}/../README.md)
    else()
      add_test(NAME luxaxis_${test_name}_tests COMMAND luxaxis_${test_name}_tests)
    endif()
  endforeach()
  if(LUXAXIS_BUILD_GPU_TESTS)
    pkg_check_modules(EGL REQUIRED IMPORTED_TARGET egl)
    add_executable(luxaxis_gpu_tests tests/gpu_test.cpp)
    target_link_libraries(luxaxis_gpu_tests PRIVATE luxaxis_gpu PkgConfig::EGL)
    target_compile_options(luxaxis_gpu_tests PRIVATE -Wall -Wextra -Wpedantic)
    add_test(NAME luxaxis_gpu_tests COMMAND luxaxis_gpu_tests)
    set_tests_properties(luxaxis_gpu_tests PROPERTIES
      ENVIRONMENT "EGL_PLATFORM=surfaceless;LIBGL_ALWAYS_SOFTWARE=1"
      TIMEOUT 60)
  endif()
  if(TARGET luxaxis)
    add_test(
      NAME luxaxis_host_symbols_test
      COMMAND ${CMAKE_COMMAND} -DPLUGIN=$<TARGET_FILE:luxaxis> -DNM=${CMAKE_NM}
        -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/host_symbols_test.cmake
    )
  endif()
  if(LUXAXIS_BUILD_LIFECYCLE_TEST)
    if(NOT TARGET luxaxis)
      message(FATAL_ERROR "The lifecycle test requires LUXAXIS_BUILD_PLUGIN=ON")
    endif()
    find_program(HYPRLAND_EXECUTABLE Hyprland REQUIRED)
    add_library(luxaxis_lifecycle_probe MODULE tests/lifecycle_probe.cpp)
    target_link_libraries(luxaxis_lifecycle_probe PRIVATE PkgConfig::HYPRLAND)
    add_test(
      NAME luxaxis_lifecycle_test
      COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tests/lifecycle_test.sh
        ${HYPRLAND_EXECUTABLE} $<TARGET_FILE:luxaxis_lifecycle_probe> $<TARGET_FILE:luxaxis>
    )
    set_tests_properties(luxaxis_lifecycle_test PROPERTIES TIMEOUT 15)
  endif()
endif()
