if(WIN32)
  add_executable(NtfsFuzzer "main.cpp")

  # Reuses the sequential fake reader instead of duplicating it.
  target_link_libraries(
    NtfsFuzzer
    PRIVATE NtfsBrowserInternals NtfsTestDiskReaders
  )

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
  "include/ntfs-browser/test/fuzz/fuzz-core.h"
  "fuzz-core.cpp"
  "include/ntfs-browser/test/fuzz/looping-disk-reader.h"
  "looping-disk-reader.cpp"
  "include/ntfs-browser/test/fuzz/named-stream-probe.h"
  "include/ntfs-browser/test/fuzz/gap-collation-probe.h"
  "passthrough-key-provider.h"
  "include/ntfs-browser/test/fuzz/public-api-probes.h"
  "public-api-probes.cpp"
  "scan-probes.h"
  "scan-probes.cpp"
)

target_link_libraries(NtfsFuzzCore PUBLIC NtfsBrowserInternals)

target_include_directories(
  NtfsFuzzCore
  PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include"
)

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

add_subdirectory("generator")
