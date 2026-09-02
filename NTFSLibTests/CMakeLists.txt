# Turns NTFS_TRACE*/src/ntfs-common.h from a no-op into real stdout output for
# this build tree only, so NtfsBrowserTests/NtfsFuzzerAfl can assert on the
# parse-error messages the library reports. Consumers that link NtfsBrowser
# without building NTFSLibTests never get this define and see the library's
# normal silent behavior.
target_compile_definitions(${PROJECT_NAME} PRIVATE NTFS_BROWSER_ENABLE_TRACE)

if(WIN32)
  add_subdirectory("ntfsdir")
  add_subdirectory("ntfsattr")
  add_subdirectory("ntfsdump")
  add_subdirectory("ntfsundel")
  add_subdirectory("unit-tests")
endif()
add_subdirectory("fuzz")
