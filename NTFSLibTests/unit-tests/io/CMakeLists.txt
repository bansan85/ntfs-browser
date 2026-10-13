target_sources(
  NtfsBrowserTests
  PRIVATE
    "disk-read-failure-tests.cpp"
    "disk-reader-tests.cpp"
    "file-reader-tests.cpp"
)

target_include_directories(
  NtfsBrowserTests
  PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}"
)

# NtfsDir and NtfsDir2 are Windows-only samples (examples/CMakeLists.txt), so
# their test exists only where the samples build.
if(WIN32)
  target_sources(NtfsBrowserTests PRIVATE "ntfsdir-sample-tests.cpp")
  add_dependencies(NtfsBrowserTests NtfsDir NtfsDir2)
  target_compile_definitions(
    NtfsBrowserTests
    PRIVATE
      NTFS_DIR_EXE="$<TARGET_FILE:NtfsDir>"
      NTFS_DIR2_EXE="$<TARGET_FILE:NtfsDir2>"
  )
endif()
