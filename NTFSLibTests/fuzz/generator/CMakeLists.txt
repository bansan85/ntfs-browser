# NtfsFuzzGen writes the saved fuzz inputs of NTFSLibTests/fuzz/data from code,
# and checks the committed ones against what the library reads today.
# Portable C++, so it builds on Linux and Windows alike.
add_library(
  NtfsFuzzGenCore
  STATIC
  "include/ntfs-browser/tool/fuzzgen/build-features.h"
  "build-features.cpp"
  "include/ntfs-browser/tool/fuzzgen/bpb-edit.h"
  "bpb-edit.cpp"
  "include/ntfs-browser/tool/fuzzgen/hex-bytes.h"
  "hex-bytes.cpp"
  "include/ntfs-browser/tool/fuzzgen/byte-writer.h"
  "byte-writer.cpp"
  "include/ntfs-browser/tool/fuzzgen/command-line.h"
  "command-line.cpp"
  "include/ntfs-browser/tool/fuzzgen/generator-app.h"
  "generator-app.cpp"
  "include/ntfs-browser/tool/fuzzgen/recorded-stream.h"
  "recorded-stream.cpp"
  "include/ntfs-browser/tool/fuzzgen/recording-reader.h"
  "recording-reader.cpp"
  "include/ntfs-browser/tool/fuzzgen/resource-locator.h"
  "resource-locator.cpp"
  "include/ntfs-browser/tool/fuzzgen/scenario.h"
  "include/ntfs-browser/tool/fuzzgen/scenario-catalog.h"
  "scenario-catalog.cpp"
  "include/ntfs-browser/tool/fuzzgen/scenario-registry.h"
  "scenario-registry.cpp"
  "include/ntfs-browser/tool/fuzzgen/stream-comparator.h"
  "stream-comparator.cpp"
  # The scenario families, one per group of saved inputs.
  "include/ntfs-browser/tool/fuzzgen/alloc-scan-scenarios.h"
  "alloc-scan-scenarios.cpp"
  "include/ntfs-browser/tool/fuzzgen/bad-cluster-scenarios.h"
  "bad-cluster-scenarios.cpp"
  "include/ntfs-browser/tool/fuzzgen/cluster-state-scenarios.h"
  "cluster-state-scenarios.cpp"
  "include/ntfs-browser/tool/fuzzgen/usn-journal-scenarios.h"
  "usn-journal-scenarios.cpp"
  "include/ntfs-browser/tool/fuzzgen/usn-tree-scenarios.h"
  "usn-tree-scenarios.cpp"
  "include/ntfs-browser/tool/fuzzgen/boot-sector-scenarios.h"
  "boot-sector-scenarios.cpp"
  "include/ntfs-browser/tool/fuzzgen/tiny-stream-scenarios.h"
  "tiny-stream-scenarios.cpp"
  "include/ntfs-browser/tool/fuzzgen/cluster-bitmap-scenarios.h"
  "cluster-bitmap-scenarios.cpp"
  "include/ntfs-browser/tool/fuzzgen/case-twin-scenarios.h"
  "case-twin-scenarios.cpp"
  "include/ntfs-browser/tool/fuzzgen/mft-scan-scenarios.h"
  "mft-scan-scenarios.cpp"
  "include/ntfs-browser/tool/fuzzgen/run-list-scenarios.h"
  "run-list-scenarios.cpp"
  "include/ntfs-browser/tool/fuzzgen/attribute-list-scan-scenarios.h"
  "attribute-list-scan-scenarios.cpp"
  "include/ntfs-browser/tool/fuzzgen/index-slack-scenarios.h"
  "index-slack-scenarios.cpp"
  "include/ntfs-browser/tool/fuzzgen/record-slack-scenarios.h"
  "record-slack-scenarios.cpp"
  "include/ntfs-browser/tool/fuzzgen/slack-names-scenarios.h"
  "slack-names-scenarios.cpp"
  "include/ntfs-browser/tool/fuzzgen/record-edit.h"
  "record-edit.cpp"
  "include/ntfs-browser/tool/fuzzgen/index-edit.h"
  "index-edit.cpp"
  "include/ntfs-browser/tool/fuzzgen/legacy-geometry.h"
  "legacy-geometry.cpp"
  "include/ntfs-browser/tool/fuzzgen/legacy-fixtures.h"
  "legacy-fixtures.cpp"
  "include/ntfs-browser/tool/fuzzgen/mft-fixture-scenarios.h"
  "mft-fixture-scenarios.cpp"
  "include/ntfs-browser/tool/fuzzgen/file-record-scenarios.h"
  "file-record-scenarios.cpp"
  "include/ntfs-browser/tool/fuzzgen/volume-record-scenarios.h"
  "volume-record-scenarios.cpp"
  "include/ntfs-browser/tool/fuzzgen/attribute-list-fixture-scenarios.h"
  "attribute-list-fixture-scenarios.cpp"
  "include/ntfs-browser/tool/fuzzgen/index-fixture-scenarios.h"
  "index-fixture-scenarios.cpp"
  "include/ntfs-browser/tool/fuzzgen/legacy-index-root-scenarios.h"
  "legacy-index-root-scenarios.cpp"
  "include/ntfs-browser/tool/fuzzgen/legacy-root-record-scenarios.h"
  "legacy-root-record-scenarios.cpp"
  "include/ntfs-browser/tool/fuzzgen/legacy-builder-scenarios.h"
  "legacy-builder-scenarios.cpp"
  "include/ntfs-browser/tool/fuzzgen/legacy-attribute-list-scenarios.h"
  "legacy-attribute-list-scenarios.cpp"
  "include/ntfs-browser/tool/fuzzgen/legacy-index-block-scenarios.h"
  "legacy-index-block-scenarios.cpp"
  "include/ntfs-browser/tool/fuzzgen/legacy-residency-scenarios.h"
  "legacy-residency-scenarios.cpp"
)

target_include_directories(
  NtfsFuzzGenCore
  PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include"
)

target_link_libraries(
  NtfsFuzzGenCore
  PUBLIC ${PROJECT_NAME} NtfsFuzzCore NtfsFakeImage NtfsTestDiskReaders
)

# BuildFeatures tells what the build contains, the way fuzzer-expected-messages.h
# does for the tests.
target_compile_definitions(
  NtfsFuzzGenCore
  PRIVATE
    $<$<BOOL:${NTFS_BROWSER_ENABLE_DECOMPRESSION}>:NTFS_BROWSER_ENABLE_DECOMPRESSION>
    $<$<BOOL:${NTFS_BROWSER_ENABLE_EFS_CRYPTOPP}>:NTFS_BROWSER_ENABLE_EFS_CRYPTOPP>
    $<$<BOOL:${NTFS_BROWSER_ENABLE_EFS_BCRYPT}>:NTFS_BROWSER_ENABLE_EFS_BCRYPT>
)

# The generated files land in the build tree, never in the source tree.
set(NTFS_FUZZGEN_OUTPUT_DIR "${CMAKE_BINARY_DIR}/fuzz-generated")

# Where the resource archives are extracted: one subfolder per archive.
set(NTFS_FUZZGEN_RESOURCES_DIR "${NTFS_FUZZGEN_OUTPUT_DIR}/resources")

# The archives that hold what code cannot compute (file contents, journal data
# of a forensic image), one .tar.xz per source image, in
# ${CMAKE_CURRENT_SOURCE_DIR}/resources. None is needed yet.
set(NTFS_FUZZGEN_ARCHIVES)

target_compile_definitions(
  NtfsFuzzGenCore
  PUBLIC NTFS_FUZZGEN_RESOURCES_DIR="${NTFS_FUZZGEN_RESOURCES_DIR}"
)

add_executable(NtfsFuzzGen "main.cpp")
target_link_libraries(NtfsFuzzGen PRIVATE NtfsFuzzGenCore)

if(WIN32 AND BUILD_SHARED_LIBS)
  add_custom_command(
    TARGET NtfsFuzzGen
    POST_BUILD
    COMMAND
      ${CMAKE_COMMAND} -E copy_if_different $<TARGET_RUNTIME_DLLS:NtfsFuzzGen>
      $<TARGET_FILE_DIR:NtfsFuzzGen>
    COMMAND_EXPAND_LISTS
  )
endif()

# Extracts each archive into <resources dir>/<name of the archive without
# .tar.xz>, at build time, with cmake itself: no external xz is needed. The stamp
# file lets the extraction run again only when the archive changes.
set(stamps)
foreach(file_name IN LISTS NTFS_FUZZGEN_ARCHIVES)
  set(archive "${CMAKE_CURRENT_SOURCE_DIR}/resources/${file_name}")
  string(REGEX REPLACE "[.]tar[.]xz$" "" stem "${file_name}")
  set(folder "${NTFS_FUZZGEN_RESOURCES_DIR}/${stem}")
  # The working directory of the command MUST exist when it starts.
  file(MAKE_DIRECTORY "${folder}")
  add_custom_command(
    OUTPUT "${folder}.stamp"
    COMMAND ${CMAKE_COMMAND} -E tar xf "${archive}"
    COMMAND ${CMAKE_COMMAND} -E touch "${folder}.stamp"
    WORKING_DIRECTORY "${folder}"
    DEPENDS "${archive}"
    COMMENT "Extracting ${file_name}"
    VERBATIM
  )
  list(APPEND stamps "${folder}.stamp")
endforeach()

if(stamps)
  add_custom_target(NtfsFuzzGenResources DEPENDS ${stamps})
  add_dependencies(NtfsFuzzGen NtfsFuzzGenResources)
endif()

# Writes every generated file under the build tree. Not part of ALL.
add_custom_target(
  NtfsFuzzGenData
  COMMAND
    NtfsFuzzGen --out "${NTFS_FUZZGEN_OUTPUT_DIR}/data" --resources
    "${NTFS_FUZZGEN_RESOURCES_DIR}"
  COMMENT "Generating the fuzz inputs"
  VERBATIM
)
add_dependencies(NtfsFuzzGenData NtfsFuzzGen)

# Each scenario the generator has must stay identical to its committed file (or
# be the start of the stream that the library reads today).
add_test(
  NAME NtfsFuzzGenVerify
  COMMAND
    NtfsFuzzGen --verify "${CMAKE_CURRENT_SOURCE_DIR}/../data" --resources
    "${NTFS_FUZZGEN_RESOURCES_DIR}"
)
