#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <catch2/catch_template_test_macros.hpp>
#include <catch2/catch_test_macros.hpp>

#include <ntfs-browser/cache/strategy.h>
#include <ntfs-browser/index-entry.h>
#include <ntfs-browser/io/file-record.h>
#include <ntfs-browser/namespace.h>
#include <ntfs-browser/ntfs-volume.h>
#include <ntfs-browser/volume-options.h>

#include "fake-ntfs-image.h"
#include "memory-disk-reader.h"
#include "optional-access.h"

using NTFS_BROWSER_NAMESPACE::IndexEntry;
using NTFS_BROWSER_NAMESPACE::NtfsVolume;
using NTFS_BROWSER_NAMESPACE::VolumeOptions;
using NTFS_BROWSER_NAMESPACE::Io::FileRecord;
namespace Cache = NTFS_BROWSER_NAMESPACE::Cache;

namespace {

// A FindSubEntry() result from $INDEX_ROOT must stay valid independent of
// the FileRecord it came from, even after that object is reparsed in place.
template <Cache::Strategy S>
void RunFindSubEntryOutlivesReparseTest() {
  auto reader = std::make_unique<
      NTFS_BROWSER_NAMESPACE::Tests::MemoryDiskReader>(
      NTFS_BROWSER_NAMESPACE::Tests::BuildFakeNtfsImageWithIndexRootVariants());

  // The named records are not built, so only recover_errors shows the entries.
  const NtfsVolume<S> volume(std::move(reader),
                             VolumeOptions{.recover_errors = true});
  REQUIRE(volume.IsVolumeOK());

  NTFS_BROWSER_NAMESPACE::Io::FileRecord<S> record(volume);
  REQUIRE(record.ParseFileRecord(
      NTFS_BROWSER_NAMESPACE::Tests::index_root_variant_a_dir_idx));
  REQUIRE(record.ParseAttrs());

  std::optional<IndexEntry> saved_entry = record.FindSubEntry(
      NTFS_BROWSER_NAMESPACE::Tests::index_root_variant_a_name);
  REQUIRE(saved_entry.has_value());
  CHECK(NTFS_BROWSER_NAMESPACE::Tests::Unwrap(saved_entry).GetFileReference() ==
        NTFS_BROWSER_NAMESPACE::Tests::index_root_variant_a_mft_ref);
  CHECK(NTFS_BROWSER_NAMESPACE::Tests::Unwrap(saved_entry).GetFilename() ==
        NTFS_BROWSER_NAMESPACE::Tests::index_root_variant_a_name);

  // Reparse the SAME FileRecord object for variant B's record - same fixed
  // size (fake_file_record_size), so record_buffer_ is reused/overwritten in
  // place, not reallocated.
  REQUIRE(record.ParseFileRecord(
      NTFS_BROWSER_NAMESPACE::Tests::index_root_variant_b_dir_idx));
  REQUIRE(record.ParseAttrs());

  // The entry saved from variant A must be entirely unaffected by parsing a
  // second, different record on the same FileRecord object.
  CHECK(NTFS_BROWSER_NAMESPACE::Tests::Unwrap(saved_entry).GetFileReference() ==
        NTFS_BROWSER_NAMESPACE::Tests::index_root_variant_a_mft_ref);
  CHECK(NTFS_BROWSER_NAMESPACE::Tests::Unwrap(saved_entry).GetFilename() ==
        NTFS_BROWSER_NAMESPACE::Tests::index_root_variant_a_name);
}

}  // namespace

TEMPLATE_TEST_CASE_SIG(
    "FindSubEntry's IndexEntry from $INDEX_ROOT stays correct across a "
    "reparse",
    "[index-entry][regression]", ((Cache::Strategy S), S),
    Cache::Strategy::NoCache, Cache::Strategy::FullCache) {
  RunFindSubEntryOutlivesReparseTest<S>();
}
