if(WIN32)
  add_executable(
    NtfsFuzzer
    "main.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/../unit-tests/sequential-disk-reader.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/../unit-tests/sequential-disk-reader.cpp")

  target_link_libraries(NtfsFuzzer PRIVATE ${PROJECT_NAME})

  # Reuses the unit tests' sequential fake reader instead of duplicating it.
  target_include_directories(
    NtfsFuzzer PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../unit-tests")

  if(BUILD_SHARED_LIBS)
    add_custom_command(
      TARGET NtfsFuzzer
      POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_if_different
              $<TARGET_RUNTIME_DLLS:NtfsFuzzer> $<TARGET_FILE_DIR:NtfsFuzzer>
      COMMAND_EXPAND_LISTS)
  endif()
endif()

# Classic afl-gcc/afl-g++ compatible harness: reads its testcase from a file
# path (argv[1]), no Win32 API usage, so it also compiles on Linux. Portable
# C++, so also built on Windows as a plain (uninstrumented) executable for local
# smoke-testing.
add_executable(NtfsFuzzerAfl "afl-main.cpp" "looping-disk-reader.h"
                             "looping-disk-reader.cpp")

target_link_libraries(NtfsFuzzerAfl PRIVATE ${PROJECT_NAME})

if(WIN32 AND BUILD_SHARED_LIBS)
  add_custom_command(
    TARGET NtfsFuzzerAfl
    POST_BUILD
    COMMAND
      ${CMAKE_COMMAND} -E copy_if_different $<TARGET_RUNTIME_DLLS:NtfsFuzzerAfl>
      $<TARGET_FILE_DIR:NtfsFuzzerAfl>
    COMMAND_EXPAND_LISTS)
endif()
