cmake_minimum_required(VERSION 3.10)

project(NtfsBrowser)

include("cmake/vs.cmake")

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED True)

option(BUILD_SHARED_LIBS "Build using shared libraries" OFF)

set(CMAKE_WINDOWS_EXPORT_ALL_SYMBOLS ON)

add_subdirectory("3rdparty/gsl")
add_subdirectory("3rdparty/Catch2")

# spdlog's own examples, tests and benchmarks are not this project's, and its
# tests would otherwise join ctest.
set(SPDLOG_BUILD_EXAMPLE
    OFF
    CACHE BOOL "" FORCE)
set(SPDLOG_BUILD_TESTS
    OFF
    CACHE BOOL "" FORCE)
set(SPDLOG_BUILD_BENCH
    OFF
    CACHE BOOL "" FORCE)
# Vendored headers are not this project's code to lint.
set(SPDLOG_SYSTEM_INCLUDES
    ON
    CACHE BOOL "" FORCE)
# Log::Config::file_path is a std::filesystem::path, whose native form is wide
# on Windows. Without this spdlog takes a narrow file name and opens it through
# the active ANSI code page, which cannot express every path. spdlog forces this
# option back off on the platforms where it does not apply, so it needs no
# if(WIN32) here.
set(SPDLOG_WCHAR_FILENAMES
    ON
    CACHE BOOL "" FORCE)
add_subdirectory("3rdparty/spdlog")
# spdlog already exports its own symbols with __declspec. Letting CMake generate
# a .def for it on top makes the linker warn about every one.
if(TARGET spdlog)
  set_target_properties(spdlog PROPERTIES WINDOWS_EXPORT_ALL_SYMBOLS OFF)
endif()
add_subdirectory("src")

enable_testing()
add_subdirectory("NTFSLibTests")
