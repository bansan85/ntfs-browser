# Embedded in the console tools on Windows, see the file for what it does.
set(
  NTFS_BROWSER_LONG_PATH_MANIFEST
  "${CMAKE_CURRENT_SOURCE_DIR}/tools/long-path-aware.manifest"
)

# NtfsFuzzGen writes the fuzz inputs and the corpus traces here, and the unit
# tests read them: data/ for the inputs, data/corpus-traces/ for the traces.
set(NTFS_FUZZGEN_OUTPUT_DIR "${CMAKE_BINARY_DIR}/fuzz-generated")

add_subdirectory("tools")
add_subdirectory("examples")
add_subdirectory("fuzz")
add_subdirectory("unit-tests")
