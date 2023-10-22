cmake_minimum_required(VERSION 3.10)

project(NtfsBrowser)

include("cmake/vs.cmake")

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED True)

add_subdirectory("3rdparty/gsl")
add_subdirectory("src")

add_subdirectory("NTFSLibTests")
