add_executable(
  NtfsUndel WIN32
  "ntfsundel.cpp"
  "ntfsundelDlg.cpp"
  "StdAfx.cpp"
  ${CMAKE_CURRENT_SOURCE_DIR}/ntfsundel.clw
  ${CMAKE_CURRENT_SOURCE_DIR}/ntfsundel.rc
  ${CMAKE_CURRENT_SOURCE_DIR}/res/ntfsundel.rc2)
target_compile_definitions(NtfsUndel PRIVATE _AFXDLL)
target_link_libraries(NtfsUndel PRIVATE ${PROJECT_NAME})
set_target_properties(NtfsUndel PROPERTIES WIN32_EXECUTABLE 1)
target_link_options(NtfsUndel PRIVATE "/entry:wWinMainCRTStartup")
