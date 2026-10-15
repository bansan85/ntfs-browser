if(WIN32)
  add_executable(NtfsFuzzer "main.cpp")

  # Reuses the sequential fake reader instead of duplicating it.
  target_link_libraries(NtfsFuzzer PRIVATE ${PROJECT_NAME} NtfsTestDiskReaders)

  if(BUILD_SHARED_LIBS)
    add_custom_command(
      TARGET NtfsFuzzer
      POST_BUILD
      COMMAND
        ${CMAKE_COMMAND} -E copy_if_different $<TARGET_RUNTIME_DLLS:NtfsFuzzer>
        $<TARGET_FILE_DIR:NtfsFuzzer>
      COMMAND_EXPAND_LISTS
    )
  endif()
endif()

# The AFL fuzzing logic, also driven in-process by the regression tests.
# Portable C++, no Win32 API, so it builds on Linux and Windows alike.
add_library(
  NtfsFuzzCore
  STATIC
  "fuzz-core.h"
  "fuzz-core.cpp"
  "looping-disk-reader.h"
  "looping-disk-reader.cpp"
  "named-stream-probe.h"
  "gap-collation-probe.h"
)

target_link_libraries(NtfsFuzzCore PUBLIC ${PROJECT_NAME})

# Portable C++, no Win32 API, so it builds on Linux and Windows alike.
add_executable(NtfsFuzzerAfl "afl-main.cpp")

target_link_libraries(NtfsFuzzerAfl PRIVATE NtfsFuzzCore)

if(WIN32 AND BUILD_SHARED_LIBS)
  add_custom_command(
    TARGET NtfsFuzzerAfl
    POST_BUILD
    COMMAND
      ${CMAKE_COMMAND} -E copy_if_different $<TARGET_RUNTIME_DLLS:NtfsFuzzerAfl>
      $<TARGET_FILE_DIR:NtfsFuzzerAfl>
    COMMAND_EXPAND_LISTS
  )
endif()
