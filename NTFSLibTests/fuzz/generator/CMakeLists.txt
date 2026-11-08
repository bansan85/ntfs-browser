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
  "include/ntfs-browser/tool/fuzzgen/transient-damage-reader.h"
  "transient-damage-reader.cpp"
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
  "include/ntfs-browser/tool/fuzzgen/data-run-lcn-scenarios.h"
  "data-run-lcn-scenarios.cpp"
  "include/ntfs-browser/tool/fuzzgen/root-index-edit.h"
  "root-index-edit.cpp"
  "include/ntfs-browser/tool/fuzzgen/efs-fixture-scenarios.h"
  "efs-fixture-scenarios.cpp"
  "include/ntfs-browser/tool/fuzzgen/compressed-file-scenarios.h"
  "compressed-file-scenarios.cpp"
  "include/ntfs-browser/tool/fuzzgen/legacy-volume-scenarios.h"
  "legacy-volume-scenarios.cpp"
  "include/ntfs-browser/tool/fuzzgen/legacy-record.h"
  "legacy-record.cpp"
  "include/ntfs-browser/tool/fuzzgen/legacy-index.h"
  "legacy-index.cpp"
  "include/ntfs-browser/tool/fuzzgen/hand-made-efs.h"
  "hand-made-efs.cpp"
  "include/ntfs-browser/tool/fuzzgen/upcase-table.h"
  "upcase-table.cpp"
  "include/ntfs-browser/tool/fuzzgen/legacy-name-scenarios.h"
  "legacy-name-scenarios.cpp"
  "include/ntfs-browser/tool/fuzzgen/mft-huge-scan-scenarios.h"
  "mft-huge-scan-scenarios.cpp"
  "include/ntfs-browser/tool/fuzzgen/layer-line.h"
  "layer-line.cpp"
  "include/ntfs-browser/tool/fuzzgen/attribute-values.h"
  "attribute-values.cpp"
  "include/ntfs-browser/tool/fuzzgen/update-sequence.h"
  "update-sequence.cpp"
  "include/ntfs-browser/tool/fuzzgen/index-layer.h"
  "index-layer.cpp"
  "include/ntfs-browser/tool/fuzzgen/record-layer.h"
  "record-layer.cpp"
  "include/ntfs-browser/tool/fuzzgen/boot-sector-layer.h"
  "boot-sector-layer.cpp"
  "include/ntfs-browser/tool/fuzzgen/layer-script.h"
  "layer-script.cpp"
  "include/ntfs-browser/tool/fuzzgen/counter-text.h"
  "counter-text.cpp"
  "include/ntfs-browser/tool/fuzzgen/lznt1-compressor.h"
  "lznt1-compressor.cpp"
  "include/ntfs-browser/tool/fuzzgen/sparse-image.h"
  "sparse-image.cpp"
  "include/ntfs-browser/tool/fuzzgen/sparse-disk-reader.h"
  "sparse-disk-reader.cpp"
  "include/ntfs-browser/tool/fuzzgen/corpus-probes.h"
  "corpus-probes.cpp"
  "include/ntfs-browser/tool/fuzzgen/corpus-trace-scenarios.h"
  "corpus-trace-scenarios.cpp"
)

target_include_directories(
  NtfsFuzzGenCore
  PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include"
)

target_link_libraries(
  NtfsFuzzGenCore
  PUBLIC ${PROJECT_NAME} NtfsFuzzCore NtfsFakeImage NtfsTestDiskReaders
)

# The EFS inputs encrypt their data with the cipher of the unit tests.
if(NTFS_BROWSER_EFS_MASTER)
  target_link_libraries(NtfsFuzzGenCore PUBLIC NtfsEfsTestSupport)
endif()

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
# ${CMAKE_CURRENT_SOURCE_DIR}/resources. make-corpus-residue.py makes them.
set(NTFS_FUZZGEN_ARCHIVES "dftt.tar.xz" "ntfs-samples.tar.xz")

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
