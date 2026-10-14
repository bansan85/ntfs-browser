# NtfsCompare's walkers and comparison (tools/ntfscompare/), tested on temp
# directories, fake images and the engine's own listings.
target_sources(
  NtfsBrowserTests
  PRIVATE
    "compare-engine-tests.cpp"
    "library-walk-tests.cpp"
    "os-api-walk-tests.cpp"
    "volume-open-tests.cpp"
)

target_link_libraries(NtfsBrowserTests PRIVATE NtfsCompareCore)

# NtfsCompare is a Windows-driven sample on the command line, and its
# argument checks run before any volume is opened, so these tests need no disk.
if(WIN32)
  target_sources(NtfsBrowserTests PRIVATE "ntfscompare-sample-tests.cpp")
  add_dependencies(NtfsBrowserTests NtfsCompare)
  target_compile_definitions(
    NtfsBrowserTests
    PRIVATE NTFS_COMPARE_EXE="$<TARGET_FILE:NtfsCompare>"
  )
endif()
