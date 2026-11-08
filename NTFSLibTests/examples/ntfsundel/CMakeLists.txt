# The search, recovery and command-line logic, shared with the unit tests
# (unit-tests/io/ntfsundel-*-tests.cpp). main.cpp only hands over argv.
add_library(
  NtfsUndelCore
  STATIC
  "include/ntfs-browser/example/ntfsundel/ntfsundel-cli.h"
  "ntfsundel-cli.cpp"
  "include/ntfs-browser/example/ntfsundel/ntfsundel-core.h"
  "ntfsundel-core.cpp"
  "include/ntfs-browser/example/ntfsundel/name-matcher.h"
  "name-matcher.cpp"
)
target_include_directories(
  NtfsUndelCore
  PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include"
)
target_link_libraries(NtfsUndelCore PUBLIC ${PROJECT_NAME} NtfsDeviceAccess)

# --name folds case through the volume's $UpCase table. NameMatcher calls
# UpCase::Table::Map(), an internal symbol that the library exports only under
# this macro (see tools/fake-image/CMakeLists.txt, which defines it on the
# library). The tool MUST define it too, so both sides agree on the export.
target_include_directories(
  NtfsUndelCore
  PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../../../src"
)
target_compile_definitions(
  NtfsUndelCore
  PRIVATE NTFS_BROWSER_EXPORT_INTERNALS_FOR_TESTS
)

add_executable(NtfsUndel WIN32 "main.cpp")
target_link_libraries(NtfsUndel PRIVATE ${PROJECT_NAME} NtfsUndelCore)
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
