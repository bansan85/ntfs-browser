#pragma once

#include <cstdint>
#include <string_view>
#include <vector>

#include <ntfs-browser/namespace.h>
#include <ntfs-browser/scan.h>
#include <ntfs-browser/types.h>

#include "scan-support.h"
#include "win-types.h"

namespace NTFS_BROWSER_NAMESPACE::Test::Search {

// The byte of the test content at offset: a pattern that does not repeat inside
// a cluster, so a misplaced or short slice cannot pass for the right one.
[[nodiscard]] constexpr std::uint8_t ContentByte(std::uint64_t offset) {
  return static_cast<std::uint8_t>((offset * 7 + 3) & 0xFF);
}

// The bytes of ContentByte() from offset, count of them.
[[nodiscard]] std::vector<std::uint8_t> ContentBytes(std::uint64_t offset,
                                                     std::uint64_t count);

// Sizes of the files of ContentImage(), in bytes. data.bin is three clusters
// short of 100 bytes, so its last cluster is not full.
inline constexpr std::uint64_t data_bin_size =
    3ULL * NTFS_BROWSER_NAMESPACE::Test::FakeImage::fake_cluster_size - 100;
inline constexpr std::uint64_t holes_bin_size =
    4ULL * NTFS_BROWSER_NAMESPACE::Test::FakeImage::fake_cluster_size;
inline constexpr std::uint64_t gone_bin_size =
    NTFS_BROWSER_NAMESPACE::Test::FakeImage::fake_cluster_size;

// Files whose content sits in clusters, under the root: data.bin (a live file
// of three clusters), holes.bin (a live file of four clusters: one cluster, a
// hole of two, one cluster) and gone.bin (a deleted file of one cluster that
// no other file claims). Every real cluster holds ContentByte() of its offset
// in the file. The hole of holes.bin reads as zeros.
[[nodiscard]] std::vector<BYTE> ContentImage();

// Scans the image with the MftList engine, deleted files included, and returns
// the rows. A hard link has one row for each name.
[[nodiscard]] std::vector<Row> ListImage(const TempImage& image);

// The row called name, or a row with no name when there is none: the test then
// fails on the next check.
[[nodiscard]] Row RowNamed(const std::vector<Row>& rows,
                           std::u16string_view name);

}  // namespace NTFS_BROWSER_NAMESPACE::Test::Search
