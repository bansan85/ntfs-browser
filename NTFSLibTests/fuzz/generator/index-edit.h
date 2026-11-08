#pragma once

#include <ntfs-browser/win-types.h>

#include <cstddef>
#include <vector>

#include <ntfs-browser/namespace.h>

namespace NTFS_BROWSER_NAMESPACE::Tool::FuzzGen {

// Edits the index entries of a fake image: the entries of an $INDEX_ROOT or of
// an index block.
class IndexEdit {
 public:
  // Cuts count bytes out of the image at offset. The bytes up to limit (not
  // included) move down, and the count bytes that free up at the end are
  // zero. Throws std::out_of_range when limit lies past the image.
  static void Remove(std::vector<BYTE>& image, std::size_t offset,
                     std::size_t count, std::size_t limit);

  // Offset of the first index block in the image: the first place that holds
  // the "INDX" magic. Throws std::out_of_range when the image has none.
  [[nodiscard]] static std::size_t FindBlock(const std::vector<BYTE>& image);

  // Gives the index entries of the $INDEX_ROOT of record idx the size they had
  // before the builders padded them, and fixes the sizes of the index and of
  // the attribute. The record is one of a volume of one-sector records.
  static void UnpadRoot(std::vector<BYTE>& image, ULONGLONG idx);

  // The same for the index block that starts at offset. The block has one
  // sector. The entries start where the block header says they do.
  static void UnpadBlock(std::vector<BYTE>& image, std::size_t offset);

  // The old builders did not pad an index entry to 8 bytes. Gives each entry
  // of the run that starts at first the size it has without the padding that
  // sits between its stream and its sub-node VCN, or at its end. The run ends
  // with the last entry. The bytes after the run move down, up to limit (not
  // included). Returns how many bytes the run lost. The sizes of the
  // structures that hold the run are for the caller to fix.
  static DWORD Unpad(std::vector<BYTE>& image, std::size_t first,
                     std::size_t limit);
};

}  // namespace NTFS_BROWSER_NAMESPACE::Tool::FuzzGen
