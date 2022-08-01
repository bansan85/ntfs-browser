add_executable(NtfsDump WIN32 "ntfsdump.cpp" "ntfsdumpDlg.cpp" "StdAfx.cpp")
target_compile_definitions(NtfsDump PRIVATE _AFXDLL)
target_link_libraries(NtfsDump PRIVATE ${PROJECT_NAME})
set_target_properties(NtfsDump PROPERTIES WIN32_EXECUTABLE 1)
