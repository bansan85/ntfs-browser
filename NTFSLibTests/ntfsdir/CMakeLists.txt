add_executable(NtfsDir WIN32 "main.cpp")
target_link_libraries(NtfsDir PRIVATE ${PROJECT_NAME})
set_target_properties(NtfsDir PROPERTIES WIN32_EXECUTABLE 0)
if(WIN32 AND BUILD_SHARED_LIBS)
  add_custom_command(
    TARGET NtfsDir
    POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_RUNTIME_DLLS:NtfsDir>
            $<TARGET_FILE_DIR:NtfsDir>
    COMMAND_EXPAND_LISTS)
endif()
