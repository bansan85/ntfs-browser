#include <ntfs-browser/win-types.h>

#include <cstddef>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <catch2/catch_template_test_macros.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <ntfs-browser/attr/type.h>
#include <ntfs-browser/cache/strategy.h>
#include <ntfs-browser/io/file-record.h>
#include <ntfs-browser/mft/idx.h>
#include <ntfs-browser/namespace.h>
#include <ntfs-browser/ntfs-volume.h>

#include "fake-ntfs-image.h"
#include "memory-disk-reader.h"
#include "test-log-sink.h"
#include "volume-tree-support.h"

using Catch::Matchers::ContainsSubstring;
using NTFS_BROWSER_NAMESPACE::NtfsVolume;
using NTFS_BROWSER_NAMESPACE::Io::FileRecord;
namespace Attr = NTFS_BROWSER_NAMESPACE::Attr;
namespace Cache = NTFS_BROWSER_NAMESPACE::Cache;
namespace Mft = NTFS_BROWSER_NAMESPACE::Mft;
using NTFS_BROWSER_NAMESPACE::Tests::MemoryDiskReader;

namespace {

// Offset of an $ATTRIBUTE_LIST entry's name_length BYTE.
constexpr size_t list_entry_name_length_field = 6;

// Offset of an $ATTRIBUTE_LIST entry's base file reference, a ULONGLONG.
constexpr size_t list_entry_base_ref_field = 16;

// Offset of a non-resident header's real_size, a ULONGLONG.
constexpr size_t attr_real_size_field = 48;

// Record number of $MFT.
constexpr ULONGLONG mft_idx = static_cast<ULONGLONG>(Mft::Idx::Mft);

// Records below Mft::Idx::User are read from a fixed address, so they count
// even when no extent maps them.
constexpr ULONGLONG fixed_address_records =
    static_cast<ULONGLONG>(Mft::Idx::User);

// $MFT's declared size: more records than its extents map.
constexpr ULONGLONG declared_mft_size =
    40 * static_cast<ULONGLONG>(
             NTFS_BROWSER_NAMESPACE::Tests::fake_file_record_size);

// Sets $MFT's base DATA real_size, which GetRecordsCount() caps at.
void SetMftRealSize(std::vector<BYTE>& image, ULONGLONG size) {
  Store<ULONGLONG>(
      image, FirstAttr(image, mft_idx, Attr::Type::Data) + attr_real_size_field,
      size);
}

// Parses record idx through volume and reports whether it could be read.
template <Cache::Strategy S>
bool CanReadRecord(const NtfsVolume<S>& volume, ULONGLONG idx) {
  FileRecord<S> record(volume);
  return record.ParseFileRecord(idx);
}

}  // namespace

TEMPLATE_TEST_CASE_SIG(
    "$MFT skips an $ATTRIBUTE_LIST entry that does not name a DATA "
    "continuation",
    "[characterization]", ((Cache::Strategy S), S), Cache::Strategy::NoCache,
    Cache::Strategy::FullCache) {
  SECTION("entry of another attribute type") {
    std::vector<BYTE> image = NTFS_BROWSER_NAMESPACE::Tests::
        BuildFakeNtfsImageWithMftDataSplitAcrossAttributeList();
    SetMftRealSize(image, declared_mft_size);
    const size_t entry = ResidentBody(
        image, FirstAttr(image, mft_idx, Attr::Type::AttributeList));
    Store<DWORD>(image, entry,
                 static_cast<DWORD>(Attr::Type::StandardInformation));
    const NtfsVolume<S> volume(
        std::make_unique<MemoryDiskReader>(std::move(image)));
    REQUIRE(volume.IsVolumeOK());
    CHECK(volume.GetRecordsCount() == fixed_address_records);
    CHECK_FALSE(CanReadRecord(
        volume, NTFS_BROWSER_NAMESPACE::Tests::mft_data_split_target_idx));
  }

  SECTION("named DATA entry") {
    std::vector<BYTE> image = NTFS_BROWSER_NAMESPACE::Tests::
        BuildFakeNtfsImageWithMftDataSplitAcrossAttributeList();
    SetMftRealSize(image, declared_mft_size);
    const size_t entry = ResidentBody(
        image, FirstAttr(image, mft_idx, Attr::Type::AttributeList));
    Store<BYTE>(image, entry + list_entry_name_length_field, 1);
    const NtfsVolume<S> volume(
        std::make_unique<MemoryDiskReader>(std::move(image)));
    REQUIRE(volume.IsVolumeOK());
    CHECK(volume.GetRecordsCount() == fixed_address_records);
    CHECK_FALSE(CanReadRecord(
        volume, NTFS_BROWSER_NAMESPACE::Tests::mft_data_split_target_idx));
  }

  SECTION("entry naming $MFT itself") {
    std::vector<BYTE> image = NTFS_BROWSER_NAMESPACE::Tests::
        BuildFakeNtfsImageWithMftDataSplitAcrossAttributeList();
    SetMftRealSize(image, declared_mft_size);
    const size_t entry = ResidentBody(
        image, FirstAttr(image, mft_idx, Attr::Type::AttributeList));
    // Record 0 with sequence 0 is $MFT's own file reference.
    Store<ULONGLONG>(image, entry + list_entry_base_ref_field, 0);
    const NtfsVolume<S> volume(
        std::make_unique<MemoryDiskReader>(std::move(image)));
    REQUIRE(volume.IsVolumeOK());
    CHECK(volume.GetRecordsCount() == fixed_address_records);
    CHECK_FALSE(CanReadRecord(
        volume, NTFS_BROWSER_NAMESPACE::Tests::mft_data_split_target_idx));
  }
}

TEMPLATE_TEST_CASE_SIG(
    "$MFT drops a DATA continuation whose record does not walk its attributes",
    "[characterization]", ((Cache::Strategy S), S), Cache::Strategy::NoCache,
    Cache::Strategy::FullCache) {
  std::vector<BYTE> image = NTFS_BROWSER_NAMESPACE::Tests::
      BuildFakeNtfsImageWithMftDataSplitAcrossAttributeList();
  SetMftRealSize(image, declared_mft_size);
  // A DATA header claiming more bytes than the record holds: the walk runs
  // out before an end marker.
  Store<DWORD>(
      image,
      FirstAttr(image, NTFS_BROWSER_NAMESPACE::Tests::mft_data_split_ext_idx,
                Attr::Type::Data) +
          attr_total_size_field,
      static_cast<DWORD>(NTFS_BROWSER_NAMESPACE::Tests::fake_file_record_size));
  (void)NTFS_BROWSER_NAMESPACE::Tests::TakeCapturedLog();
  const NtfsVolume<S> volume(
      std::make_unique<MemoryDiskReader>(std::move(image)));
  const std::string captured = NTFS_BROWSER_NAMESPACE::Tests::TakeCapturedLog();

  REQUIRE(volume.IsVolumeOK());
  CHECK(volume.GetRecordsCount() == fixed_address_records);
  CHECK_FALSE(CanReadRecord(
      volume, NTFS_BROWSER_NAMESPACE::Tests::mft_data_split_target_idx));
  CHECK_THAT(captured,
             ContainsSubstring(
                 "$MFT DATA continuation in record " +
                 std::to_string(
                     NTFS_BROWSER_NAMESPACE::Tests::mft_data_split_ext_idx) +
                 " could not be resolved"));
}
