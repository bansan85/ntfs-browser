#pragma once

#include <ntfs-browser/win-types.h>

#include <cstddef>
#include <vector>

#include <ntfs-browser/namespace.h>

namespace NTFS_BROWSER_NAMESPACE::Tool::FuzzGen {

// The update sequence array that protects a record or an index block: the last
// word of each 512-byte sector moves into the array, and the sequence number
// takes its place.
class UpdateSequence {
 public:
  // Bytes in the sector that one word of the array protects.
  static constexpr std::size_t sector_bytes = 512;

  // Writes number at array_offset, saves the last word of each sector of
  // bytes after it in the array, and puts number in its place. Throws
  // std::out_of_range when the array does not fit in bytes.
  static void Protect(std::vector<BYTE>& bytes, std::size_t array_offset,
                      WORD number);
};

}  // namespace NTFS_BROWSER_NAMESPACE::Tool::FuzzGen
