#include <ntfs-browser/win-types.h>

#include <cstddef>
#include <memory>
#include <utility>
#include <vector>

#include <catch2/catch_template_test_macros.hpp>
#include <catch2/catch_test_macros.hpp>

#include <ntfs-browser/attr/defines.h>
#include <ntfs-browser/attr/mask.h>
#include <ntfs-browser/attr/type.h>
#include <ntfs-browser/cache/strategy.h>
#include <ntfs-browser/io/file-record.h>
#include <ntfs-browser/mft/idx.h>
#include <ntfs-browser/ntfs-volume.h>

#include "fake-ntfs-image.h"
#include "memory-disk-reader.h"
#include "volume-tree-support.h"

using NtfsBrowser::NtfsVolume;
using NtfsBrowser::Io::FileRecord;
namespace Attr = NtfsBrowser::Attr;
namespace Cache = NtfsBrowser::Cache;
namespace Mft = NtfsBrowser::Mft;
using NtfsBrowserTests::MemoryDiskReader;

namespace {

// Offset of an attribute header's name_length BYTE.
constexpr size_t attr_name_length_field = 9;

// Offset of a non-resident header's lowest VCN.
constexpr size_t attr_lowest_vcn_field = 16;

// Offset of a non-resident header's highest VCN.
constexpr size_t attr_highest_vcn_field = 24;

// Offset of a resident attribute's body length, a DWORD.
constexpr size_t attr_body_size_field = 16;

// Offset of an attribute list entry's record length, a WORD.
constexpr size_t list_entry_record_size_field = 4;

// Offset of the major version in $VOLUME_INFORMATION, after 8 reserved bytes.
constexpr size_t volume_info_major_field = 8;

// Boot sector offsets of the BPB fields the size tests patch.
constexpr size_t bpb_bytes_per_sector_field = 0x0B;

constexpr size_t bpb_sectors_per_cluster_field = 0x0D;

constexpr size_t bpb_clusters_per_file_record_field = 0x40;

// Offset of the "NTFS" OEM id that the boot sector signature check reads.
constexpr size_t bpb_oem_id_field = 3;

// A type no attribute list accepts, so the scan stops at its entry.
constexpr DWORD invalid_attr_type = 0x1234;

// Lowest VCN the mismatched extension extent is moved to, unlisted by
// $ATTRIBUTE_LIST.
constexpr ULONGLONG unlisted_start_vcn = 17;

// Lowest VCN of the second extent in the two-extents fixture, moved onto the
// first.
constexpr ULONGLONG overlapping_start_vcn = 16;

// Major version an NT4-era volume reports, below the NTFS 3.0 floor.
constexpr BYTE old_major_version = 2;

// Record number of $Volume.
constexpr ULONGLONG volume_idx = static_cast<ULONGLONG>(Mft::Idx::Volume);

// Record number of $MFT.
constexpr ULONGLONG mft_idx = static_cast<ULONGLONG>(Mft::Idx::Mft);

// Raw attribute callbacks that fired since the test last reset the count.
size_t raw_attr_calls = 0;

void CountRawAttr(const Attr::HeaderCommon& /*header*/, bool& /*discard*/) {
  ++raw_attr_calls;
}

// Parses $Volume's VolumeInformation attribute through volume.
template <Cache::Strategy S>
void ParseVolumeInformation(const NtfsVolume<S>& volume) {
  NtfsBrowser::Io::FileRecord<S> volume_record(volume);
  volume_record.SetAttrMask(Attr::Mask::VolumeInformation);
  REQUIRE(volume_record.ParseFileRecord(volume_idx));
  REQUIRE(volume_record.ParseAttrs());
}

}  // namespace

TEMPLATE_TEST_CASE_SIG(
    "NtfsVolume rejects a boot sector without the NTFS signature", "[cov-vol]",
    ((Cache::Strategy S), S), Cache::Strategy::NoCache,
    Cache::Strategy::FullCache) {
  std::vector<BYTE> image = NtfsBrowserTests::BuildFakeNtfsImage();
  Store<BYTE>(image, bpb_oem_id_field, 'X');

  const NtfsVolume<S> volume(
      std::make_unique<MemoryDiskReader>(std::move(image)));

  CHECK_FALSE(volume.IsVolumeOK());
}

TEMPLATE_TEST_CASE_SIG(
    "NtfsVolume rejects a boot sector whose size fields cannot be decoded",
    "[cov-vol]", ((Cache::Strategy S), S), Cache::Strategy::NoCache,
    Cache::Strategy::FullCache) {
  SECTION("sector size below one WORD") {
    std::vector<BYTE> image = NtfsBrowserTests::BuildFakeNtfsImage();
    Store<WORD>(image, bpb_bytes_per_sector_field, 1);
    const NtfsVolume<S> volume(
        std::make_unique<MemoryDiskReader>(std::move(image)));
    CHECK_FALSE(volume.IsVolumeOK());
  }

  SECTION("sectors per cluster magnitude beyond 2^12") {
    std::vector<BYTE> image = NtfsBrowserTests::BuildFakeNtfsImage();
    Store<BYTE>(image, bpb_sectors_per_cluster_field, 0x80);
    const NtfsVolume<S> volume(
        std::make_unique<MemoryDiskReader>(std::move(image)));
    CHECK_FALSE(volume.IsVolumeOK());
  }

  SECTION("file record size below the header and not whole sectors") {
    std::vector<BYTE> image = NtfsBrowserTests::BuildFakeNtfsImage();
    // A -1 exponent in this field means 2^1 bytes.
    Store<BYTE>(image, bpb_clusters_per_file_record_field, 0xFF);
    const NtfsVolume<S> volume(
        std::make_unique<MemoryDiskReader>(std::move(image)));
    CHECK_FALSE(volume.IsVolumeOK());
  }
}

TEMPLATE_TEST_CASE_SIG(
    "NtfsVolume reads a negative sectors-per-cluster as a power of two",
    "[cov-vol]", ((Cache::Strategy S), S), Cache::Strategy::NoCache,
    Cache::Strategy::FullCache) {
  std::vector<BYTE> image = NtfsBrowserTests::BuildFakeNtfsImage();
  // A -1 exponent doubles the 512-byte sector: the same 1 KiB cluster.
  Store<BYTE>(image, bpb_sectors_per_cluster_field, 0xFF);

  const NtfsVolume<S> volume(
      std::make_unique<MemoryDiskReader>(std::move(image)));

  REQUIRE(volume.IsVolumeOK());
  CHECK(volume.GetClusterSize() == NtfsBrowserTests::fake_cluster_size);
}

TEMPLATE_TEST_CASE_SIG("NtfsVolume rejects a damaged $Volume record",
                       "[cov-vol]", ((Cache::Strategy S), S),
                       Cache::Strategy::NoCache, Cache::Strategy::FullCache) {
  SECTION("$Volume record does not parse") {
    std::vector<BYTE> image = NtfsBrowserTests::BuildFakeNtfsImage();
    ZeroRecord(image, volume_idx);
    const NtfsVolume<S> volume(
        std::make_unique<MemoryDiskReader>(std::move(image)));
    CHECK_FALSE(volume.IsVolumeOK());
  }

  SECTION("$Volume has no VolumeInformation attribute") {
    std::vector<BYTE> image = NtfsBrowserTests::BuildFakeNtfsImage();
    Store<DWORD>(image,
                 RecordOffset(volume_idx) +
                     Load<WORD>(image, RecordOffset(volume_idx) +
                                           record_first_attr_field),
                 end_of_attrs_marker);
    const NtfsVolume<S> volume(
        std::make_unique<MemoryDiskReader>(std::move(image)));
    CHECK_FALSE(volume.IsVolumeOK());
  }

  SECTION("volume major version below NTFS 3.0") {
    std::vector<BYTE> image = NtfsBrowserTests::BuildFakeNtfsImage();
    const size_t attr =
        FirstAttr(image, volume_idx, Attr::Type::VolumeInformation);
    Store<BYTE>(image, ResidentBody(image, attr) + volume_info_major_field,
                old_major_version);
    const NtfsVolume<S> volume(
        std::make_unique<MemoryDiskReader>(std::move(image)));
    CHECK_FALSE(volume.IsVolumeOK());
    CHECK(volume.GetVersion().first == old_major_version);
  }
}

TEMPLATE_TEST_CASE_SIG(
    "NtfsVolume rejects a $MFT without an unnamed non-resident DATA",
    "[cov-vol]", ((Cache::Strategy S), S), Cache::Strategy::NoCache,
    Cache::Strategy::FullCache) {
  std::vector<BYTE> image = NtfsBrowserTests::BuildFakeNtfsImage();
  Store<BYTE>(
      image,
      FirstAttr(image, mft_idx, Attr::Type::Data) + attr_name_length_field, 1);

  const NtfsVolume<S> volume(
      std::make_unique<MemoryDiskReader>(std::move(image)));

  CHECK_FALSE(volume.IsVolumeOK());
}

TEMPLATE_TEST_CASE_SIG(
    "NtfsVolume drops a damaged $MFT continuation and keeps the volume",
    "[cov-vol]", ((Cache::Strategy S), S), Cache::Strategy::NoCache,
    Cache::Strategy::FullCache) {
  SECTION("attribute list entry of an unknown type stops the scan") {
    std::vector<BYTE> image = NtfsBrowserTests::
        BuildFakeNtfsImageWithMftDataSplitAcrossAttributeList();
    const size_t entry = ResidentBody(
        image, FirstAttr(image, mft_idx, Attr::Type::AttributeList));
    Store<DWORD>(image, entry, invalid_attr_type);
    const NtfsVolume<S> volume(
        std::make_unique<MemoryDiskReader>(std::move(image)));
    CHECK(volume.IsVolumeOK());
  }

  SECTION("attribute list entry with a zero record length stops the scan") {
    std::vector<BYTE> image = NtfsBrowserTests::
        BuildFakeNtfsImageWithMftDataSplitAcrossAttributeList();
    const size_t entry = ResidentBody(
        image, FirstAttr(image, mft_idx, Attr::Type::AttributeList));
    Store<WORD>(image, entry + list_entry_record_size_field, 0);
    const NtfsVolume<S> volume(
        std::make_unique<MemoryDiskReader>(std::move(image)));
    CHECK(volume.IsVolumeOK());
  }

  SECTION("extension record that does not parse is dropped") {
    std::vector<BYTE> image = NtfsBrowserTests::
        BuildFakeNtfsImageWithMftDataSplitAcrossAttributeList();
    ZeroRecord(image, NtfsBrowserTests::mft_data_split_ext_idx);
    const NtfsVolume<S> volume(
        std::make_unique<MemoryDiskReader>(std::move(image)));
    CHECK(volume.IsVolumeOK());
  }

  SECTION("named extension DATA is rejected") {
    std::vector<BYTE> image = NtfsBrowserTests::
        BuildFakeNtfsImageWithMftDataSplitAcrossAttributeList();
    Store<BYTE>(image,
                FirstAttr(image, NtfsBrowserTests::mft_data_split_ext_idx,
                          Attr::Type::Data) +
                    attr_name_length_field,
                1);
    const NtfsVolume<S> volume(
        std::make_unique<MemoryDiskReader>(std::move(image)));
    CHECK(volume.IsVolumeOK());
  }

  SECTION("extension DATA with an inverted VCN range is rejected") {
    std::vector<BYTE> image = NtfsBrowserTests::
        BuildFakeNtfsImageWithMftDataSplitAcrossAttributeList();
    const size_t attr = FirstAttr(
        image, NtfsBrowserTests::mft_data_split_ext_idx, Attr::Type::Data);
    const auto lowest = Load<ULONGLONG>(image, attr + attr_lowest_vcn_field);
    REQUIRE(lowest > 0);
    Store<ULONGLONG>(image, attr + attr_highest_vcn_field, lowest - 1);
    const NtfsVolume<S> volume(
        std::make_unique<MemoryDiskReader>(std::move(image)));
    CHECK(volume.IsVolumeOK());
  }

  SECTION("$MFT attribute list body larger than its record") {
    std::vector<BYTE> image = NtfsBrowserTests::
        BuildFakeNtfsImageWithMftDataSplitAcrossAttributeList();
    Store<DWORD>(image,
                 FirstAttr(image, mft_idx, Attr::Type::AttributeList) +
                     attr_body_size_field,
                 0x10000U);
    const NtfsVolume<S> volume(
        std::make_unique<MemoryDiskReader>(std::move(image)));
    static_cast<void>(volume.IsVolumeOK());
  }

  SECTION("extension DATA at a VCN its list entry does not name") {
    std::vector<BYTE> image = NtfsBrowserTests::
        BuildFakeNtfsImageWithMftDataSplitAcrossAttributeList();
    const size_t attr = FirstAttr(
        image, NtfsBrowserTests::mft_data_split_ext_idx, Attr::Type::Data);
    Store<ULONGLONG>(image, attr + attr_lowest_vcn_field, unlisted_start_vcn);
    Store<ULONGLONG>(image, attr + attr_highest_vcn_field, unlisted_start_vcn);
    const NtfsVolume<S> volume(
        std::make_unique<MemoryDiskReader>(std::move(image)));
    CHECK(volume.IsVolumeOK());
  }
}

TEMPLATE_TEST_CASE_SIG(
    "NtfsVolume rejects a $MFT continuation that overlaps an accepted extent",
    "[cov-vol]", ((Cache::Strategy S), S), Cache::Strategy::NoCache,
    Cache::Strategy::FullCache) {
  std::vector<BYTE> image =
      NtfsBrowserTests::BuildFakeNtfsImageWithMftDataTwoExtentsInOneRecord();
  const std::vector<size_t> extents = AttrOffsets(
      image, NtfsBrowserTests::mft_two_extents_ext_idx, Attr::Type::Data);
  REQUIRE(extents.size() == 2);
  // The second extent now maps the same VCN the first one maps.
  Store<ULONGLONG>(image, extents[1] + attr_lowest_vcn_field,
                   overlapping_start_vcn);
  Store<ULONGLONG>(image, extents[1] + attr_highest_vcn_field,
                   overlapping_start_vcn);

  const NtfsVolume<S> volume(
      std::make_unique<MemoryDiskReader>(std::move(image)));

  CHECK(volume.IsVolumeOK());
}

TEST_CASE("NtfsVolume refuses a $MFT read that fails on disk", "[cov-vol]") {
  auto refused = std::make_shared<size_t>(0);
  // VCN 16 of the forged $MFT run sits at LCN 20 + 16.
  const auto lcn_begin =
      (static_cast<ULONGLONG>(NtfsBrowserTests::fragmented_mft_data_run_lcn) +
       NtfsBrowserTests::fragmented_mft_invalid_record_idx) *
      NtfsBrowserTests::fake_cluster_size;
  const auto lcn_end = lcn_begin + NtfsBrowserTests::fake_cluster_size;

  const NtfsVolume<Cache::Strategy::NoCache> volume(
      std::make_unique<RefusingDiskReader>(
          NtfsBrowserTests::BuildFakeNtfsImageWithFragmentedMftInvalidRecord(),
          lcn_begin, lcn_end, refused));
  REQUIRE(volume.IsVolumeOK());

  NtfsBrowser::Io::FileRecord<Cache::Strategy::NoCache> record(volume);
  CHECK_FALSE(record.ParseFileRecord(
      NtfsBrowserTests::fragmented_mft_invalid_record_idx));
  CHECK(*refused > 0);
}

TEMPLATE_TEST_CASE_SIG("NtfsVolume reports its boot sector geometry",
                       "[cov-vol]", ((Cache::Strategy S), S),
                       Cache::Strategy::NoCache, Cache::Strategy::FullCache) {
  const NtfsVolume<S> volume(std::make_unique<MemoryDiskReader>(
      NtfsBrowserTests::BuildFakeNtfsImage()));
  REQUIRE(volume.IsVolumeOK());

  CHECK(volume.GetSectorSize() == NtfsBrowserTests::fake_bytes_per_sector);
  CHECK(volume.GetClusterSize() == NtfsBrowserTests::fake_cluster_size);
  CHECK(volume.GetFileRecordSize() == NtfsBrowserTests::fake_file_record_size);
  CHECK(volume.GetIndexBlockSize() == NtfsBrowserTests::fake_cluster_size);
  CHECK(volume.GetMFTAddr() == mft_base_offset);
}

TEMPLATE_TEST_CASE_SIG("NtfsVolume installs and clears raw attribute callbacks",
                       "[cov-vol]", ((Cache::Strategy S), S),
                       Cache::Strategy::NoCache, Cache::Strategy::FullCache) {
  NtfsVolume<S> volume(std::make_unique<MemoryDiskReader>(
      NtfsBrowserTests::BuildFakeNtfsImage()));
  REQUIRE(volume.IsVolumeOK());

  SECTION("an attribute type outside the table is refused") {
    CHECK_FALSE(volume.InstallAttrRawCB(
        static_cast<Attr::Type>(invalid_attr_type), &CountRawAttr));
  }

  SECTION("a callback fires on parse until cleared") {
    raw_attr_calls = 0;
    REQUIRE(
        volume.InstallAttrRawCB(Attr::Type::VolumeInformation, &CountRawAttr));
    ParseVolumeInformation(volume);
    CHECK(raw_attr_calls > 0);

    volume.ClearAttrRawCB();
    raw_attr_calls = 0;
    ParseVolumeInformation(volume);
    CHECK(raw_attr_calls == 0);
  }
}

TEMPLATE_TEST_CASE_SIG("NtfsVolume reads a zero-length span", "[cov-vol]",
                       ((Cache::Strategy S), S), Cache::Strategy::NoCache,
                       Cache::Strategy::FullCache) {
  const NtfsVolume<S> volume(std::make_unique<MemoryDiskReader>(
      NtfsBrowserTests::BuildFakeNtfsImage()));
  REQUIRE(volume.IsVolumeOK());

  LARGE_INTEGER addr{.QuadPart = 0};
  const auto span = volume.Read(addr, 0);

  REQUIRE(span.has_value());
  CHECK(span->empty());
}

TEMPLATE_TEST_CASE_SIG("NtfsVolume has no EFS key provider off Windows",
                       "[cov-vol]", ((Cache::Strategy S), S),
                       Cache::Strategy::NoCache, Cache::Strategy::FullCache) {
  const NtfsVolume<S> volume(std::make_unique<MemoryDiskReader>(
      NtfsBrowserTests::BuildFakeNtfsImage()));
  REQUIRE(volume.IsVolumeOK());

#ifndef _WIN32
  // Off Windows no certificate store exists, so no provider is created.
  CHECK(volume.GetEfsKeyProvider() == nullptr);
#endif
}
