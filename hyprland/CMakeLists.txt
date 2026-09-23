cmake_minimum_required(VERSION 3.25)

project(luxaxis-hyprland VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(LUXAXIS_BUILD_PLUGIN "Build the Hyprland plugin" ON)
option(LUXAXIS_BUILD_TESTS "Build core unit tests" ON)

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

