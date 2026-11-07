# The attribute dump logic, shared with the unit tests
# (unit-tests/io/ntfsattr-core-tests.cpp). The command line, the volume opening
# and the run all live here. main.cpp only hands over argv.
add_library(
  NtfsAttrCore
  STATIC
  "include/ntfs-browser/example/ntfsattr/ntfsattr-core.h"
  "ntfsattr-core.cpp"
)
target_include_directories(
  NtfsAttrCore
  PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include"
)
target_link_libraries(NtfsAttrCore PUBLIC ${PROJECT_NAME} NtfsDeviceAccess)

add_executable(NtfsAttr WIN32 "main.cpp")
target_link_libraries(NtfsAttr PRIVATE ${PROJECT_NAME} NtfsAttrCore)
if(WIN32)
  target_sources(NtfsAttr PRIVATE "${NTFS_BROWSER_LONG_PATH_MANIFEST}")
endif()
set_target_properties(NtfsAttr PROPERTIES WIN32_EXECUTABLE 0)
if(WIN32 AND BUILD_SHARED_LIBS)
  add_custom_command(
    TARGET NtfsAttr
    POST_BUILD
    COMMAND
      ${CMAKE_COMMAND} -E copy_if_different $<TARGET_RUNTIME_DLLS:NtfsAttr>
      $<TARGET_FILE_DIR:NtfsAttr>
    COMMAND_EXPAND_LISTS
  )
endif()
