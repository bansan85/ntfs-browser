#include <ntfs-browser/win-types.h>

#include <cstddef>
#include <cstring>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <catch2/catch_template_test_macros.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <ntfs-browser/attr/type.h>
#include <ntfs-browser/cache/strategy.h>
#include <ntfs-browser/index-entry.h>
#include <ntfs-browser/io/file-record.h>
#include <ntfs-browser/mft/idx.h>
#include <ntfs-browser/namespace.h>
#include <ntfs-browser/ntfs-volume.h>
#include <ntfs-browser/volume-options.h>

#include "data/index-block.h"
#include "data/index-entry.h"
#include "fake-ntfs-image.h"
#include "memory-disk-reader.h"
#include "test-log-sink.h"

using Catch::Matchers::ContainsSubstring;
using NTFS_BROWSER_NAMESPACE::IndexEntryView;
using NTFS_BROWSER_NAMESPACE::NtfsVolume;
using NTFS_BROWSER_NAMESPACE::VolumeOptions;
namespace Cache = NTFS_BROWSER_NAMESPACE::Cache;
namespace Data = NTFS_BROWSER_NAMESPACE::Data;

namespace {

// Offset of the entry_offset field in an index block, which the entries follow.
constexpr size_t index_block_entry_offset_field =
    offsetof(Data::IndexBlock, entry_offset);

// Offset of an index entry's own size field.
constexpr size_t index_entry_size_field = offsetof(Data::IndexEntry, size);

// Offset of the first cluster-aligned index block ("INDX") in the image, the
// one $INDEX_ROOT points at.
size_t FirstIndexBlockOffset(const std::vector<BYTE>& image) {
  for (size_t offset = 0; offset + sizeof(DWORD) <= image.size();
       offset += NTFS_BROWSER_NAMESPACE::Tests::fake_cluster_size) {
    DWORD magic = 0;
    std::memcpy(&magic, &image.at(offset), sizeof(magic));
    if (magic == Data::index_block_magic) {
      return offset;
    }
  }
  return image.size();
}

// Names the root directory's walk reports, in callback order.
template <Cache::Strategy S>
std::vector<std::wstring> RootNames(std::vector<BYTE> image,
                                    const VolumeOptions& options) {
  const NtfsVolume<S> volume(
      std::make_unique<NTFS_BROWSER_NAMESPACE::Tests::MemoryDiskReader>(
          std::move(image)),
      options);
  REQUIRE(volume.IsVolumeOK());

  NTFS_BROWSER_NAMESPACE::Io::FileRecord<S> record(volume);
  REQUIRE(record.ParseFileRecord(
      static_cast<ULONGLONG>(NTFS_BROWSER_NAMESPACE::Mft::Idx::Root)));
  REQUIRE(record.ParseAttrs());

  std::vector<std::wstring> names;
  record.TraverseSubEntries(
      [](const IndexEntryView& index_entry, void* context) {
        static_cast<std::vector<std::wstring>*>(context)->emplace_back(
            index_entry.GetFilename());
      },
      &names);
  return names;
}

// Gives the block's first entry a size of zero, which no entry can have.
std::vector<BYTE> WithZeroSizedFirstEntry() {
  std::vector<BYTE> image = NTFS_BROWSER_NAMESPACE::Tests::
      BuildFakeNtfsImageWithOrphanedIndexBlocks();
  const size_t block = FirstIndexBlockOffset(image);
  REQUIRE(block < image.size());

  DWORD entry_offset = 0;
  std::memcpy(&entry_offset, &image.at(block + index_block_entry_offset_field),
              sizeof(entry_offset));
  const WORD zero = 0;
  std::memcpy(&image.at(block + index_block_entry_offset_field + entry_offset +
                        index_entry_size_field),
              &zero, sizeof(zero));
  return image;
}

}  // namespace

TEMPLATE_TEST_CASE_SIG(
    "An index block whose first entry has a zero size is rejected whole",
    "[attr-index-alloc]", ((Cache::Strategy S), S), Cache::Strategy::NoCache,
    Cache::Strategy::FullCache) {
  const std::vector<std::wstring> intact =
      RootNames<S>(NTFS_BROWSER_NAMESPACE::Tests::
                       BuildFakeNtfsImageWithOrphanedIndexBlocks(),
                   VolumeOptions{.recover_errors = true});
  REQUIRE(!intact.empty());

  (void)NTFS_BROWSER_NAMESPACE::Tests::TakeCapturedLog();
  const std::vector<std::wstring> strict =
      RootNames<S>(WithZeroSizedFirstEntry(), {});
  const std::string strict_log =
      NTFS_BROWSER_NAMESPACE::Tests::TakeCapturedLog();

  CHECK(strict.size() < intact.size());
  CHECK_THAT(strict_log, ContainsSubstring(
                             "Index Block: index entry exceeds block bounds"));
}

TEMPLATE_TEST_CASE_SIG(
    "A zero-sized index entry is logged as recoverable when recovery is on",
    "[attr-index-alloc]", ((Cache::Strategy S), S), Cache::Strategy::NoCache,
    Cache::Strategy::FullCache) {
  const std::vector<std::wstring> strict =
      RootNames<S>(WithZeroSizedFirstEntry(), {});

  (void)NTFS_BROWSER_NAMESPACE::Tests::TakeCapturedLog();
  const std::vector<std::wstring> recovered = RootNames<S>(
      WithZeroSizedFirstEntry(), VolumeOptions{.recover_errors = true});
  const std::string recovered_log =
      NTFS_BROWSER_NAMESPACE::Tests::TakeCapturedLog();

  // The block still yields nothing: its entries stop at the zero-sized one.
  // Only the VCN 1 orphan comes back, from the orphan scan.
  CHECK(strict.empty());
  CHECK(recovered ==
        std::vector<std::wstring>{std::wstring(
            NTFS_BROWSER_NAMESPACE::Tests::orphaned_block_orphan_name)});
  CHECK_THAT(
      recovered_log,
      ContainsSubstring("Index Block: index entry exceeds block bounds"));
}
