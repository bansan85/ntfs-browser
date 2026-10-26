#include "alloc/content-check.h"

#include <ntfs-browser/win-types.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstring>
#include <span>

#include <ntfs-browser/namespace.h>

namespace NTFS_BROWSER_NAMESPACE::Alloc {

namespace {

// The sector size assumed when the caller gives none: what NTFS has always
// used for its update sequence arrays.
constexpr std::size_t default_sector_size = 512;

// The four-byte signatures of NTFS metadata blocks: file records, index
// blocks, and the two kinds of $LogFile pages.
constexpr std::array<std::array<char, 4>, 4> metadata_signatures{
    {{'F', 'I', 'L', 'E'},
     {'I', 'N', 'D', 'X'},
     {'R', 'C', 'R', 'D'},
     {'R', 'S', 'T', 'R'}}};

// Whether every byte of bytes is zero.
bool AllZero(std::span<const BYTE> bytes) noexcept {
  constexpr std::size_t word = sizeof(unsigned long long);
  std::size_t at = 0;
  for (; at + word <= bytes.size(); at += word) {
    unsigned long long value = 0;
    std::memcpy(&value, bytes.data() + at, word);
    if (value != 0) {
      return false;
    }
  }
  for (; at < bytes.size(); at++) {
    if (bytes[at] != 0) {
      return false;
    }
  }
  return true;
}

// Whether the sector at the front of sector starts with a metadata signature.
bool StartsWithSignature(std::span<const BYTE> sector) noexcept {
  return std::ranges::any_of(
      metadata_signatures, [&sector](const std::array<char, 4>& signature) {
        return std::memcmp(sector.data(), signature.data(), signature.size()) ==
               0;
      });
}

}  // namespace

ClusterContent ClassifyCluster(std::span<const BYTE> cluster,
                               std::size_t sector_size) noexcept {
  if (AllZero(cluster)) {
    return ClusterContent::Zeros;
  }
  const std::size_t stride =
      sector_size != 0 ? sector_size : default_sector_size;
  for (std::size_t at = 0;
       at + metadata_signatures.front().size() <= cluster.size();
       at += stride) {
    if (StartsWithSignature(cluster.subspan(at))) {
      return ClusterContent::Metadata;
    }
  }
  return ClusterContent::Data;
}

}  // namespace NTFS_BROWSER_NAMESPACE::Alloc
