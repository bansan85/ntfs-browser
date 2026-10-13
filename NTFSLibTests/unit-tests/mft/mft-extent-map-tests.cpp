#include <ntfs-browser/win-types.h>

#include <algorithm>
#include <cstddef>
#include <memory>
#include <utility>
#include <vector>

#include <catch2/catch_template_test_macros.hpp>
#include <catch2/catch_test_macros.hpp>

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

// Offset of a non-resident header's real_size, a ULONGLONG.
constexpr size_t attr_real_size_field = 48;

// Start VCN no $ATTRIBUTE_LIST entry of the split fixture names.
constexpr ULONGLONG unlisted_start_vcn = 17;

// Lowest VCN of the second extent in the two-extents fixture, moved onto the
// first.
constexpr ULONGLONG overlapping_start_vcn = 16;

// Record number of $MFT.
constexpr ULONGLONG mft_idx = static_cast<ULONGLONG>(Mft::Idx::Mft);

// Records below Mft::Idx::User are read from a fixed address, so they count
// even when no extent maps them.
constexpr ULONGLONG fixed_address_records =
    static_cast<ULONGLONG>(Mft::Idx::User);

// $MFT's declared size: more records than its extents map.
constexpr ULONGLONG declared_mft_size =
    40 * NtfsBrowserTests::fake_file_record_size;

// Records the split fixture maps: the base extent's VCN 0 and the
// continuation's VCN mft_data_split_target_idx.
constexpr ULONGLONG split_mapped_records =
    NtfsBrowserTests::mft_data_split_target_idx + 1;

// Records the two-extents fixture maps: its second extent reaches VCN 32.
constexpr ULONGLONG two_extents_mapped_records =
    NtfsBrowserTests::mft_two_extents_second_vcn + 1;

// Declared record count below the two-extents fixture's mapped range.
constexpr ULONGLONG truncated_records = 20;

// Record the chain fixture's last continuation reaches, past every naively
// reachable extent.
constexpr ULONGLONG chain_target_idx =
    NtfsBrowserTests::mft_chain_ext_a_start_vcn;

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

// Zeroes the one cluster at lcn.
void ZeroCluster(std::vector<BYTE>& image, DWORD lcn) {
  const auto begin = static_cast<std::ptrdiff_t>(
      static_cast<size_t>(lcn) * NtfsBrowserTests::fake_cluster_size);
  std::fill_n(image.begin() + begin, NtfsBrowserTests::fake_cluster_size,
              BYTE{0});
}

}  // namespace

TEMPLATE_TEST_CASE_SIG("$MFT counts only the records its extents map",
                       "[mft-extent-map]", ((Cache::Strategy S), S),
                       Cache::Strategy::NoCache, Cache::Strategy::FullCache) {
  SECTION("base extent alone, declared size inside it") {
    const NtfsVolume<S> volume(std::make_unique<MemoryDiskReader>(
        NtfsBrowserTests::BuildFakeNtfsImage()));
    REQUIRE(volume.IsVolumeOK());
    CHECK(volume.GetRecordsCount() == NtfsBrowserTests::sentinel_record_count);
  }

  SECTION("base extent alone, declared size past the fixed records") {
    std::vector<BYTE> image = NtfsBrowserTests::BuildFakeNtfsImage();
    SetMftRealSize(image, declared_mft_size);
    const NtfsVolume<S> volume(
        std::make_unique<MemoryDiskReader>(std::move(image)));
    REQUIRE(volume.IsVolumeOK());
    CHECK(volume.GetRecordsCount() == fixed_address_records);
  }

  SECTION("split DATA counts through its continuation") {
    std::vector<BYTE> image = NtfsBrowserTests::
        BuildFakeNtfsImageWithMftDataSplitAcrossAttributeList();
    SetMftRealSize(image, declared_mft_size);
    const NtfsVolume<S> volume(
        std::make_unique<MemoryDiskReader>(std::move(image)));
    REQUIRE(volume.IsVolumeOK());
    CHECK(volume.GetRecordsCount() == split_mapped_records);
    CHECK(CanReadRecord(volume, NtfsBrowserTests::mft_data_split_target_idx));
  }

  SECTION("two extents in one record count through the last one") {
    std::vector<BYTE> image =
        NtfsBrowserTests::BuildFakeNtfsImageWithMftDataTwoExtentsInOneRecord();
    SetMftRealSize(image, declared_mft_size);
    const NtfsVolume<S> volume(
        std::make_unique<MemoryDiskReader>(std::move(image)));
    REQUIRE(volume.IsVolumeOK());
    CHECK(volume.GetRecordsCount() == two_extents_mapped_records);
    CHECK(CanReadRecord(volume, NtfsBrowserTests::mft_two_extents_first_vcn));
    CHECK(CanReadRecord(volume, NtfsBrowserTests::mft_two_extents_second_vcn));
  }

  SECTION("a declared size below the mapped range caps the count") {
    std::vector<BYTE> image =
        NtfsBrowserTests::BuildFakeNtfsImageWithMftDataTwoExtentsInOneRecord();
    SetMftRealSize(image,
                   truncated_records * NtfsBrowserTests::fake_file_record_size);
    const NtfsVolume<S> volume(
        std::make_unique<MemoryDiskReader>(std::move(image)));
    REQUIRE(volume.IsVolumeOK());
    CHECK(volume.GetRecordsCount() == truncated_records);
  }

  SECTION("a record past every extent does not parse") {
    std::vector<BYTE> image = NtfsBrowserTests::
        BuildFakeNtfsImageWithMftDataSplitAcrossAttributeList();
    SetMftRealSize(image, declared_mft_size);
    const NtfsVolume<S> volume(
        std::make_unique<MemoryDiskReader>(std::move(image)));
    REQUIRE(volume.IsVolumeOK());
    CHECK_FALSE(CanReadRecord(volume, split_mapped_records));
  }
}

TEMPLATE_TEST_CASE_SIG("$MFT rejects a DATA continuation it cannot map",
                       "[mft-extent-map]", ((Cache::Strategy S), S),
                       Cache::Strategy::NoCache, Cache::Strategy::FullCache) {
  SECTION("named continuation") {
    std::vector<BYTE> image = NtfsBrowserTests::
        BuildFakeNtfsImageWithMftDataSplitAcrossAttributeList();
    SetMftRealSize(image, declared_mft_size);
    Store<BYTE>(image,
                FirstAttr(image, NtfsBrowserTests::mft_data_split_ext_idx,
                          Attr::Type::Data) +
                    attr_name_length_field,
                1);
    const NtfsVolume<S> volume(
        std::make_unique<MemoryDiskReader>(std::move(image)));
    REQUIRE(volume.IsVolumeOK());
    CHECK(volume.GetRecordsCount() == fixed_address_records);
    CHECK_FALSE(
        CanReadRecord(volume, NtfsBrowserTests::mft_data_split_target_idx));
  }

  SECTION("inverted VCN range") {
    std::vector<BYTE> image = NtfsBrowserTests::
        BuildFakeNtfsImageWithMftDataSplitAcrossAttributeList();
    SetMftRealSize(image, declared_mft_size);
    const size_t attr = FirstAttr(
        image, NtfsBrowserTests::mft_data_split_ext_idx, Attr::Type::Data);
    const auto lowest = Load<ULONGLONG>(image, attr + attr_lowest_vcn_field);
    Store<ULONGLONG>(image, attr + attr_highest_vcn_field, lowest - 1);
    const NtfsVolume<S> volume(
        std::make_unique<MemoryDiskReader>(std::move(image)));
    REQUIRE(volume.IsVolumeOK());
    CHECK(volume.GetRecordsCount() == fixed_address_records);
    CHECK_FALSE(
        CanReadRecord(volume, NtfsBrowserTests::mft_data_split_target_idx));
  }

  SECTION("start VCN its $ATTRIBUTE_LIST entry does not name") {
    std::vector<BYTE> image = NtfsBrowserTests::
        BuildFakeNtfsImageWithMftDataSplitAcrossAttributeList();
    SetMftRealSize(image, declared_mft_size);
    const size_t attr = FirstAttr(
        image, NtfsBrowserTests::mft_data_split_ext_idx, Attr::Type::Data);
    Store<ULONGLONG>(image, attr + attr_lowest_vcn_field, unlisted_start_vcn);
    Store<ULONGLONG>(image, attr + attr_highest_vcn_field, unlisted_start_vcn);
    const NtfsVolume<S> volume(
        std::make_unique<MemoryDiskReader>(std::move(image)));
    REQUIRE(volume.IsVolumeOK());
    CHECK(volume.GetRecordsCount() == fixed_address_records);
    CHECK_FALSE(
        CanReadRecord(volume, NtfsBrowserTests::mft_data_split_target_idx));
  }

  SECTION("base last VCN that overflows a byte offset") {
    const NtfsVolume<S> volume(std::make_unique<MemoryDiskReader>(
        NtfsBrowserTests::BuildFakeNtfsImageWithMftDataLastVcnOverflow()));
    REQUIRE(volume.IsVolumeOK());
    CHECK(volume.GetRecordsCount() == 1);
    CHECK_FALSE(CanReadRecord(
        volume, NtfsBrowserTests::mft_last_vcn_overflow_target_idx));
  }

  SECTION("continuation overlapping the extent before it") {
    std::vector<BYTE> image =
        NtfsBrowserTests::BuildFakeNtfsImageWithMftDataTwoExtentsInOneRecord();
    SetMftRealSize(image, declared_mft_size);
    const std::vector<size_t> extents = AttrOffsets(
        image, NtfsBrowserTests::mft_two_extents_ext_idx, Attr::Type::Data);
    REQUIRE(extents.size() == 2);
    Store<ULONGLONG>(image, extents[1] + attr_lowest_vcn_field,
                     overlapping_start_vcn);
    Store<ULONGLONG>(image, extents[1] + attr_highest_vcn_field,
                     overlapping_start_vcn);
    // If the overlapping extent were accepted, VCN 16 would read this cluster.
    ZeroCluster(image, NtfsBrowserTests::mft_two_extents_second_lcn);
    const NtfsVolume<S> volume(
        std::make_unique<MemoryDiskReader>(std::move(image)));
    REQUIRE(volume.IsVolumeOK());
    CHECK(CanReadRecord(volume, NtfsBrowserTests::mft_two_extents_first_vcn));
  }

  SECTION("continuation overlapping the extent after it") {
    std::vector<BYTE> image =
        NtfsBrowserTests::BuildFakeNtfsImageWithMftDataTwoExtentsInOneRecord();
    SetMftRealSize(image, declared_mft_size);
    const std::vector<size_t> extents = AttrOffsets(
        image, NtfsBrowserTests::mft_two_extents_ext_idx, Attr::Type::Data);
    REQUIRE(extents.size() == 2);
    // The record's first extent is added first, at VCN 32. The second one
    // then reaches VCN 40, past it, and is refused.
    Store<ULONGLONG>(image, extents[0] + attr_lowest_vcn_field,
                     NtfsBrowserTests::mft_two_extents_second_vcn);
    Store<ULONGLONG>(image, extents[0] + attr_highest_vcn_field,
                     NtfsBrowserTests::mft_two_extents_second_vcn);
    Store<ULONGLONG>(image, extents[1] + attr_highest_vcn_field,
                     NtfsBrowserTests::mft_two_extents_second_vcn + 8);
    const NtfsVolume<S> volume(
        std::make_unique<MemoryDiskReader>(std::move(image)));
    REQUIRE(volume.IsVolumeOK());
    CHECK_FALSE(
        CanReadRecord(volume, NtfsBrowserTests::mft_two_extents_first_vcn));
  }
}

TEMPLATE_TEST_CASE_SIG("$MFT reads records through chained continuations",
                       "[mft-extent-map]", ((Cache::Strategy S), S),
                       Cache::Strategy::NoCache, Cache::Strategy::FullCache) {
  SECTION("a continuation reached through another one") {
    const NtfsVolume<S> volume(std::make_unique<MemoryDiskReader>(
        NtfsBrowserTests::BuildFakeNtfsImageWithMftDataExtentChain()));
    REQUIRE(volume.IsVolumeOK());
    CHECK(CanReadRecord(volume, chain_target_idx));
  }

  SECTION("an unresolvable continuation beside a resolvable one") {
    const NtfsVolume<S> volume(std::make_unique<MemoryDiskReader>(
        NtfsBrowserTests::BuildFakeNtfsImageWithUnresolvableMftDataExtent()));
    REQUIRE(volume.IsVolumeOK());
    CHECK(
        CanReadRecord(volume, NtfsBrowserTests::mft_unresolvable_good_record));
  }
}
