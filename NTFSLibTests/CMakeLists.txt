# Embedded in the console tools on Windows, see the file for what it does.
set(
  NTFS_BROWSER_LONG_PATH_MANIFEST
  "${CMAKE_CURRENT_SOURCE_DIR}/tools/long-path-aware.manifest"
)

# The internal headers of the library live in src/. The library target keeps
# that folder private: a test, a tool or a fuzzer that needs them links this
# target. It also carries NTFS_BROWSER_EXPORT_INTERNALS_FOR_TESTS, the macro
# that makes a shared library export the internal symbols (see
# src/internal-export.h). The library MUST define it too, in the same builds, so
# that both sides agree. A build without tests never defines it, and then the
# shared library exports Search::Scan and Search::Reader only.
add_library(NtfsBrowserInternals INTERFACE)
target_include_directories(
  NtfsBrowserInternals
  INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/../src"
)
target_compile_definitions(
  NtfsBrowserInternals
  INTERFACE NTFS_BROWSER_EXPORT_INTERNALS_FOR_TESTS
)
target_link_libraries(NtfsBrowserInternals INTERFACE ${PROJECT_NAME})
target_compile_definitions(
  ${PROJECT_NAME}
  PRIVATE NTFS_BROWSER_EXPORT_INTERNALS_FOR_TESTS
)

# NtfsFuzzGen writes the fuzz inputs and the corpus traces here, and the unit
# tests read them: data/ for the inputs, data/corpus-traces/ for the traces.
set(NTFS_FUZZGEN_OUTPUT_DIR "${CMAKE_BINARY_DIR}/fuzz-generated")

add_subdirectory("tools")
add_subdirectory("examples")
add_subdirectory("fuzz")
add_subdirectory("public-api")
add_subdirectory("unit-tests")
