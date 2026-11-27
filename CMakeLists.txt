cmake_minimum_required(VERSION 3.24)
project(ScreamerRecomp VERSION 1.0.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

option(SCREAMER_BUILD_TESTS "Build the test suite" ON)

# Where the user's original game files live (never committed).
set(SCREAMER_DATA_DIR "${CMAKE_SOURCE_DIR}/SCREAMER" CACHE PATH "Path to the original Screamer install")

# Release packages (tools\package.ps1, preset "dist"): the C/C++ runtime linked into each
# binary (no Visual C++ redistributable needed), the game as a Windows-subsystem program (no
# console window), and no build machine's game path compiled in.
option(SCREAMER_DIST "Build for a release package" OFF)
if(SCREAMER_DIST AND MSVC)
  set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

include(cmake/Dependencies.cmake)

if(MSVC)
  add_compile_options(/W4 /permissive- /utf-8 /MP)
  add_compile_definitions(_CRT_SECURE_NO_WARNINGS)
else()
  add_compile_options(-Wall -Wextra)
endif()

add_subdirectory(src)

if(SCREAMER_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
