add_executable(
  NtfsDump WIN32
  "ntfsdump.cpp"
  "ntfsdumpDlg.cpp"
  "StdAfx.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/ntfsdump.clw"
  "${CMAKE_CURRENT_SOURCE_DIR}/ntfsdump.rc"
  "${CMAKE_CURRENT_SOURCE_DIR}/res/ntfsdump.rc2")
target_compile_definitions(NtfsDump PRIVATE _AFXDLL)
target_link_libraries(NtfsDump PRIVATE ${PROJECT_NAME})
set_target_properties(NtfsDump PROPERTIES WIN32_EXECUTABLE 1)
target_link_options(NtfsDump PRIVATE "/entry:wWinMainCRTStartup")
if(WIN32 AND BUILD_SHARED_LIBS)
  add_custom_command(
    TARGET NtfsDump
    POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_RUNTIME_DLLS:NtfsDump> $<TARGET_FILE_DIR:NtfsDump>
    COMMAND_EXPAND_LISTS)
endif()
