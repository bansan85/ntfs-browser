# NtfsFuzzGen writes the fuzz inputs and the corpus traces that the unit tests
# and the AFL fuzzer read, from code, with the library's current read order.
# Portable C++, so it builds on Linux and Windows alike.
add_library(
  NtfsFuzzGenCore
  STATIC
  "include/ntfs-browser/tool/fuzzgen/build-features.h"
  "build-features.cpp"
  "bpb-edit.h"
  "bpb-edit.cpp"
  "hex-bytes.h"
  "hex-bytes.cpp"
  "byte-writer.h"
  "byte-writer.cpp"
  "include/ntfs-browser/tool/fuzzgen/command-line.h"
  "command-line.cpp"
  "generator-app.h"
  "generator-app.cpp"
  "include/ntfs-browser/tool/fuzzgen/recorded-stream.h"
  "recorded-stream.cpp"
  "recording-reader.h"
  "recording-reader.cpp"
  "resource-locator.h"
  "resource-locator.cpp"
  "include/ntfs-browser/tool/fuzzgen/scenario.h"
  "include/ntfs-browser/tool/fuzzgen/scenario-catalog.h"
  "scenario-catalog.cpp"
  "include/ntfs-browser/tool/fuzzgen/scenario-registry.h"
  "scenario-registry.cpp"
  "stream-comparator.h"
  "stream-comparator.cpp"
  "transient-damage-reader.h"
  "transient-damage-reader.cpp"
  # The scenario families, one per group of saved inputs.
  "alloc-scan-scenarios.h"
  "alloc-scan-scenarios.cpp"
  "bad-cluster-scenarios.h"
  "bad-cluster-scenarios.cpp"
  "cluster-state-scenarios.h"
  "cluster-state-scenarios.cpp"
  "usn-journal-scenarios.h"
  "usn-journal-scenarios.cpp"
  "usn-tree-scenarios.h"
  "usn-tree-scenarios.cpp"
  "boot-sector-scenarios.h"
  "boot-sector-scenarios.cpp"
  "tiny-stream-scenarios.h"
  "tiny-stream-scenarios.cpp"
  "cluster-bitmap-scenarios.h"
  "cluster-bitmap-scenarios.cpp"
  "case-twin-scenarios.h"
  "case-twin-scenarios.cpp"
  "mft-scan-scenarios.h"
  "mft-scan-scenarios.cpp"
  "run-list-scenarios.h"
  "run-list-scenarios.cpp"
  "attribute-list-scan-scenarios.h"
  "attribute-list-scan-scenarios.cpp"
  "index-slack-scenarios.h"
  "index-slack-scenarios.cpp"
  "record-slack-scenarios.h"
  "record-slack-scenarios.cpp"
  "slack-names-scenarios.h"
  "slack-names-scenarios.cpp"
  "record-edit.h"
  "record-edit.cpp"
  "index-edit.h"
  "index-edit.cpp"
  "legacy-geometry.h"
  "legacy-geometry.cpp"
  "legacy-fixtures.h"
  "legacy-fixtures.cpp"
  "mft-fixture-scenarios.h"
  "mft-fixture-scenarios.cpp"
  "file-record-scenarios.h"
  "file-record-scenarios.cpp"
  "volume-record-scenarios.h"
  "volume-record-scenarios.cpp"
  "attribute-list-fixture-scenarios.h"
  "attribute-list-fixture-scenarios.cpp"
  "index-fixture-scenarios.h"
  "index-fixture-scenarios.cpp"
  "legacy-index-root-scenarios.h"
  "legacy-index-root-scenarios.cpp"
  "legacy-root-record-scenarios.h"
  "legacy-root-record-scenarios.cpp"
  "legacy-builder-scenarios.h"
  "legacy-builder-scenarios.cpp"
  "legacy-attribute-list-scenarios.h"
  "legacy-attribute-list-scenarios.cpp"
  "legacy-index-block-scenarios.h"
  "legacy-index-block-scenarios.cpp"
  "legacy-residency-scenarios.h"
  "legacy-residency-scenarios.cpp"
  "data-run-lcn-scenarios.h"
  "data-run-lcn-scenarios.cpp"
  "root-index-edit.h"
  "root-index-edit.cpp"
  "efs-fixture-scenarios.h"
  "efs-fixture-scenarios.cpp"
  "compressed-file-scenarios.h"
  "compressed-file-scenarios.cpp"
  "legacy-volume-scenarios.h"
  "legacy-volume-scenarios.cpp"
  "legacy-record.h"
  "legacy-record.cpp"
  "legacy-index.h"
  "legacy-index.cpp"
  "hand-made-efs.h"
  "hand-made-efs.cpp"
  "upcase-table.h"
  "upcase-table.cpp"
  "legacy-name-scenarios.h"
  "legacy-name-scenarios.cpp"
  "mft-huge-scan-scenarios.h"
  "mft-huge-scan-scenarios.cpp"
  "layer-line.h"
  "layer-line.cpp"
  "attribute-values.h"
  "attribute-values.cpp"
  "update-sequence.h"
  "update-sequence.cpp"
  "index-layer.h"
  "index-layer.cpp"
  "record-layer.h"
  "record-layer.cpp"
  "boot-sector-layer.h"
  "boot-sector-layer.cpp"
  "layer-script.h"
  "layer-script.cpp"
  "counter-text.h"
  "counter-text.cpp"
  "lznt1-compressor.h"
  "lznt1-compressor.cpp"
  "sparse-image.h"
  "sparse-image.cpp"
  "sparse-disk-reader.h"
  "sparse-disk-reader.cpp"
  "corpus-probes.h"
  "corpus-probes.cpp"
  "corpus-trace-scenarios.h"
  "corpus-trace-scenarios.cpp"
)

target_include_directories(
  NtfsFuzzGenCore
  PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include"
)

target_link_libraries(
  NtfsFuzzGenCore
  PUBLIC NtfsBrowserInternals NtfsFuzzCore NtfsFakeImage NtfsTestDiskReaders
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

# The generated files land in the build tree (NTFS_FUZZGEN_OUTPUT_DIR, set in
# NTFSLibTests/CMakeLists.txt), never in the source tree.

# Where the resource archives are extracted: one subfolder per archive.
set(NTFS_FUZZGEN_RESOURCES_DIR "${NTFS_FUZZGEN_OUTPUT_DIR}/resources")

# The archives that hold what code cannot compute (file contents, journal data
# of a forensic image), one .tar.xz per source image, in
# ${CMAKE_CURRENT_SOURCE_DIR}/resources. make-corpus-residue.py makes them.
set(NTFS_FUZZGEN_ARCHIVES "dftt.tar.xz" "ntfs-samples.tar.xz" "nps.tar.xz")

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

# Writes every generated file under the build tree, in <output dir>/data
# (the traces in its corpus-traces subfolder). The stamp makes the run
# incremental: it happens again only when the generator, the library or an
# extracted archive changes. The folder is emptied first, so that a scenario
# that no longer exists leaves no stale file. Test targets that read the files
# depend on this target.
set(NTFS_FUZZGEN_DATA_STAMP "${NTFS_FUZZGEN_OUTPUT_DIR}/data.stamp")
add_custom_command(
  OUTPUT "${NTFS_FUZZGEN_DATA_STAMP}"
  COMMAND ${CMAKE_COMMAND} -E rm -rf "${NTFS_FUZZGEN_OUTPUT_DIR}/data"
  COMMAND
    NtfsFuzzGen --out "${NTFS_FUZZGEN_OUTPUT_DIR}/data" --resources
    "${NTFS_FUZZGEN_RESOURCES_DIR}"
  COMMAND ${CMAKE_COMMAND} -E touch "${NTFS_FUZZGEN_DATA_STAMP}"
  # The library is a DLL in a shared build: relinking the generator is not
  # enough to notice that it changed.
  DEPENDS NtfsFuzzGen NtfsBrowser ${stamps}
  COMMENT "Generating the fuzz inputs"
  VERBATIM
)
add_custom_target(NtfsFuzzGenData DEPENDS "${NTFS_FUZZGEN_DATA_STAMP}")

# Two runs must write the same bytes: a scenario never depends on the clock, on
# a random seed or on the order of a directory listing.
add_test(
  NAME NtfsFuzzGenDeterminism
  COMMAND
    ${CMAKE_COMMAND} -DGENERATOR=$<TARGET_FILE:NtfsFuzzGen>
    -DRESOURCES=${NTFS_FUZZGEN_RESOURCES_DIR}
    -DWORK=${NTFS_FUZZGEN_OUTPUT_DIR}/determinism -P
    "${CMAKE_CURRENT_SOURCE_DIR}/check-determinism.cmake"
)

# The seed corpus of AFL and libFuzzer, on demand (not part of ALL): the fuzz
# inputs without the corpus traces, flat in <output dir>/afl-seeds.
add_custom_target(
  NtfsFuzzSeedCorpus
  COMMAND ${CMAKE_COMMAND} -E rm -rf "${NTFS_FUZZGEN_OUTPUT_DIR}/afl-seeds"
  COMMAND
    NtfsFuzzGen --seeds "${NTFS_FUZZGEN_OUTPUT_DIR}/afl-seeds" --resources
    "${NTFS_FUZZGEN_RESOURCES_DIR}"
  COMMENT "Writing the AFL seed corpus"
  VERBATIM
)
