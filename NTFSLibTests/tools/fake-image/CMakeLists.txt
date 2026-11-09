# Builds synthetic NTFS images in memory, for the unit tests.
add_library(
  NtfsFakeImage
  STATIC
  "include/ntfs-browser/test/fake-image/fake-ntfs-image.h"
  "fake-ntfs-image-internal.h"
  "fake-ntfs-image.cpp"
  "fake-ntfs-image-records.cpp"
  "fake-ntfs-image-attributes.cpp"
  "fake-ntfs-image-mft.cpp"
  "fake-ntfs-image-mft-scan.cpp"
  "fake-ntfs-image-mft-tree.cpp"
  "fake-ntfs-image-name-tree.cpp"
  "fake-ntfs-image-attr-list.cpp"
  "fake-ntfs-image-attr-data.cpp"
  "fake-ntfs-image-backup-boot.cpp"
  "fake-ntfs-image-account.cpp"
  "include/ntfs-browser/test/fake-image/account-volume.h"
  "fake-ntfs-image-account-volume.cpp"
  "fake-ntfs-image-badclus.cpp"
  "fake-ntfs-image-bitmap.cpp"
  "fake-ntfs-image-index-blocks.cpp"
  "fake-ntfs-image-index-orphans.cpp"
  "fake-ntfs-image-index-slack.cpp"
  "fake-ntfs-image-index-root.cpp"
  "fake-ntfs-image-index-writers.cpp"
  "fake-ntfs-image-compressed-file.cpp"
  "fake-ntfs-image-compressed-index.cpp"
  "fake-ntfs-image-efs.cpp"
  "fake-ntfs-image-slack.cpp"
  "include/ntfs-browser/test/fake-image/slack-inputs.h"
  "fake-ntfs-image-slack-inputs.cpp"
  "fake-ntfs-image-usn.cpp"
  "include/ntfs-browser/test/fake-image/file-record-header-edit.h"
  "include/ntfs-browser/test/usn/usn-journal-record.h"
)

# fuzz/include resolves the named-stream-probe.h and gap-collation-probe.h that
# fake-ntfs-image.h includes. NtfsBrowserInternals resolves the on-disk layouts.
target_include_directories(
  NtfsFakeImage
  PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/../../fuzz/include"
)

target_link_libraries(NtfsFakeImage PUBLIC NtfsBrowserInternals)

# The feature macros mirror the library's own: efs-context.h pulls
# sector-cipher.h, which tests them.
target_compile_definitions(
  NtfsFakeImage
  PRIVATE
    $<$<BOOL:${NTFS_BROWSER_ENABLE_DECOMPRESSION}>:NTFS_BROWSER_ENABLE_DECOMPRESSION>
    $<$<BOOL:${NTFS_BROWSER_ENABLE_EFS_CRYPTOPP}>:NTFS_BROWSER_ENABLE_EFS_CRYPTOPP>
    $<$<BOOL:${NTFS_BROWSER_ENABLE_EFS_BCRYPT}>:NTFS_BROWSER_ENABLE_EFS_BCRYPT>
)
