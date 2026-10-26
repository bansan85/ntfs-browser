# The listing logic, shared by both samples. Each sample picks the cache
# strategy of its volume in its own wmain (main on POSIX).
add_library(NtfsDirCore STATIC "ntfsdir-core.cpp")
target_include_directories(NtfsDirCore PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(NtfsDirCore PUBLIC ${PROJECT_NAME} NtfsDeviceAccess)

add_executable(NtfsDir WIN32 "main.cpp")
target_link_libraries(NtfsDir PRIVATE ${PROJECT_NAME} NtfsDirCore)
if(WIN32)
  target_sources(NtfsDir PRIVATE "${NTFS_BROWSER_LONG_PATH_MANIFEST}")
endif()
set_target_properties(NtfsDir PROPERTIES WIN32_EXECUTABLE 0)
if(WIN32 AND BUILD_SHARED_LIBS)
  add_custom_command(
    TARGET NtfsDir
    POST_BUILD
    COMMAND
      ${CMAKE_COMMAND} -E copy_if_different $<TARGET_RUNTIME_DLLS:NtfsDir>
      $<TARGET_FILE_DIR:NtfsDir>
    COMMAND_EXPAND_LISTS
  )
endif()

# NtfsDir2 stays a Windows-only sample.
if(WIN32)
  add_executable(NtfsDir2 WIN32 "main2.cpp")
  target_link_libraries(NtfsDir2 PRIVATE ${PROJECT_NAME} NtfsDirCore)
  target_sources(NtfsDir2 PRIVATE "${NTFS_BROWSER_LONG_PATH_MANIFEST}")
  set_target_properties(NtfsDir2 PROPERTIES WIN32_EXECUTABLE 0)
  if(BUILD_SHARED_LIBS)
    add_custom_command(
      TARGET NtfsDir2
      POST_BUILD
      COMMAND
        ${CMAKE_COMMAND} -E copy_if_different $<TARGET_RUNTIME_DLLS:NtfsDir2>
        $<TARGET_FILE_DIR:NtfsDir2>
      COMMAND_EXPAND_LISTS
    )
  endif()
endif()
