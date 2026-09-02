# Scoped to this build tree, so only tests can assert on NTFS_TRACE* output.
target_compile_definitions(${PROJECT_NAME} PRIVATE NTFS_BROWSER_ENABLE_TRACE)

if(WIN32)
  add_subdirectory("ntfsdir")
  add_subdirectory("ntfsattr")
  add_subdirectory("ntfsdump")
  add_subdirectory("ntfsundel")
  add_subdirectory("unit-tests")
endif()
add_subdirectory("fuzz")
