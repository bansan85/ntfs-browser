# The search, recovery and command-line logic, shared with the unit tests
# (unit-tests/io/ntfsundel-*-tests.cpp). main.cpp only hands over argv.
add_library(
  NtfsUndelCore
  STATIC
  "include/ntfs-browser/example/ntfsundel/ntfsundel-cli.h"
  "ntfsundel-cli.cpp"
  "include/ntfs-browser/example/ntfsundel/ntfsundel-core.h"
  "ntfsundel-core.cpp"
)
target_include_directories(
  NtfsUndelCore
  PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include"
)
target_link_libraries(
  NtfsUndelCore
  PUBLIC NtfsBrowserInternals NtfsDeviceAccess
)

add_executable(NtfsUndel WIN32 "main.cpp")
target_link_libraries(NtfsUndel PRIVATE NtfsBrowserInternals NtfsUndelCore)
if(WIN32)
  target_sources(NtfsUndel PRIVATE "${NTFS_BROWSER_LONG_PATH_MANIFEST}")
endif()
set_target_properties(NtfsUndel PROPERTIES WIN32_EXECUTABLE 0)
if(WIN32 AND BUILD_SHARED_LIBS)
  add_custom_command(
    TARGET NtfsUndel
    POST_BUILD
    COMMAND
      ${CMAKE_COMMAND} -E copy_if_different $<TARGET_RUNTIME_DLLS:NtfsUndel>
      $<TARGET_FILE_DIR:NtfsUndel>
    COMMAND_EXPAND_LISTS
  )
endif()
