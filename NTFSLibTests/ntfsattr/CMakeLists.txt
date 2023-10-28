add_executable(
  NtfsAttr WIN32
  "ntfsattr.cpp"
  "ntfsattrDlg.cpp"
  "StdAfx.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/ntfsattr.clw"
  "${CMAKE_CURRENT_SOURCE_DIR}/ntfsattr.rc"
  "${CMAKE_CURRENT_SOURCE_DIR}/res/ntfsattr.rc2")
target_compile_definitions(NtfsAttr PRIVATE _AFXDLL)
target_link_libraries(NtfsAttr PRIVATE ${PROJECT_NAME})
set_target_properties(NtfsAttr PROPERTIES WIN32_EXECUTABLE 1)
target_link_options(NtfsAttr PRIVATE "/entry:wWinMainCRTStartup")
if(WIN32 AND BUILD_SHARED_LIBS)
  add_custom_command(
    TARGET NtfsAttr
    POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_RUNTIME_DLLS:NtfsAttr> $<TARGET_FILE_DIR:NtfsAttr>
    COMMAND_EXPAND_LISTS)
endif()
