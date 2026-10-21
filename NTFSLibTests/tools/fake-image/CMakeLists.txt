# Builds synthetic NTFS images in memory, for the unit tests.
add_library(
  NtfsFakeImage
  STATIC
  "fake-ntfs-image.h"
  "fake-ntfs-image-internal.h"
  "fake-ntfs-image.cpp"
  "fake-ntfs-image-records.cpp"
  "fake-ntfs-image-attributes.cpp"
  "fake-ntfs-image-mft.cpp"
  "fake-ntfs-image-mft-scan.cpp"
  "fake-ntfs-image-mft-tree.cpp"
  "fake-ntfs-image-attr-list.cpp"
  "fake-ntfs-image-attr-data.cpp"
  "fake-ntfs-image-backup-boot.cpp"
  "fake-ntfs-image-badclus.cpp"
  "fake-ntfs-image-bitmap.cpp"
  "fake-ntfs-image-index-blocks.cpp"
  "fake-ntfs-image-index-orphans.cpp"
  "fake-ntfs-image-index-root.cpp"
  "fake-ntfs-image-compressed-file.cpp"
  "fake-ntfs-image-compressed-index.cpp"
  "fake-ntfs-image-efs.cpp"
  "fake-ntfs-image-slack.cpp"
  "fake-ntfs-image-usn.cpp"
  "file-record-header-edit.h"
)

# fuzz/ resolves the bare "named-stream-probe.h" and "gap-collation-probe.h"
# that fake-ntfs-image.h includes. src/ resolves the on-disk layouts.
target_include_directories(
  NtfsFakeImage
  PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "${CMAKE_CURRENT_SOURCE_DIR}/../../fuzz"
    "${CMAKE_CURRENT_SOURCE_DIR}/../../../src"
)

target_link_libraries(NtfsFakeImage PUBLIC ${PROJECT_NAME})

# The internal src/ headers it includes export their symbols under this macro,
# so the library and every target including them MUST define it identically. A
# normal build of NtfsBrowser never defines it. The feature macros mirror the
# library's own: efs-context.h pulls sector-cipher.h, which tests them.
target_compile_definitions(
  ${PROJECT_NAME}
  PRIVATE NTFS_BROWSER_EXPORT_INTERNALS_FOR_TESTS
)
target_compile_definitions(
  NtfsFakeImage
  PRIVATE
    NTFS_BROWSER_EXPORT_INTERNALS_FOR_TESTS
    $<$<BOOL:${NTFS_BROWSER_ENABLE_DECOMPRESSION}>:NTFS_BROWSER_ENABLE_DECOMPRESSION>
    $<$<BOOL:${NTFS_BROWSER_ENABLE_EFS_CRYPTOPP}>:NTFS_BROWSER_ENABLE_EFS_CRYPTOPP>
    $<$<BOOL:${NTFS_BROWSER_ENABLE_EFS_BCRYPT}>:NTFS_BROWSER_ENABLE_EFS_BCRYPT>
)
