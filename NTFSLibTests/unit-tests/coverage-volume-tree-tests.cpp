#include <ntfs-browser/win-types.h>

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <catch2/catch_template_test_macros.hpp>
#include <catch2/catch_test_macros.hpp>

#include <ntfs-browser/data/attr-defines.h>
#include <ntfs-browser/data/attr-type.h>
#include <ntfs-browser/disk-reader.h>
#include <ntfs-browser/file-record.h>
#include <ntfs-browser/index-entry.h>
#include <ntfs-browser/mask.h>
#include <ntfs-browser/mft-idx.h>
#include <ntfs-browser/mft-tree.h>
#include <ntfs-browser/ntfs-volume.h>
#include <ntfs-browser/strategy.h>

#include "fake-ntfs-image.h"
#include "memory-disk-reader.h"

using NtfsBrowser::FileRecord;
using NtfsBrowser::MftTree;
using NtfsBrowser::NtfsVolume;
namespace Attr = NtfsBrowser::Attr;
namespace Cache = NtfsBrowser::Cache;
namespace Mft = NtfsBrowser::Mft;
using NtfsBrowserTests::MemoryDiskReader;
using NtfsBrowserTests::NonAsciiNameLayout;

namespace {

// $MFT starts at byte 1024: lcn_mft 1 times the fake 1 KiB cluster.
constexpr size_t mft_base_offset = 1024;
// Offset of offset_of_attr, a WORD in the file record header.
constexpr size_t record_first_attr_field = 20;
// Offset of an attribute header's total_size DWORD.
constexpr size_t attr_total_size_field = 4;
// Offset of an attribute header's name_length BYTE.
constexpr size_t attr_name_length_field = 9;
// Offset of a resident attribute's body offset, a WORD.
constexpr size_t attr_value_offset_field = 20;
// Offset of a non-resident header's lowest VCN.
constexpr size_t attr_lowest_vcn_field = 16;
// Offset of a non-resident header's highest VCN.
constexpr size_t attr_highest_vcn_field = 24;
// Offset of a non-resident header's real size.
constexpr size_t attr_real_size_field = 48;
// Offset of a non-resident header's initialized size.
constexpr size_t attr_ini_size_field = 56;
// Offset of a non-resident header's data run list, a WORD.
constexpr size_t attr_data_run_offset_field = 32;
// Offset of a resident attribute's body length, a DWORD.
constexpr size_t attr_body_size_field = 16;
// Offset of the name space BYTE in a $FILE_NAME body (Data::Filename).
constexpr size_t filename_name_space_field = 65;
// Name space value of a DOS-only $FILE_NAME, which MftTree treats as an alias.
constexpr BYTE filename_dos_namespace = 2;
// Offset of an attribute list entry's record length, a WORD.
constexpr size_t list_entry_record_size_field = 4;
// Offset of the major version in $VOLUME_INFORMATION, after 8 reserved bytes.
constexpr size_t volume_info_major_field = 8;
// Offset of the name length in a $FILE_NAME body (Data::Filename).
constexpr size_t filename_name_length_field = 64;
// Boot sector offsets of the BPB fields the size tests patch.
constexpr size_t bpb_bytes_per_sector_field = 0x0B;
constexpr size_t bpb_sectors_per_cluster_field = 0x0D;
constexpr size_t bpb_clusters_per_file_record_field = 0x40;
// Offset of the "NTFS" OEM id that the boot sector signature check reads.
constexpr size_t bpb_oem_id_field = 3;
// Marks the end of a file record's attribute list.
constexpr DWORD end_of_attrs_marker = 0xFFFFFFFF;
// A type no attribute list accepts, so the scan stops at its entry.
constexpr DWORD invalid_attr_type = 0x1234;
// Mask for the 48-bit record number inside a file reference.
constexpr ULONGLONG ref_record_mask = 0x0000FFFFFFFFFFFFULL;
// Sequence number the MFT tree fixture gives its root directory.
constexpr ULONGLONG mft_tree_root_sequence = 5;
// Bytes of a file record: the fixture's record size.
constexpr size_t record_bytes = NtfsBrowserTests::fake_file_record_size;
// Lowest VCN the mismatched extension extent is moved to, unlisted by
// $ATTRIBUTE_LIST.
constexpr ULONGLONG unlisted_start_vcn = 17;
// Lowest VCN of the second extent in the two-extents fixture, moved onto the
// first.
constexpr ULONGLONG overlapping_start_vcn = 16;
// Major version an NT4-era volume reports, below the NTFS 3.0 floor.
constexpr BYTE old_major_version = 2;
// Size a $UpCase DATA attribute is shrunk to, far below the 128 KiB table.
constexpr ULONGLONG short_upcase_size = 100;

// Record number of $Volume.
constexpr ULONGLONG volume_idx = static_cast<ULONGLONG>(Mft::Idx::Volume);
// Record number of $MFT.
constexpr ULONGLONG mft_idx = static_cast<ULONGLONG>(Mft::Idx::Mft);
// Record number of $UpCase.
constexpr ULONGLONG upcase_idx = static_cast<ULONGLONG>(Mft::Idx::UpCase);
// Record number of the root directory.
constexpr ULONGLONG root_idx = static_cast<ULONGLONG>(Mft::Idx::Root);

template <typename T>
[[nodiscard]] T Load(const std::vector<BYTE>& image, size_t offset) {
  T value{};
  std::memcpy(&value, &image.at(offset), sizeof(T));
  return value;
}

template <typename T>
void Store(std::vector<BYTE>& image, size_t offset, T value) {
  std::memcpy(&image.at(offset), &value, sizeof(T));
}

// Builds a file reference from a record number and a sequence number.
[[nodiscard]] constexpr ULONGLONG MakeRef(ULONGLONG record,
                                          ULONGLONG sequence) {
  return record | (sequence << 48U);
}

[[nodiscard]] size_t RecordOffset(ULONGLONG idx) {
  return mft_base_offset + record_bytes * static_cast<size_t>(idx);
}

// Zeroes one whole file record, so its magic no longer matches.
void ZeroRecord(std::vector<BYTE>& image, ULONGLONG idx) {
  std::fill_n(image.begin() + static_cast<std::ptrdiff_t>(RecordOffset(idx)),
              record_bytes, BYTE{0});
}

// Byte offsets of every attribute of the given type in record idx, in order.
[[nodiscard]] std::vector<size_t> AttrOffsets(const std::vector<BYTE>& image,
                                              ULONGLONG idx, Attr::Type type) {
  std::vector<size_t> found;
  const size_t record = RecordOffset(idx);
  const size_t record_end = record + record_bytes;
  size_t offset = record + Load<WORD>(image, record + record_first_attr_field);
  while (offset + (2 * sizeof(DWORD)) <= record_end) {
    const auto attr_type = Load<DWORD>(image, offset);
    const auto total_size = Load<DWORD>(image, offset + attr_total_size_field);
    if (attr_type == end_of_attrs_marker || total_size == 0) {
      break;
    }
    if (attr_type == static_cast<DWORD>(type)) {
      found.push_back(offset);
    }
    offset += total_size;
  }
  return found;
}

// Byte offset of the first attribute of the given type; fails when absent.
[[nodiscard]] size_t FirstAttr(const std::vector<BYTE>& image, ULONGLONG idx,
                               Attr::Type type) {
  const std::vector<size_t> offsets = AttrOffsets(image, idx, type);
  REQUIRE(!offsets.empty());
  return offsets.front();
}

// First physical cluster of a non-resident attribute's first data run.
[[nodiscard]] ULONGLONG FirstRunLcn(const std::vector<BYTE>& image,
                                    size_t attr) {
  const size_t run =
      attr + Load<WORD>(image, attr + attr_data_run_offset_field);
  const auto header = Load<BYTE>(image, run);
  const size_t length_bytes = header & 0x0FU;
  const size_t lcn_bytes = header >> 4U;
  const size_t lcn_field = run + 1 + length_bytes;
  ULONGLONG lcn = 0;
  for (size_t i = 0; i < lcn_bytes; i++) {
    lcn |= static_cast<ULONGLONG>(Load<BYTE>(image, lcn_field + i)) << (8U * i);
  }
  return lcn;
}

// Byte offset of a resident attribute's body.
[[nodiscard]] size_t ResidentBody(const std::vector<BYTE>& image, size_t attr) {
  return attr + Load<WORD>(image, attr + attr_value_offset_field);
}

// Repoints the record number of every $FILE_NAME of idx, keeping its sequence.
void SetFileNameParents(std::vector<BYTE>& image, ULONGLONG idx,
                        ULONGLONG record_number) {
  for (const size_t attr : AttrOffsets(image, idx, Attr::Type::FileName)) {
    const size_t body = ResidentBody(image, attr);
    const auto ref = Load<ULONGLONG>(image, body);
    Store<ULONGLONG>(image, body, (ref & ~ref_record_mask) | record_number);
  }
}

// Writes new_ref over every $FILE_NAME of idx that names from_record.
void RedirectFileNames(std::vector<BYTE>& image, ULONGLONG idx,
                       ULONGLONG from_record, ULONGLONG new_ref) {
  for (const size_t attr : AttrOffsets(image, idx, Attr::Type::FileName)) {
    const size_t body = ResidentBody(image, attr);
    if ((Load<ULONGLONG>(image, body) & ref_record_mask) == from_record) {
      Store<ULONGLONG>(image, body, new_ref);
    }
  }
}

// Serves a fake image, refusing every read that overlaps [begin, end).
class RefusingDiskReader : public NtfsBrowser::IDiskReader {
 public:
  RefusingDiskReader(std::vector<BYTE> image, ULONGLONG begin, ULONGLONG end,
                     std::shared_ptr<size_t> refused)
      : inner_(std::move(image)),
        begin_(begin),
        end_(end),
        refused_(std::move(refused)) {}

  bool Open(std::wstring_view /*path*/) override { return false; }

  [[nodiscard]] bool ReadInto(LARGE_INTEGER& addr,
                              std::span<BYTE> dest) const override {
    const auto start = static_cast<ULONGLONG>(addr.QuadPart);
    if (start < end_ && start + dest.size() > begin_) {
      ++*refused_;
      return false;
    }
    return inner_.ReadInto(addr, dest);
  }

 private:
  MemoryDiskReader inner_;
  ULONGLONG begin_;
  ULONGLONG end_;
  std::shared_ptr<size_t> refused_;
};

// Raw attribute callbacks that fired since the test last reset the count.
size_t raw_attr_calls = 0;

void CountRawAttr(const Attr::HeaderCommon& /*header*/, bool& /*discard*/) {
  ++raw_attr_calls;
}

// Parses $Volume's VolumeInformation attribute through volume.
template <Cache::Strategy S>
void ParseVolumeInformation(const NtfsVolume<S>& volume) {
  FileRecord<S> volume_record(volume);
  volume_record.SetAttrMask(Attr::Mask::VolumeInformation);
  REQUIRE(volume_record.ParseFileRecord(volume_idx));
  REQUIRE(volume_record.ParseAttrs());
}

// Opens the MFT tree fixture as a FullCache volume.
std::unique_ptr<NtfsVolume<Cache::Strategy::FullCache>> OpenMftTree() {
  auto volume = std::make_unique<NtfsVolume<Cache::Strategy::FullCache>>(
      std::make_unique<MemoryDiskReader>(
          NtfsBrowserTests::BuildFakeNtfsImageWithMftTree()));
  REQUIRE(volume->IsVolumeOK());
  return volume;
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

  FileRecord<Cache::Strategy::NoCache> record(volume);
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

TEMPLATE_TEST_CASE_SIG(
    "FindSubEntry still finds a name when $UpCase is unusable", "[cov-vol]",
    ((Cache::Strategy S), S), Cache::Strategy::NoCache,
    Cache::Strategy::FullCache) {
  std::vector<BYTE> image =
      NtfsBrowserTests::BuildFakeNtfsImageWithNonAsciiNames(
          NonAsciiNameLayout::IndexRoot, true);

  SECTION("$UpCase record does not parse") { ZeroRecord(image, upcase_idx); }

  SECTION("$UpCase data is shorter than the table") {
    Store<ULONGLONG>(image,
                     FirstAttr(image, upcase_idx, Attr::Type::Data) +
                         attr_real_size_field,
                     short_upcase_size);
  }

  SECTION("$UpCase initialized size is shorter than the table") {
    Store<ULONGLONG>(image,
                     FirstAttr(image, upcase_idx, Attr::Type::Data) +
                         attr_ini_size_field,
                     short_upcase_size);
  }

  SECTION("$UpCase table does not map a to A") {
    const size_t attr = FirstAttr(image, upcase_idx, Attr::Type::Data);
    const auto table_offset =
        FirstRunLcn(image, attr) * NtfsBrowserTests::fake_cluster_size;
    std::fill_n(image.begin() + static_cast<std::ptrdiff_t>(table_offset),
                NtfsBrowserTests::fake_cluster_size, BYTE{0});
  }

  const NtfsVolume<S> volume(
      std::make_unique<MemoryDiskReader>(std::move(image)));
  REQUIRE(volume.IsVolumeOK());

  FileRecord<S> root(volume);
  root.SetAttrMask(Attr::Mask::IndexRoot | Attr::Mask::IndexAllocation);
  REQUIRE(root.ParseFileRecord(root_idx));
  REQUIRE(root.ParseAttrs());

  const std::optional<NtfsBrowser::IndexEntry> found =
      root.FindSubEntry(NtfsBrowserTests::non_ascii_acute_name);
  REQUIRE(found.has_value());
  CHECK(found->GetFileReference() == NtfsBrowserTests::non_ascii_acute_mft_ref);
}

TEST_CASE("MftTree copies and moves its scanned namespace", "[cov-vol]") {
  const auto volume = OpenMftTree();
  const MftTree source(*volume);

  MftTree copied(source);
  CHECK(copied.Entries().size() == source.Entries().size());

  MftTree assigned(*volume);
  assigned = copied;
  CHECK(assigned.Entries().size() == source.Entries().size());

  MftTree moved(std::move(copied));
  CHECK(moved.Entries().size() == source.Entries().size());

  MftTree target(*volume);
  target = std::move(moved);
  CHECK(target.Find(NtfsBrowserTests::mft_tree_docs_idx) != nullptr);
}

TEST_CASE("MftTree skips a file name with no characters", "[cov-vol]") {
  std::vector<BYTE> image = NtfsBrowserTests::BuildFakeNtfsImageWithMftTree();
  // Only the first $FILE_NAME of report.txt loses its name; its DOS alias
  // stays.
  Store<BYTE>(image,
              ResidentBody(
                  image, FirstAttr(image, NtfsBrowserTests::mft_tree_report_idx,
                                   Attr::Type::FileName)) +
                  filename_name_length_field,
              0);
  const auto volume = std::make_unique<NtfsVolume<Cache::Strategy::FullCache>>(
      std::make_unique<MemoryDiskReader>(std::move(image)));
  REQUIRE(volume->IsVolumeOK());

  const MftTree tree(*volume);

  const MftTree::Entry* entry =
      tree.Find(NtfsBrowserTests::mft_tree_report_idx);
  REQUIRE(entry != nullptr);
  CHECK(entry->names.size() == 1);
}

TEST_CASE("MftTree keeps a record whose names are all DOS aliases",
          "[cov-vol]") {
  std::vector<BYTE> image = NtfsBrowserTests::BuildFakeNtfsImageWithMftTree();
  for (const size_t attr :
       AttrOffsets(image, NtfsBrowserTests::mft_tree_report_idx,
                   Attr::Type::FileName)) {
    Store<BYTE>(image, ResidentBody(image, attr) + filename_name_space_field,
                filename_dos_namespace);
  }
  const auto volume = std::make_unique<NtfsVolume<Cache::Strategy::FullCache>>(
      std::make_unique<MemoryDiskReader>(std::move(image)));
  REQUIRE(volume->IsVolumeOK());

  const MftTree tree(*volume);

  CHECK_FALSE(tree.GetPath(NtfsBrowserTests::mft_tree_report_idx).empty());
}

TEST_CASE("MftTree reports a lost ancestor that has no usable name",
          "[cov-vol]") {
  std::vector<BYTE> image = NtfsBrowserTests::BuildFakeNtfsImageWithMftTree();
  // Docs keeps its directory flag and sequence, but loses every name.
  for (const size_t attr : AttrOffsets(
           image, NtfsBrowserTests::mft_tree_docs_idx, Attr::Type::FileName)) {
    Store<BYTE>(image, ResidentBody(image, attr) + filename_name_length_field,
                0);
  }
  const auto volume = std::make_unique<NtfsVolume<Cache::Strategy::FullCache>>(
      std::make_unique<MemoryDiskReader>(std::move(image)));
  REQUIRE(volume->IsVolumeOK());

  const MftTree tree(*volume);

  std::optional<ULONGLONG> lost;
  static_cast<void>(tree.GetPath(NtfsBrowserTests::mft_tree_report_idx, &lost));
  CHECK(lost == NtfsBrowserTests::mft_tree_docs_idx);
}

TEST_CASE("FindSubEntry falls back when a $UpCase read is refused",
          "[cov-vol]") {
  std::vector<BYTE> image =
      NtfsBrowserTests::BuildFakeNtfsImageWithNonAsciiNames(
          NonAsciiNameLayout::IndexRoot, true);
  const ULONGLONG table_begin =
      FirstRunLcn(image, FirstAttr(image, upcase_idx, Attr::Type::Data)) *
      NtfsBrowserTests::fake_cluster_size;
  auto refused = std::make_shared<size_t>(0);
  const NtfsVolume<Cache::Strategy::NoCache> volume(
      std::make_unique<RefusingDiskReader>(
          std::move(image), table_begin,
          table_begin + NtfsBrowserTests::fake_cluster_size, refused));
  REQUIRE(volume.IsVolumeOK());

  FileRecord<Cache::Strategy::NoCache> root(volume);
  root.SetAttrMask(Attr::Mask::IndexRoot | Attr::Mask::IndexAllocation);
  REQUIRE(root.ParseFileRecord(root_idx));
  REQUIRE(root.ParseAttrs());

  const std::optional<NtfsBrowser::IndexEntry> found =
      root.FindSubEntry(NtfsBrowserTests::non_ascii_acute_name);
  REQUIRE(found.has_value());
  CHECK(found->GetFileReference() == NtfsBrowserTests::non_ascii_acute_mft_ref);
  CHECK(*refused > 0);
}

TEST_CASE("MftTree marks a name invalid when its parent is not a directory",
          "[cov-vol]") {
  std::vector<BYTE> image = NtfsBrowserTests::BuildFakeNtfsImageWithMftTree();
  SetFileNameParents(image, NtfsBrowserTests::mft_tree_report_idx,
                     NtfsBrowserTests::mft_tree_hard_link_idx);
  const auto volume = std::make_unique<NtfsVolume<Cache::Strategy::FullCache>>(
      std::make_unique<MemoryDiskReader>(std::move(image)));
  REQUIRE(volume->IsVolumeOK());

  const MftTree tree(*volume);

  const MftTree::Entry* entry =
      tree.Find(NtfsBrowserTests::mft_tree_report_idx);
  REQUIRE(entry != nullptr);
  for (const MftTree::Name& name : entry->names) {
    CHECK_FALSE(name.parent_valid);
  }
  CHECK_FALSE(tree.IsReachable(NtfsBrowserTests::mft_tree_report_idx));
}

TEST_CASE("MftTree marks a name invalid when its parent is not in the scan",
          "[cov-vol]") {
  std::vector<BYTE> image = NtfsBrowserTests::BuildFakeNtfsImageWithMftTree();
  SetFileNameParents(image, NtfsBrowserTests::mft_tree_report_idx,
                     NtfsBrowserTests::mft_tree_zeroed_idx);
  const auto volume = std::make_unique<NtfsVolume<Cache::Strategy::FullCache>>(
      std::make_unique<MemoryDiskReader>(std::move(image)));
  REQUIRE(volume->IsVolumeOK());

  const MftTree tree(*volume);

  const MftTree::Entry* entry =
      tree.Find(NtfsBrowserTests::mft_tree_report_idx);
  REQUIRE(entry != nullptr);
  for (const MftTree::Name& name : entry->names) {
    CHECK_FALSE(name.parent_valid);
  }
  std::optional<ULONGLONG> lost;
  static_cast<void>(tree.GetPath(NtfsBrowserTests::mft_tree_report_idx, &lost));
  CHECK(lost == NtfsBrowserTests::mft_tree_zeroed_idx);
}

TEST_CASE("MftTree files a record once when two of its names share a parent",
          "[cov-vol]") {
  std::vector<BYTE> image = NtfsBrowserTests::BuildFakeNtfsImageWithMftTree();
  // link-b moves from Docs to the root, so both names of record 18 name the
  // root.
  RedirectFileNames(image, NtfsBrowserTests::mft_tree_hard_link_idx,
                    NtfsBrowserTests::mft_tree_docs_idx,
                    MakeRef(root_idx, mft_tree_root_sequence));
  const auto volume = std::make_unique<NtfsVolume<Cache::Strategy::FullCache>>(
      std::make_unique<MemoryDiskReader>(std::move(image)));
  REQUIRE(volume->IsVolumeOK());

  const MftTree tree(*volume);

  const auto children = tree.Children(root_idx);
  CHECK(std::count(children.begin(), children.end(),
                   NtfsBrowserTests::mft_tree_hard_link_idx) == 1);
}

TEST_CASE("MftTree GetPath reports nothing for an unknown record or name",
          "[cov-vol]") {
  const auto volume = OpenMftTree();
  const MftTree tree(*volume);

  CHECK_FALSE(tree.IsReachable(999));

  std::optional<ULONGLONG> lost = root_idx;
  CHECK(tree.GetPath(999, &lost).empty());
  CHECK_FALSE(lost.has_value());

  lost = root_idx;
  CHECK(tree.GetPath(NtfsBrowserTests::mft_tree_docs_idx, 99, &lost).empty());
  CHECK_FALSE(lost.has_value());
}
