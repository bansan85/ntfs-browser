# Proves that the public headers stand alone. The target links the library and
# nothing else: it never gets NtfsBrowserInternals, so the src/ folder is out of
# its include path, exactly as for a program that uses the library.
add_executable(NtfsPublicApiCheck "main.cpp")
target_link_libraries(NtfsPublicApiCheck PRIVATE ${PROJECT_NAME})

if(WIN32 AND BUILD_SHARED_LIBS)
  add_custom_command(
    TARGET NtfsPublicApiCheck
    POST_BUILD
    COMMAND
      ${CMAKE_COMMAND} -E copy_if_different
      $<TARGET_RUNTIME_DLLS:NtfsPublicApiCheck>
      $<TARGET_FILE_DIR:NtfsPublicApiCheck>
    COMMAND_EXPAND_LISTS
  )
endif()

add_test(NAME PublicApiCheck COMMAND NtfsPublicApiCheck)
