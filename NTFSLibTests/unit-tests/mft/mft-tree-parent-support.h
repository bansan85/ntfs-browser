#pragma once

#include <cstddef>
#include <vector>

#include <ntfs-browser/namespace.h>
#include <ntfs-browser/test/fake-image/fake-ntfs-image.h>
#include <ntfs-browser/test/volume/volume-tree-support.h>

#include "attr/type.h"
#include "win-types.h"

namespace NTFS_BROWSER_NAMESPACE::Test::Mft {

// Mask for the 48-bit record number inside a file reference.
inline constexpr ULONGLONG ref_record_mask = 0x0000FFFFFFFFFFFFULL;

// Sequence numbers the MFT tree fixture gives its root, Docs and NewDir.
inline constexpr ULONGLONG root_sequence = 5;
inline constexpr ULONGLONG docs_sequence = 1;
inline constexpr ULONGLONG new_dir_sequence = 7;

// Builds a file reference from a record number and a sequence number.
[[nodiscard]] constexpr ULONGLONG MakeRef(ULONGLONG record,
                                          ULONGLONG sequence) {
  return record | (sequence << 48U);
}

// Writes new_ref over every $FILE_NAME of idx that names from_record.
inline void RedirectFileNames(std::vector<BYTE>& image, ULONGLONG idx,
                              ULONGLONG from_record, ULONGLONG new_ref) {
  for (const size_t attr :
       AttrOffsets(image, idx, NTFS_BROWSER_NAMESPACE::Attr::Type::FileName)) {
    const size_t body = ResidentBody(image, attr);
    if ((Load<ULONGLONG>(image, body) & ref_record_mask) == from_record) {
      Store<ULONGLONG>(image, body, new_ref);
    }
  }
}

// Makes Docs and NewDir of the MFT tree fixture name each other as parent, so
// neither reaches the root.
inline void LoopDocsAndNewDir(std::vector<BYTE>& image) {
  RedirectFileNames(
      image, Test::FakeImage::mft_tree_docs_idx, root_idx,
      MakeRef(Test::FakeImage::mft_tree_reused_dir_idx, new_dir_sequence));
  RedirectFileNames(image, Test::FakeImage::mft_tree_reused_dir_idx, root_idx,
                    MakeRef(Test::FakeImage::mft_tree_docs_idx, docs_sequence));
}

}  // namespace NTFS_BROWSER_NAMESPACE::Test::Mft
