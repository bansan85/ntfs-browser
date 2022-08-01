add_executable(NtfsDir WIN32 "main.cpp")
target_link_libraries(NtfsDir PRIVATE ${PROJECT_NAME})
set_target_properties(NtfsDir PROPERTIES WIN32_EXECUTABLE 0)
