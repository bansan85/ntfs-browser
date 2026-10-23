#pragma once

#include <ntfs-browser/win-types.h>

#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include <ntfs-browser/cache/strategy.h>
#include <ntfs-browser/mft/tree.h>
#include <ntfs-browser/namespace.h>
#include <ntfs-browser/volume-options.h>

#include "alloc-test-support.h"

namespace NTFS_BROWSER_NAMESPACE::Tests::MftTreeSupport {

// A volume over an image, and the tree scanned from it. The volume lives as
// long as the tree, so a test can read its counting reader after the scan.
template <Cache::Strategy S>
struct ScannedVolume {
  explicit ScannedVolume(
      std::vector<BYTE> image,
      const VolumeOptions& options = AllocSupport::DeletedOptions(),
      const Mft::MftTree::ScanOptions& scan = {})
      : opened(AllocSupport::OpenVolume<S>(std::move(image), options)),
        tree(*opened.volume, scan) {}

  AllocSupport::Opened<S> opened;
  Mft::MftTree tree;
};

// The entry of record, which the test cannot go on without.
inline const Mft::MftTree::Entry& RequiredEntry(const Mft::MftTree& tree,
                                                ULONGLONG record) {
  const Mft::MftTree::Entry* entry = tree.Find(record);
  REQUIRE(entry != nullptr);
  return *entry;
}

// Checks that no entry of tree carries a data state.
inline void CheckNoEntryIsAnnotated(const Mft::MftTree& tree) {
  for (const Mft::MftTree::Entry& entry : tree.Entries()) {
    CHECK_FALSE(entry.data_state);
  }
}

}  // namespace NTFS_BROWSER_NAMESPACE::Tests::MftTreeSupport
