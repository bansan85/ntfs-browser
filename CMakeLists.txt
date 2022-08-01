cmake_minimum_required(VERSION 3.10)

project(NtfsBrowser)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED True)

add_subdirectory("src")

add_subdirectory("NTFSLibTests")
