set(
  ${PROJECT_NAME}_SRC
  "alloc/alloc-loader.cpp"
  "alloc/bad-cluster-list.cpp"
  "alloc/bitmap-windows.cpp"
  "alloc/claim-inventory.cpp"
  "alloc/cluster-account.cpp"
  "alloc/cluster-bitmap.cpp"
  "alloc/cluster-map.cpp"
  "alloc/cluster-sweep.cpp"
  "alloc/content-check.cpp"
  "alloc/data-state.cpp"
  "attr/base.cpp"
  "attr/bitmap.cpp"
  "attr/compression-units.cpp"
  "attr/file-name.cpp"
  "attr/index-alloc.cpp"
  "attr/index-entry-walker.cpp"
  "attr/index-root.cpp"
  "attr/list.cpp"
  "attr/non-resident-layout.cpp"
  "attr/non-resident.cpp"
  "attr/resident.cpp"
  "attr/run-list.cpp"
  "attr/std-info.cpp"
  "attr/vol-info.cpp"
  "attr/vol-name.cpp"
  "bench/read-log.cpp"
  "bench/recorder.cpp"
  "bench/replay.cpp"
  "cache/block-cache.cpp"
  "cache/view.cpp"
  "efs/fek.cpp"
  "io/file-record.cpp"
  "io/file-record-attrs.cpp"
  "io/file-record-efs.cpp"
  "io/file-record-extensions.cpp"
  "io/file-record-index.cpp"
  "io/batch-reader.cpp"
  "io/block-batch-reader.cpp"
  "io/directory-read-ahead.cpp"
  "io/entry-prober.cpp"
  "io/orphan-block-scanner.cpp"
  "io/mft-runs.cpp"
  "io/lane-zone.cpp"
  "io/probe-lane.cpp"
  "io/raw-attr-headers.cpp"
  "io/record-loader.cpp"
  "io/record-memo.cpp"
  "io/record-window.cpp"
  "io/run-planner.cpp"
  "io/slack-name-carver.cpp"
  "filename.cpp"
  "io/file-reader.cpp"
  "io/guarded-disk-reader.cpp"
  "io/read-ahead-disk-reader.cpp"
  "io/run-block-loader.cpp"
  "attr/index-block.cpp"
  "index-entry.cpp"
  "log/log.cpp"
  "log/worker-log.cpp"
  "mft/cluster-accounts.cpp"
  "mft/data-state-resolver.cpp"
  "mft/entry-links.cpp"
  "mft/entry-table.cpp"
  "mft/extent-map.cpp"
  "mft/journal-annotator.cpp"
  "mft/metadata-opener.cpp"
  "mft/mft-bootstrap.cpp"
  "mft/path-builder.cpp"
  "mft/slot-entry.cpp"
  "mft/slot-parsers.cpp"
  "mft/slot-reader.cpp"
  "mft/slot-scan.cpp"
  "mft/tree.cpp"
  "mft/used-slots.cpp"
  "mft/waiting-reads.cpp"
  "ntfs-volume.cpp"
  "record/header.cpp"
  "record/slack-carver.cpp"
  "search/attribute-parse.cpp"
  "search/directory-engine.cpp"
  "search/engine.cpp"
  "search/entry-buffer.cpp"
  "search/exact-matcher.cpp"
  "search/field-filter.cpp"
  "search/match-sink.cpp"
  "search/matcher.cpp"
  "search/mft-list-engine.cpp"
  "search/reader-probe.cpp"
  "search/reader.cpp"
  "search/scan-exchange.cpp"
  "search/scan-probe.cpp"
  "search/log.cpp"
  "search/scan.cpp"
  "search/target.cpp"
  "search/wildcard-matcher.cpp"
  "tuning/knobs.cpp"
  "upcase/upcase-loader.cpp"
  "upcase/upcase.cpp"
  "usn/usn-journal-loader.cpp"
  "usn/usn-record-scanner.cpp"
  "utf/utf.cpp"
  "util/chunk-pool.cpp"
  "util/small-pool.cpp"
  "util/util.cpp"
  "volume/boot-sector-loader.cpp"
  "volume/volume-geometry.cpp"
  "volume/volume-record-loader.cpp"
)

if(NTFS_BROWSER_ENABLE_DECOMPRESSION)
  list(APPEND ${PROJECT_NAME}_SRC "lznt1/decompress.cpp")
endif()

if(NTFS_BROWSER_ENABLE_EFS_CRYPTOPP)
  list(APPEND ${PROJECT_NAME}_SRC "efs/cipher-cryptopp.cpp")
endif()

if(NTFS_BROWSER_ENABLE_REGEX)
  list(APPEND ${PROJECT_NAME}_SRC "search/regex-matcher.cpp")
endif()

# The backend-agnostic EFS machinery beyond fek.cpp - which key-provider-win.cpp
# needs unconditionally on Windows, so it stays in the base list above - is only
# needed, and only compiles, when at least one backend is compiled in.
# NTFS_BROWSER_EFS_MASTER (set in the root CMakeLists.txt) is that condition.
if(NTFS_BROWSER_EFS_MASTER)
  list(APPEND ${PROJECT_NAME}_SRC "efs/efs-context.cpp" "efs/efs-stream.cpp")
endif()

# Code that differs per platform lives in a -win.cpp and a -posix.cpp file
# behind one header.
if(WIN32)
  list(
    APPEND ${PROJECT_NAME}_SRC
    "bench/platform-reader-win.cpp"
    "bench/process-times-win.cpp"
    "io/extended-path-win.cpp"
    "io/host-media-kind.cpp"
    "io/async-read-pool.cpp"
    "io/read-lane.cpp"
    "io/win32-disk-reader.cpp"
    "io/win32-prefetcher.cpp"
    "efs/key-provider-win.cpp"
    "util/local-file-time-win.cpp"
  )
  if(NTFS_BROWSER_ENABLE_EFS_BCRYPT)
    list(APPEND ${PROJECT_NAME}_SRC "efs/cipher-bcrypt.cpp")
  endif()
else()
  list(
    APPEND ${PROJECT_NAME}_SRC
    "bench/platform-reader-posix.cpp"
    "bench/process-times-posix.cpp"
    "io/extended-path-posix.cpp"
    "io/posix-disk-reader.cpp"
    "util/local-file-time-posix.cpp"
  )
endif()

set(
  ${PROJECT_NAME}_H
  "alloc/cluster-account.h"
  "alloc/types.h"
  "attr/defines.h"
  "attr/header-common.h"
  "attr/type.h"
  "attr/base.h"
  "bench/bench.h"
  "bench/platform-reader.h"
  "bench/read-log.h"
  "bench/replay.h"
  "bench/types.h"
  "disk-reader.h"
  "efs/efs.h"
  "../include/ntfs-browser/namespace.h.in"
  "io/directory-read-ahead.h"
  "io/extended-path.h"
  "io/file-record.h"
  "io/guarded-disk-reader.h"
  "io/read-ahead-disk-reader.h"
  "io/record-parse-options.h"
  "io/record-standing.h"
  "index-entry.h"
  "log/log.h"
  "log/sink.h"
  "log/spdlog-adapter.h"
  "attr/mask.h"
  "mft/idx.h"
  "mft/tree.h"
  "record/carved-name.h"
  "../include/ntfs-browser/log.h"
  "../include/ntfs-browser/reader-types.h"
  "../include/ntfs-browser/reader.h"
  "../include/ntfs-browser/scan.h"
  "../include/ntfs-browser/types.h"
  "ntfs-volume.h"
  "cache/stats.h"
  "cache/strategy.h"
  "cache/view.h"
  "tuning/knobs.h"
  "tuning/tuning.h"
  "usn/types.h"
  "volume-options.h"
  "alloc/alloc-loader.h"
  "alloc/bad-cluster-list.h"
  "alloc/bitmap-windows.h"
  "alloc/claim-inventory.h"
  "alloc/cluster-bitmap.h"
  "alloc/cluster-map.h"
  "alloc/cluster-sweep.h"
  "alloc/content-check.h"
  "alloc/data-state.h"
  "attr/flags.h"
  "attr/forward.h"
  "attr/compression-units.h"
  "bench/process-times.h"
  "bench/recorder.h"
  "attr/bitmap.h"
  "attr/data.h"
  "attr/file-name.h"
  "attr/index-alloc.h"
  "attr/index-entry-walker.h"
  "attr/index-root.h"
  "attr/list-chain.h"
  "attr/list.h"
  "attr/non-resident-layout.h"
  "attr/non-resident.h"
  "attr/resident.h"
  "attr/run-list.h"
  "attr/slot.h"
  "attr/std-info.h"
  "attr/vol-info.h"
  "attr/vol-name.h"
  "cache/block-cache.h"
  "data/attribute-list.h"
  "data/file-record-flag.h"
  "data/file-record-header.h"
  "data/filename-flag.h"
  "data/filename-namespace.h"
  "data/filename.h"
  "data/header-non-resident.h"
  "data/header-resident.h"
  "data/index-block.h"
  "data/index-entry-flag.h"
  "data/index-entry.h"
  "data/index-root.h"
  "data/ntfs-bpb.h"
  "data/run-entry.h"
  "data/standard-information.h"
  "data/std-info-permission.h"
  "data/usn-record.h"
  "data/volume-information.h"
  "efs/cipher-bcrypt.h"
  "efs/cipher-cryptopp.h"
  "efs/efs-context.h"
  "efs/efs-stream.h"
  "efs/fek.h"
  "efs/key-provider-win.h"
  "efs/sector-cipher.h"
  "io/file-reader.h"
  "io/file-record-attr-types.h"
  "io/file-record-attrs.h"
  "io/file-record-efs.h"
  "io/file-record-extensions.h"
  "io/file-record-impl.h"
  "io/file-record-index.h"
  "io/directory-read-ahead-testing.h"
  "io/batch-reader.h"
  "io/block-batch-reader.h"
  "io/entry-prober.h"
  "io/orphan-block-scanner.h"
  "io/mft-runs.h"
  "io/lane-zone.h"
  "io/probe-lane.h"
  "io/raw-attr-headers.h"
  "io/record-loader.h"
  "io/read-ahead-held.h"
  "io/record-memo.h"
  "io/record-window.h"
  "io/run-planner.h"
  "io/slack-name-carver.h"
  "io/run-block-loader.h"
  "attr/index-block.h"
  "internal-export.h"
  "lznt1/decompress.h"
  "mft/cluster-accounts.h"
  "mft/data-state-resolver.h"
  "mft/entry-links.h"
  "mft/entry-table.h"
  "mft/extent-map.h"
  "mft/file-reference.h"
  "mft/held-records.h"
  "mft/journal-annotator.h"
  "mft/metadata-opener.h"
  "mft/mft-bootstrap.h"
  "mft/path-builder.h"
  "mft/record-offset.h"
  "mft/slot-entry.h"
  "mft/slot-parsers.h"
  "mft/slot-reader.h"
  "mft/slot-scan.h"
  "mft/tree-testing.h"
  "mft/used-slots.h"
  "mft/waiting-reads.h"
  "log/ntfs-common.h"
  "log/worker-log.h"
  "ntfs-volume-impl.h"
  "record/header.h"
  "record/slack-carver.h"
  "upcase/upcase-loader.h"
  "upcase/upcase.h"
  "usn/usn-journal-loader.h"
  "usn/usn-record-scanner.h"
  "utf/utf.h"
  "util/chunk-pool.h"
  "util/local-file-time.h"
  "util/small-pool.h"
  "util/util.h"
  "volume/boot-sector-loader.h"
  "volume/volume-geometry.h"
  "volume/volume-record-loader.h"
  "io/host-media-kind.h"
  "io/posix-disk-reader.h"
  "io/async-read-pool.h"
  "io/read-lane.h"
  "io/win32-disk-reader.h"
  "io/win32-prefetcher.h"
  "search/attribute-parse.h"
  "search/case-folder.h"
  "search/directory-engine.h"
  "search/engine.h"
  "search/entry-buffer.h"
  "search/exact-matcher.h"
  "search/field-filter.h"
  "search/match-sink.h"
  "search/matcher.h"
  "search/mft-list-engine.h"
  "search/reader-probe.h"
  "search/regex-matcher.h"
  "search/scan-exchange.h"
  "search/scan-probe.h"
  "search/target.h"
  "search/wildcard-matcher.h"
)

set(${PROJECT_NAME}_CMAKE "${CMAKE_CURRENT_SOURCE_DIR}/CMakeLists.txt")

add_library(
  ${PROJECT_NAME}
  ${${PROJECT_NAME}_SRC}
  ${${PROJECT_NAME}_H}
  ${${PROJECT_NAME}_CMAKE}
)

include(GenerateExportHeader)
# Per-symbol export/import, replacing a blanket
# CMAKE_WINDOWS_EXPORT_ALL_SYMBOLS: only the types of the public headers
# (include/ntfs-browser/*.h) carry NTFS_BROWSER_EXPORT. An internal
# symbol that the tests reach carries NTFS_BROWSER_EXPORT_TESTS_ONLY
# (internal-export.h). A static build (the default) gets an empty macro, so
# this header is safe to include either way.
generate_export_header(
  ${PROJECT_NAME}
  BASE_NAME NTFS_BROWSER
  EXPORT_FILE_NAME "${CMAKE_CURRENT_BINARY_DIR}/include/ntfs-browser/export.h"
)

# The root namespace name is baked into the generated header rather than passed
# as a compile definition, so every consumer reads the same value from the
# installed header and none has to repeat the CMake variable.
configure_file(
  "${CMAKE_CURRENT_SOURCE_DIR}/../include/ntfs-browser/namespace.h.in"
  "${CMAKE_CURRENT_BINARY_DIR}/include/ntfs-browser/namespace.h"
  @ONLY
)

target_link_libraries(${PROJECT_NAME} PUBLIC Microsoft.GSL::GSL)
# Private: the read-ahead worker of Io::ReadAheadDiskReader is a std::thread.
find_package(Threads REQUIRED)
target_link_libraries(${PROJECT_NAME} PRIVATE Threads::Threads)
# Private: spdlog is an implementation detail, absent from the public API.
target_link_libraries(${PROJECT_NAME} PRIVATE spdlog::spdlog)
# Private too: frozen's maps back internal static lookups only, never a public
# header.
target_link_libraries(${PROJECT_NAME} PRIVATE frozen::frozen)
# Private too: Crypto++ and the Windows crypto libraries stay out of the API.
# The shipped library stays Crypto++-free whenever it does not itself use that
# backend.
if(NTFS_BROWSER_ENABLE_EFS_CRYPTOPP)
  target_link_libraries(${PROJECT_NAME} PRIVATE cryptopp::cryptopp)
endif()
# Private too: PCRE2 is only used by src/search/regex-matcher.cpp.
if(NTFS_BROWSER_ENABLE_REGEX)
  if(NTFS_BROWSER_USE_INSTALLED_PCRE2)
    target_link_libraries(${PROJECT_NAME} PRIVATE PCRE2::16BIT)
  else()
    target_link_libraries(${PROJECT_NAME} PRIVATE pcre2-16)
  endif()
endif()
if(WIN32)
  target_link_libraries(${PROJECT_NAME} PRIVATE bcrypt crypt32 ncrypt)
endif()
target_include_directories(
  ${PROJECT_NAME}
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/../include>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
  # The internal headers. Private: a consumer of the library sees the public
  # headers only. Unlike MSVC, GCC doesn't search transitively-including files'
  # directories.
  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}
)
target_compile_definitions(${PROJECT_NAME} PUBLIC "_UNICODE" "UNICODE")
# Feed the options through as macros to the private .cpp files that test
# them with #ifdef/#if defined(...).
target_compile_definitions(
  ${PROJECT_NAME}
  PRIVATE
    $<$<BOOL:${NTFS_BROWSER_ENABLE_DECOMPRESSION}>:NTFS_BROWSER_ENABLE_DECOMPRESSION>
    $<$<BOOL:${NTFS_BROWSER_ENABLE_EFS_CRYPTOPP}>:NTFS_BROWSER_ENABLE_EFS_CRYPTOPP>
    $<$<BOOL:${NTFS_BROWSER_ENABLE_EFS_BCRYPT}>:NTFS_BROWSER_ENABLE_EFS_BCRYPT>
    $<$<BOOL:${NTFS_BROWSER_ENABLE_REGEX}>:NTFS_BROWSER_ENABLE_REGEX>
)

group_src(${${PROJECT_NAME}_SRC} ${${PROJECT_NAME}_H} ${${PROJECT_NAME}_CMAKE})
