#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <catch2/catch_template_test_macros.hpp>
#include <catch2/catch_test_macros.hpp>

#include <ntfs-browser/file-record.h>
#include <ntfs-browser/index-entry.h>
#include <ntfs-browser/ntfs-volume.h>
#include <ntfs-browser/strategy.h>

#include "fake-ntfs-image.h"
#include "memory-disk-reader.h"
#include "optional-access.h"

using NtfsBrowser::FileRecord;
using NtfsBrowser::IndexEntry;
using NtfsBrowser::NtfsVolume;
using NtfsBrowser::Strategy;

namespace
{

// A FindSubEntry() result from $INDEX_ROOT must stay valid independent of
// the FileRecord it came from, even after that object is reparsed in place.
template <Strategy S>
void RunFindSubEntryOutlivesReparseTest()
{
  auto reader = std::make_unique<NtfsBrowserTests::MemoryDiskReader>(
      NtfsBrowserTests::BuildFakeNtfsImageWithIndexRootVariants());

  NtfsVolume<S> const volume(std::move(reader));
  REQUIRE(volume.IsVolumeOK());

  FileRecord<S> record(volume);
  REQUIRE(
      record.ParseFileRecord(NtfsBrowserTests::index_root_variant_a_dir_idx));
  REQUIRE(record.ParseAttrs());

  std::optional<IndexEntry> saved_entry =
      record.FindSubEntry(NtfsBrowserTests::index_root_variant_a_name);
  REQUIRE(saved_entry.has_value());
  CHECK(NtfsBrowserTests::Unwrap(saved_entry).GetFileReference() ==
        NtfsBrowserTests::index_root_variant_a_mft_ref);
  CHECK(NtfsBrowserTests::Unwrap(saved_entry).GetFilename() ==
        NtfsBrowserTests::index_root_variant_a_name);

  // Reparse the SAME FileRecord object for variant B's record - same fixed
  // size (fake_file_record_size), so record_buffer_ is reused/overwritten in
  // place, not reallocated.
  REQUIRE(
      record.ParseFileRecord(NtfsBrowserTests::index_root_variant_b_dir_idx));
  REQUIRE(record.ParseAttrs());

  // The entry saved from variant A must be entirely unaffected by parsing a
  // second, different record on the same FileRecord object.
  CHECK(NtfsBrowserTests::Unwrap(saved_entry).GetFileReference() ==
        NtfsBrowserTests::index_root_variant_a_mft_ref);
  CHECK(NtfsBrowserTests::Unwrap(saved_entry).GetFilename() ==
        NtfsBrowserTests::index_root_variant_a_name);
}

}  // namespace

TEMPLATE_TEST_CASE_SIG(
    "FindSubEntry's IndexEntry from $INDEX_ROOT stays correct across a "
    "reparse",
    "[index-entry][regression]", ((Strategy S), S), Strategy::NoCache,
    Strategy::FullCache)
{
  RunFindSubEntryOutlivesReparseTest<S>();
}
