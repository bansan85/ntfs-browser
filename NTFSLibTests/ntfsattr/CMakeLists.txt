add_executable(NtfsAttr WIN32 "ntfsattr.cpp" "ntfsattrDlg.cpp" "StdAfx.cpp")
target_compile_definitions(NtfsAttr PRIVATE _AFXDLL)
target_link_libraries(NtfsAttr PRIVATE ${PROJECT_NAME})
set_target_properties(NtfsAttr PROPERTIES WIN32_EXECUTABLE 1)
