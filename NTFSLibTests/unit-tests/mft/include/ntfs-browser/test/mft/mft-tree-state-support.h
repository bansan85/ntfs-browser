#pragma once

#include <optional>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include <ntfs-browser/namespace.h>
#include <ntfs-browser/test/alloc/alloc-test-support.h>

#include "mft/tree.h"
#include "win-types.h"

namespace NTFS_BROWSER_NAMESPACE {
namespace Cache {

enum class Strategy : std::uint8_t;

}  // namespace Cache
struct VolumeOptions;

}  // namespace NTFS_BROWSER_NAMESPACE

namespace NTFS_BROWSER_NAMESPACE::Test::Mft {

// A volume over an image, and the tree scanned from it. The volume lives as
// long as the tree, so a test can read its counting reader after the scan.
template <NTFS_BROWSER_NAMESPACE::Cache::Strategy S>
struct ScannedVolume {
  explicit ScannedVolume(
      std::vector<BYTE> image,
      const VolumeOptions& options =
          NTFS_BROWSER_NAMESPACE::Test::Alloc::DeletedOptions(),
      const NTFS_BROWSER_NAMESPACE::Mft::MftTree::ScanOptions& scan = {})
      : opened(NTFS_BROWSER_NAMESPACE::Test::Alloc::OpenVolume<S>(
            std::move(image), options)),
        tree(*opened.volume, scan) {}

  NTFS_BROWSER_NAMESPACE::Test::Alloc::Opened<S> opened;
  NTFS_BROWSER_NAMESPACE::Mft::MftTree tree;
};

// The entry of record, which the test cannot go on without.
inline const NTFS_BROWSER_NAMESPACE::Mft::MftTree::Entry&
    RequiredEntry(const NTFS_BROWSER_NAMESPACE::Mft::MftTree& tree,
                  ULONGLONG record) {
  const NTFS_BROWSER_NAMESPACE::Mft::MftTree::Entry* entry = tree.Find(record);
  REQUIRE(entry != nullptr);
  return *entry;
}

// Checks that no entry of tree carries a data state.
inline void
    CheckNoEntryIsAnnotated(const NTFS_BROWSER_NAMESPACE::Mft::MftTree& tree) {
  for (const NTFS_BROWSER_NAMESPACE::Mft::MftTree::Entry& entry :
       tree.Entries()) {
    CHECK_FALSE(entry.data_state);
  }
}

}  // namespace NTFS_BROWSER_NAMESPACE::Test::Mft
