# The dump logic, with no MFC in it, so the unit tests can link it too
# (unit-tests/io/ntfsdump-core-tests.cpp).
add_library(NtfsDumpCore STATIC "ntfsdump-core.cpp")
target_include_directories(NtfsDumpCore PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(NtfsDumpCore PUBLIC ${PROJECT_NAME})

# The MFC dialog app. clang-cl cannot build MFC: the root option turns it off.
if(NTFS_BROWSER_BUILD_MFC_APPS)
  add_executable(
    NtfsDump
    WIN32
    "ntfsdump.cpp"
    "ntfsdumpDlg.cpp"
    "StdAfx.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/ntfsdump.clw"
    "${CMAKE_CURRENT_SOURCE_DIR}/ntfsdump.rc"
    "${CMAKE_CURRENT_SOURCE_DIR}/res/ntfsdump.rc2"
  )
  target_compile_definitions(NtfsDump PRIVATE _AFXDLL)
  target_link_libraries(NtfsDump PRIVATE ${PROJECT_NAME} NtfsDumpCore)
  set_target_properties(NtfsDump PROPERTIES WIN32_EXECUTABLE 1)
  target_link_options(NtfsDump PRIVATE "/entry:wWinMainCRTStartup")
  if(WIN32 AND BUILD_SHARED_LIBS)
    add_custom_command(
      TARGET NtfsDump
      POST_BUILD
      COMMAND
        ${CMAKE_COMMAND} -E copy_if_different $<TARGET_RUNTIME_DLLS:NtfsDump>
        $<TARGET_FILE_DIR:NtfsDump>
      COMMAND_EXPAND_LISTS
    )
  endif()
endif()
