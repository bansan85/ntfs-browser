add_executable(NtfsUndel WIN32 "ntfsundel.cpp" "ntfsundelDlg.cpp" "StdAfx.cpp")
target_compile_definitions(NtfsUndel PRIVATE _AFXDLL)
target_link_libraries(NtfsUndel PRIVATE ${PROJECT_NAME})
set_target_properties(NtfsUndel PROPERTIES WIN32_EXECUTABLE 1)
