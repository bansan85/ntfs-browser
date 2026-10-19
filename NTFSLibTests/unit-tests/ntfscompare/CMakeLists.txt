# NtfsCompare's walkers and comparison (tools/ntfscompare/), tested on temp
# directories, fake images and the engine's own listings.
target_sources(
  NtfsBrowserTests
  PRIVATE
    "compare-engine-tests.cpp"
    "library-walk-tests.cpp"
    "ntfscompare-sample-tests.cpp"
    "os-api-walk-tests.cpp"
    "volume-open-tests.cpp"
)

target_link_libraries(NtfsBrowserTests PRIVATE NtfsCompareCore)

# NtfsCompare's argument checks run before any volume is opened, and its device
# form runs on a temp image file, so these tests need no real disk.
add_dependencies(NtfsBrowserTests NtfsCompare)
target_compile_definitions(
  NtfsBrowserTests
  PRIVATE NTFS_COMPARE_EXE="$<TARGET_FILE:NtfsCompare>"
)
