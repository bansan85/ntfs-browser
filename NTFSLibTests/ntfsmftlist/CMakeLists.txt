add_executable(NtfsMftList WIN32 "main.cpp")
target_link_libraries(NtfsMftList PRIVATE ${PROJECT_NAME})
set_target_properties(NtfsMftList PROPERTIES WIN32_EXECUTABLE 0)
if(WIN32 AND BUILD_SHARED_LIBS)
  add_custom_command(
    TARGET NtfsMftList
    POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_RUNTIME_DLLS:NtfsMftList> $<TARGET_FILE_DIR:NtfsMftList>
    COMMAND_EXPAND_LISTS)
endif()
