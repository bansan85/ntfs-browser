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
#include <ntfs-browser/namespace.h>
#include <ntfs-browser/ntfs-volume.h>

#include "memory-disk-reader.h"
#include "volume-tree-support.h"

using NTFS_BROWSER_NAMESPACE::NtfsVolume;
using NTFS_BROWSER_NAMESPACE::Io::FileRecord;
namespace Attr = NTFS_BROWSER_NAMESPACE::Attr;
namespace Cache = NTFS_BROWSER_NAMESPACE::Cache;
namespace Mft = NTFS_BROWSER_NAMESPACE::Mft;
namespace Tests = NTFS_BROWSER_NAMESPACE::Tests;
using Tests::MemoryDiskReader;

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
constexpr ULONGLONG declared_mft_size = 40 * Tests::fake_file_record_size;

// Records the split fixture maps: the base extent's VCN 0 and the
// continuation's VCN mft_data_split_target_idx.
constexpr ULONGLONG split_mapped_records = Tests::mft_data_split_target_idx + 1;

// Records the two-extents fixture maps: its second extent reaches VCN 32.
constexpr ULONGLONG two_extents_mapped_records =
    Tests::mft_two_extents_second_vcn + 1;

// Declared record count below the two-extents fixture's mapped range.
constexpr ULONGLONG truncated_records = 20;

// Record the chain fixture's last continuation reaches, past every naively
// reachable extent.
constexpr ULONGLONG chain_target_idx = Tests::mft_chain_ext_a_start_vcn;

// Sets $MFT's base DATA real_size, which GetRecordsCount() caps at.
void SetMftRealSize(std::vector<BYTE>& image, ULONGLONG size) {
  Store<ULONGLONG>(
      image, FirstAttr(image, mft_idx, Attr::Type::Data) + attr_real_size_field,
      size);
}

// The $MFT whose DATA is split across a continuation, declared far larger
// than its extents map.
std::vector<BYTE> SplitImage() {
  std::vector<BYTE> image =
      Tests::BuildFakeNtfsImageWithMftDataSplitAcrossAttributeList();
  SetMftRealSize(image, declared_mft_size);
  return image;
}

// The $MFT whose DATA has two extents in one continuation record, declared
// real_size bytes long.
std::vector<BYTE> TwoExtentsImage(ULONGLONG real_size = declared_mft_size) {
  std::vector<BYTE> image =
      Tests::BuildFakeNtfsImageWithMftDataTwoExtentsInOneRecord();
  SetMftRealSize(image, real_size);
  return image;
}

// Mounts image, which MUST come up as a volume.
template <Cache::Strategy S>
std::unique_ptr<const NtfsVolume<S>> Mount(std::vector<BYTE> image) {
  auto volume = std::make_unique<const NtfsVolume<S>>(
      std::make_unique<MemoryDiskReader>(std::move(image)));
  REQUIRE(volume->IsVolumeOK());
  return volume;
}

// Parses record idx through volume and reports whether it could be read.
template <Cache::Strategy S>
bool CanReadRecord(const NtfsVolume<S>& volume, ULONGLONG idx) {
  FileRecord<S> record(volume);
  return record.ParseFileRecord(idx);
}

// The split fixture's continuation is refused: $MFT keeps only its fixed
// records, and the record the continuation would map does not parse.
template <Cache::Strategy S>
void CheckSplitContinuationRefused(std::vector<BYTE> image) {
  const auto volume = Mount<S>(std::move(image));

  CHECK(volume->GetRecordsCount() == fixed_address_records);
  CHECK_FALSE(CanReadRecord(*volume, Tests::mft_data_split_target_idx));
}

// Zeroes the one cluster at lcn.
void ZeroCluster(std::vector<BYTE>& image, DWORD lcn) {
  const auto begin = static_cast<std::ptrdiff_t>(static_cast<size_t>(lcn) *
                                                 Tests::fake_cluster_size);
  std::fill_n(image.begin() + begin, Tests::fake_cluster_size, BYTE{0});
}

}  // namespace

TEMPLATE_TEST_CASE_SIG("$MFT counts only the records its extents map",
                       "[mft-extent-map]", ((Cache::Strategy S), S),
                       Cache::Strategy::NoCache, Cache::Strategy::FullCache) {
  SECTION("base extent alone, declared size inside it") {
    const auto volume = Mount<S>(Tests::BuildFakeNtfsImage());

    CHECK(volume->GetRecordsCount() == Tests::sentinel_record_count);
  }

  SECTION("base extent alone, declared size past the fixed records") {
    std::vector<BYTE> image = Tests::BuildFakeNtfsImage();
    SetMftRealSize(image, declared_mft_size);
    const auto volume = Mount<S>(std::move(image));

    CHECK(volume->GetRecordsCount() == fixed_address_records);
  }

  SECTION("split DATA counts through its continuation") {
    const auto volume = Mount<S>(SplitImage());

    CHECK(volume->GetRecordsCount() == split_mapped_records);
    CHECK(CanReadRecord(*volume, Tests::mft_data_split_target_idx));
  }

  SECTION("two extents in one record count through the last one") {
    const auto volume = Mount<S>(TwoExtentsImage());

    CHECK(volume->GetRecordsCount() == two_extents_mapped_records);
    CHECK(CanReadRecord(*volume, Tests::mft_two_extents_first_vcn));
    CHECK(CanReadRecord(*volume, Tests::mft_two_extents_second_vcn));
  }

  SECTION("a declared size below the mapped range caps the count") {
    const auto volume = Mount<S>(
        TwoExtentsImage(truncated_records * Tests::fake_file_record_size));

    CHECK(volume->GetRecordsCount() == truncated_records);
  }

  SECTION("a record past every extent does not parse") {
    const auto volume = Mount<S>(SplitImage());

    CHECK_FALSE(CanReadRecord(*volume, split_mapped_records));
  }
}

TEMPLATE_TEST_CASE_SIG("$MFT rejects a DATA continuation it cannot map",
                       "[mft-extent-map]", ((Cache::Strategy S), S),
                       Cache::Strategy::NoCache, Cache::Strategy::FullCache) {
  SECTION("named continuation") {
    std::vector<BYTE> image = SplitImage();
    Store<BYTE>(
        image,
        FirstAttr(image, Tests::mft_data_split_ext_idx, Attr::Type::Data) +
            attr_name_length_field,
        1);
    CheckSplitContinuationRefused<S>(std::move(image));
  }

  SECTION("inverted VCN range") {
    std::vector<BYTE> image = SplitImage();
    const size_t attr =
        FirstAttr(image, Tests::mft_data_split_ext_idx, Attr::Type::Data);
    const auto lowest = Load<ULONGLONG>(image, attr + attr_lowest_vcn_field);
    Store<ULONGLONG>(image, attr + attr_highest_vcn_field, lowest - 1);
    CheckSplitContinuationRefused<S>(std::move(image));
  }

  SECTION("start VCN its $ATTRIBUTE_LIST entry does not name") {
    std::vector<BYTE> image = SplitImage();
    const size_t attr =
        FirstAttr(image, Tests::mft_data_split_ext_idx, Attr::Type::Data);
    Store<ULONGLONG>(image, attr + attr_lowest_vcn_field, unlisted_start_vcn);
    Store<ULONGLONG>(image, attr + attr_highest_vcn_field, unlisted_start_vcn);
    CheckSplitContinuationRefused<S>(std::move(image));
  }

  SECTION("base last VCN that overflows a byte offset") {
    const auto volume =
        Mount<S>(Tests::BuildFakeNtfsImageWithMftDataLastVcnOverflow());

    CHECK(volume->GetRecordsCount() == 1);
    CHECK_FALSE(
        CanReadRecord(*volume, Tests::mft_last_vcn_overflow_target_idx));
  }

  SECTION("continuation overlapping the extent before it") {
    std::vector<BYTE> image = TwoExtentsImage();
    const std::vector<size_t> extents =
        AttrOffsets(image, Tests::mft_two_extents_ext_idx, Attr::Type::Data);
    REQUIRE(extents.size() == 2);
    Store<ULONGLONG>(image, extents[1] + attr_lowest_vcn_field,
                     overlapping_start_vcn);
    Store<ULONGLONG>(image, extents[1] + attr_highest_vcn_field,
                     overlapping_start_vcn);
    // If the overlapping extent were accepted, VCN 16 would read this cluster.
    ZeroCluster(image, Tests::mft_two_extents_second_lcn);
    const auto volume = Mount<S>(std::move(image));

    CHECK(CanReadRecord(*volume, Tests::mft_two_extents_first_vcn));
  }

  SECTION("continuation overlapping the extent after it") {
    std::vector<BYTE> image = TwoExtentsImage();
    const std::vector<size_t> extents =
        AttrOffsets(image, Tests::mft_two_extents_ext_idx, Attr::Type::Data);
    REQUIRE(extents.size() == 2);
    // The record's first extent is added first, at VCN 32. The second one
    // then reaches VCN 40, past it, and is refused.
    Store<ULONGLONG>(image, extents[0] + attr_lowest_vcn_field,
                     Tests::mft_two_extents_second_vcn);
    Store<ULONGLONG>(image, extents[0] + attr_highest_vcn_field,
                     Tests::mft_two_extents_second_vcn);
    Store<ULONGLONG>(image, extents[1] + attr_highest_vcn_field,
                     Tests::mft_two_extents_second_vcn + 8);
    const auto volume = Mount<S>(std::move(image));

    CHECK_FALSE(CanReadRecord(*volume, Tests::mft_two_extents_first_vcn));
  }
}

TEMPLATE_TEST_CASE_SIG("$MFT reads records through chained continuations",
                       "[mft-extent-map]", ((Cache::Strategy S), S),
                       Cache::Strategy::NoCache, Cache::Strategy::FullCache) {
  SECTION("a continuation reached through another one") {
    const auto volume =
        Mount<S>(Tests::BuildFakeNtfsImageWithMftDataExtentChain());

    CHECK(CanReadRecord(*volume, chain_target_idx));
  }

  SECTION("an unresolvable continuation beside a resolvable one") {
    const auto volume =
        Mount<S>(Tests::BuildFakeNtfsImageWithUnresolvableMftDataExtent());

    CHECK(CanReadRecord(*volume, Tests::mft_unresolvable_good_record));
  }
}
