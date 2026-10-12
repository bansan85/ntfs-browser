#include <ntfs-browser/win-types.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstring>
#include <initializer_list>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <catch2/catch_template_test_macros.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <gsl/narrow>

#include <ntfs-browser/attr/mask.h>
#include <ntfs-browser/attr/type.h>
#include <ntfs-browser/cache/strategy.h>
#include <ntfs-browser/index-entry.h>
#include <ntfs-browser/io/file-record.h>
#include <ntfs-browser/mft/idx.h>
#include <ntfs-browser/ntfs-volume.h>
#include <ntfs-browser/volume-options.h>

#include "data/file-record-flag.h"
#include "data/file-record-header.h"
#include "data/filename.h"
#include "data/header-non-resident.h"
#include "data/header-resident.h"
#include "data/index-entry-flag.h"
#include "data/index-entry.h"
#include "data/index-root.h"
#include "data/standard-information.h"
#include "data/std-info-permission.h"
#include "data/volume-information.h"
#include "fake-ntfs-image.h"
#include "file-record-header-edit.h"
#include "memory-disk-reader.h"
#include "test-log-sink.h"

namespace Attr = NtfsBrowser::Attr;
namespace Cache = NtfsBrowser::Cache;
namespace Data = NtfsBrowser::Data;
namespace Mft = NtfsBrowser::Mft;
using Catch::Matchers::ContainsSubstring;
using NtfsBrowser::IndexEntryView;
using NtfsBrowser::NtfsVolume;
using NtfsBrowser::VolumeOptions;
using NtfsBrowser::Data::StdInfoPermission;
using NtfsBrowser::Io::FileRecord;
using NtfsBrowserTests::MemoryDiskReader;
using NtfsBrowserTests::TakeCapturedLog;

namespace {

// Bytes in one file record, as the fake images lay them out.
constexpr size_t record_bytes = NtfsBrowserTests::fake_file_record_size;
// $MFT slot the scratch records fill: free in every fake image, and below
// Mft::Idx::User, so it is read straight from disk.
constexpr ULONGLONG scratch_idx = 6;
// Byte offset of scratch_idx: $MFT starts one cluster into the image.
constexpr size_t scratch_offset =
    static_cast<size_t>(NtfsBrowserTests::fake_cluster_size) +
    static_cast<size_t>(scratch_idx) * record_bytes;
// Sequence number the scratch record carries. Any non-zero value works.
constexpr WORD scratch_sequence = 1;
// Where the update sequence array sits. Its words are zero, so a record that
// is zero at the block ends still verifies.
constexpr WORD usa_offset = 40;
// First attribute of a scratch record, past the header and the array.
constexpr WORD first_attr_offset = 56;
// Bytes in a resident attribute header. Its body follows immediately.
constexpr DWORD resident_header_size = 24;
// Bytes in a non-resident attribute header. Its run list follows it.
constexpr DWORD non_resident_header_size = 64;
// Total size of an empty non-resident attribute: header plus run terminator,
// padded to 8.
constexpr DWORD non_resident_total_size = 72;

using RecordBytes = std::array<BYTE, record_bytes>;

// Zeroed in-use record whose first attribute sits at offset_of_attr.
RecordBytes MakeScratchRecord(WORD offset_of_attr) {
  RecordBytes record{};
  NtfsBrowserTests::EditFileRecordHeader(
      record, [&](Data::FileRecordHeader& header) {
        header.magic = Data::FileRecordHeader::file_record_magic;
        header.offset_of_us = usa_offset;
        header.size_of_us = 3;
        header.offset_of_attr = offset_of_attr;
        header.flags = Data::FileRecordFlag::InUse;
        header.seq_no = scratch_sequence;
      });
  return record;
}

// Copies value into record at offset.
template <typename T>
void PutAt(RecordBytes& record, size_t offset, const T& value) {
  std::memcpy(&record.at(offset), &value, sizeof(T));
}

// Rounds an attribute size up to 8 bytes, as NTFS does.
DWORD AlignAttr(size_t size) {
  return gsl::narrow<DWORD>((size + 7U) & ~size_t{7U});
}

// Writes a resident attribute, its body after the header. Returns its size.
DWORD PutResidentAttr(RecordBytes& record, DWORD offset, Attr::Type type,
                      std::span<const BYTE> body) {
  Data::HeaderResident header{};
  header.header.type = type;
  header.header.non_resident = 0;
  header.header.total_size = AlignAttr(resident_header_size + body.size());
  header.attr_size = gsl::narrow<DWORD>(body.size());
  header.attr_offset = static_cast<WORD>(resident_header_size);
  PutAt(record, offset, header);
  if (!body.empty()) {
    std::memcpy(&record.at(offset + resident_header_size), body.data(),
                body.size());
  }
  return header.header.total_size;
}

// Writes an empty non-resident attribute: no runs, no data. Returns its size.
DWORD PutNonResidentAttr(RecordBytes& record, DWORD offset, Attr::Type type) {
  Data::HeaderNonResident header{};
  header.header.type = type;
  header.header.non_resident = 1;
  header.header.total_size = non_resident_total_size;
  header.data_run_offset = static_cast<WORD>(non_resident_header_size);
  PutAt(record, offset, header);
  return non_resident_total_size;
}

// Writes the end-of-attributes marker.
void PutEndMarker(RecordBytes& record, DWORD offset) {
  PutAt(record, offset, static_cast<DWORD>(Attr::Type::All));
}

// BuildFakeNtfsImage() with record in its scratch slot.
std::vector<BYTE> ImageWithScratch(const RecordBytes& record) {
  std::vector<BYTE> image = NtfsBrowserTests::BuildFakeNtfsImage();
  std::memcpy(&image.at(scratch_offset), record.data(), record.size());
  return image;
}

// Byte view of a value, for a resident attribute body.
template <typename T>
std::span<const BYTE> AsBytes(const T& value) {
  return {reinterpret_cast<const BYTE*>(&value), sizeof(T)};
}

// What ParseAttrs() left of one attribute type in a scratch record.
struct Parsed {
  bool ok = false;
  size_t count = 0;
  bool non_resident = false;
};

// Parses a scratch record holding one attribute, and reports what became of
// the attributes of type. recover selects the recovering mode.
template <Cache::Strategy S>
Parsed ParseScratch(const RecordBytes& record, Attr::Type type, bool recover) {
  auto reader = std::make_unique<MemoryDiskReader>(ImageWithScratch(record));
  const NtfsVolume<S> volume(std::move(reader),
                             VolumeOptions{.recover_errors = recover});
  REQUIRE(volume.IsVolumeOK());

  FileRecord<S> file(volume);
  REQUIRE(file.ParseFileRecord(scratch_idx));

  Parsed result{};
  result.ok = file.ParseAttrs();
  const auto& attrs = file.GetAttr(type);
  result.count = attrs.size();
  result.non_resident = !attrs.empty() && attrs.front()->IsNonResident();
  return result;
}

// A type that is always resident on disk parses from a resident header, and
// fails the whole walk from a non-resident one, in either mode.
template <Cache::Strategy S>
void CheckResidentOnly(Attr::Type type, std::span<const BYTE> body) {
  RecordBytes resident = MakeScratchRecord(first_attr_offset);
  DWORD offset = first_attr_offset;
  offset += PutResidentAttr(resident, offset, type, body);
  PutEndMarker(resident, offset);

  const Parsed accepted = ParseScratch<S>(resident, type, false);
  CHECK(accepted.ok);
  CHECK(accepted.count == 1);
  CHECK_FALSE(accepted.non_resident);

  RecordBytes non_resident = MakeScratchRecord(first_attr_offset);
  offset = first_attr_offset;
  offset += PutNonResidentAttr(non_resident, offset, type);
  PutEndMarker(non_resident, offset);
  for (const bool recover : {false, true}) {
    const Parsed rejected = ParseScratch<S>(non_resident, type, recover);
    CHECK_FALSE(rejected.ok);
    CHECK(rejected.count == 0);
  }
}

// A type that may be either way parses in each form, and the wrapper keeps the
// residency the header gives.
template <Cache::Strategy S>
void CheckFollowsResidency(Attr::Type type, std::span<const BYTE> body) {
  RecordBytes resident = MakeScratchRecord(first_attr_offset);
  DWORD offset = first_attr_offset;
  offset += PutResidentAttr(resident, offset, type, body);
  PutEndMarker(resident, offset);

  const Parsed as_resident = ParseScratch<S>(resident, type, false);
  CHECK(as_resident.ok);
  CHECK(as_resident.count == 1);
  CHECK_FALSE(as_resident.non_resident);

  RecordBytes non_resident = MakeScratchRecord(first_attr_offset);
  offset = first_attr_offset;
  offset += PutNonResidentAttr(non_resident, offset, type);
  PutEndMarker(non_resident, offset);

  const Parsed as_non_resident = ParseScratch<S>(non_resident, type, false);
  CHECK(as_non_resident.ok);
  CHECK(as_non_resident.count == 1);
  CHECK(as_non_resident.non_resident);
}

// Options that run the orphan scan, and keep an entry whose named record is
// freed.
constexpr VolumeOptions recover_keep_deleted{.include_deleted = true,
                                             .recover_errors = true};

// Options that run the orphan scan, and drop an entry whose named record is
// freed or reused.
constexpr VolumeOptions recover_only{.recover_errors = true};

// Names TraverseSubEntries() reports on the root directory of image, in
// callback order.
template <Cache::Strategy S>
std::vector<std::wstring> RootNames(std::vector<BYTE> image,
                                    VolumeOptions options) {
  auto reader = std::make_unique<MemoryDiskReader>(std::move(image));
  const NtfsVolume<S> volume(std::move(reader), options);
  REQUIRE(volume.IsVolumeOK());

  FileRecord<S> root(volume);
  root.SetAttrMask(Attr::Mask::IndexRoot | Attr::Mask::IndexAllocation);
  REQUIRE(root.ParseFileRecord(static_cast<ULONGLONG>(Mft::Idx::Root)));
  REQUIRE(root.ParseAttrs());

  std::vector<std::wstring> names;
  root.TraverseSubEntries(
      [](const IndexEntryView& index_entry, void* context) {
        static_cast<std::vector<std::wstring>*>(context)->emplace_back(
            index_entry.GetFilename());
      },
      &names);
  return names;
}

// Checks that names holds exactly expected, in order.
void CheckNames(const std::vector<std::wstring>& names,
                std::initializer_list<std::wstring_view> expected) {
  CHECK(std::ranges::equal(names, expected));
}

// Bytes in one index entry of FileNameIndexRootBody(): a terminal entry with
// no sub-node, sized as the fake images size one.
constexpr size_t index_entry_size = 32;

// A file-name $INDEX_ROOT body holding one terminal entry, with no sub-node.
std::array<BYTE, sizeof(Data::IndexRoot) + index_entry_size>
    FileNameIndexRootBody() {
  std::array<BYTE, sizeof(Data::IndexRoot) + index_entry_size> body{};

  Data::IndexRoot root{};
  root.attr_type = Attr::Type::FileName;
  root.ib_size = NtfsBrowserTests::fake_cluster_size;
  root.clusters_per_ib = 1;
  // Counted from the entry_offset field, to the entry after the header.
  root.entry_offset = static_cast<DWORD>(
      sizeof(Data::IndexRoot) - offsetof(Data::IndexRoot, entry_offset));
  root.total_entry_size = static_cast<DWORD>(index_entry_size);
  root.alloc_entry_size = static_cast<DWORD>(index_entry_size);
  std::memcpy(body.data(), &root, sizeof(root));

  Data::IndexEntry last{};
  last.size = static_cast<WORD>(index_entry_size);
  last.flags = Data::IndexEntryFlag::Last;
  std::memcpy(body.data() + sizeof(root), &last, sizeof(last));

  return body;
}

// Sets the DOS permission bits of record idx's resident $STANDARD_INFORMATION.
void SetStdInfoPermission(std::vector<BYTE>& image, ULONGLONG idx,
                          StdInfoPermission permission) {
  const size_t record_offset =
      static_cast<size_t>(NtfsBrowserTests::fake_cluster_size) +
      static_cast<size_t>(idx) * record_bytes;

  Data::FileRecordHeader header{};
  std::memcpy(&header, &image.at(record_offset),
              Data::FileRecordHeader::min_file_record_header_size);
  const size_t attr_offset = record_offset + header.offset_of_attr;
  const auto& attr =
      *reinterpret_cast<const Data::HeaderResident*>(&image.at(attr_offset));

  const size_t permission_offset =
      attr_offset + attr.attr_offset +
      offsetof(Data::StandardInformation, permission);
  const auto bits = static_cast<DWORD>(permission);
  std::memcpy(&image.at(permission_offset), &bits, sizeof(bits));
}

}  // namespace

TEMPLATE_TEST_CASE_SIG(
    "AllocAttr takes an always-resident type only in resident form",
    "[file-record][characterization]", ((Cache::Strategy S), S),
    Cache::Strategy::NoCache, Cache::Strategy::FullCache) {
  const Data::StandardInformation std_info{};
  const Data::Filename file_name{};
  const Data::VolumeInformation vol_info{};
  const std::array<BYTE, sizeof(Data::IndexRoot) + index_entry_size>
      index_root = FileNameIndexRootBody();

  SECTION("StandardInformation") {
    CheckResidentOnly<S>(Attr::Type::StandardInformation, AsBytes(std_info));
  }
  SECTION("FileName") {
    CheckResidentOnly<S>(Attr::Type::FileName, AsBytes(file_name));
  }
  SECTION("VolumeName") {
    CheckResidentOnly<S>(Attr::Type::VolumeName, std::span<const BYTE>{});
  }
  SECTION("VolumeInformation") {
    CheckResidentOnly<S>(Attr::Type::VolumeInformation, AsBytes(vol_info));
  }
  SECTION("IndexRoot") {
    CheckResidentOnly<S>(Attr::Type::IndexRoot, index_root);
  }
  SECTION("IndexAllocation is the inverse: resident is refused") {
    RecordBytes resident = MakeScratchRecord(first_attr_offset);
    DWORD offset = first_attr_offset;
    offset += PutResidentAttr(resident, offset, Attr::Type::IndexAllocation,
                              std::span<const BYTE>{});
    PutEndMarker(resident, offset);
    for (const bool recover : {false, true}) {
      CHECK_FALSE(
          ParseScratch<S>(resident, Attr::Type::IndexAllocation, recover).ok);
    }

    RecordBytes non_resident = MakeScratchRecord(first_attr_offset);
    offset = first_attr_offset;
    offset +=
        PutNonResidentAttr(non_resident, offset, Attr::Type::IndexAllocation);
    PutEndMarker(non_resident, offset);
    const Parsed accepted =
        ParseScratch<S>(non_resident, Attr::Type::IndexAllocation, false);
    CHECK(accepted.ok);
    CHECK(accepted.count == 1);
    CHECK(accepted.non_resident);
  }
}

TEMPLATE_TEST_CASE_SIG(
    "AllocAttr wraps Data, Bitmap and LoggedUtilityStream by residency",
    "[file-record][characterization]", ((Cache::Strategy S), S),
    Cache::Strategy::NoCache, Cache::Strategy::FullCache) {
  const std::array<BYTE, 4> small_body{0xDE, 0xAD, 0xBE, 0xEF};
  const std::array<BYTE, 8> bitmap_body{0x01, 0, 0, 0, 0, 0, 0, 0};

  SECTION("Data") { CheckFollowsResidency<S>(Attr::Type::Data, small_body); }
  SECTION("Bitmap") {
    CheckFollowsResidency<S>(Attr::Type::Bitmap, bitmap_body);
  }
  SECTION("LoggedUtilityStream") {
    CheckFollowsResidency<S>(Attr::Type::LoggedUtilityStream, small_body);
  }
}

TEMPLATE_TEST_CASE_SIG(
    "AllocAttr wraps an AttributeList by residency when the list is resolved",
    "[file-record][characterization]", ((Cache::Strategy S), S),
    Cache::Strategy::NoCache, Cache::Strategy::FullCache) {
  CheckFollowsResidency<S>(Attr::Type::AttributeList, std::span<const BYTE>{});
}

TEMPLATE_TEST_CASE_SIG(
    "AllocAttr wraps a type with no wrapper of its own by residency",
    "[file-record][characterization]", ((Cache::Strategy S), S),
    Cache::Strategy::NoCache, Cache::Strategy::FullCache) {
  const std::array<BYTE, 4> small_body{0xDE, 0xAD, 0xBE, 0xEF};

  SECTION("ObjectId") {
    CheckFollowsResidency<S>(Attr::Type::ObjectId, small_body);
  }
  SECTION("ReparsePoint") {
    CheckFollowsResidency<S>(Attr::Type::ReparsePoint, small_body);
  }
}

TEMPLATE_TEST_CASE_SIG(
    "A non-resident $ATTRIBUTE_LIST in $MFT still resolves its DATA "
    "continuation, as a resident one does",
    "[file-record][characterization]", ((Cache::Strategy S), S),
    Cache::Strategy::NoCache, Cache::Strategy::FullCache) {
  for (const bool non_resident_list : {false, true}) {
    auto reader = std::make_unique<MemoryDiskReader>(
        non_resident_list
            ? NtfsBrowserTests::
                  BuildFakeNtfsImageWithNonResidentMftAttributeList()
            : NtfsBrowserTests::
                  BuildFakeNtfsImageWithMftDataSplitAcrossAttributeList());

    const NtfsVolume<S> volume(std::move(reader));
    REQUIRE(volume.IsVolumeOK());
    CHECK(volume.GetRecordsCount() == 1);

    FileRecord<S> record(volume);
    CHECK(record.ParseFileRecord(NtfsBrowserTests::mft_data_split_target_idx));
  }
}

TEMPLATE_TEST_CASE_SIG(
    "ParseAttr lets a raw callback discard an attribute, or keep it",
    "[file-record][characterization]", ((Cache::Strategy S), S),
    Cache::Strategy::NoCache, Cache::Strategy::FullCache) {
  const std::array<BYTE, 4> small_body{0xDE, 0xAD, 0xBE, 0xEF};
  RecordBytes record = MakeScratchRecord(first_attr_offset);
  DWORD offset = first_attr_offset;
  offset += PutResidentAttr(record, offset, Attr::Type::Data, small_body);
  PutEndMarker(record, offset);

  auto reader = std::make_unique<MemoryDiskReader>(ImageWithScratch(record));
  const NtfsVolume<S> volume(std::move(reader));
  REQUIRE(volume.IsVolumeOK());

  FileRecord<S> file(volume);
  REQUIRE(file.ParseFileRecord(scratch_idx));

  SECTION("discarded before it is wrapped") {
    CHECK(file.InstallAttrRawCB(Attr::Type::Data,
                                [](const Attr::HeaderCommon& /*head*/,
                                   bool& discard) { discard = true; }));
    REQUIRE(file.ParseAttrs());
    CHECK(file.GetAttr(Attr::Type::Data).empty());
  }
  SECTION("kept when the callback leaves discard clear") {
    CHECK(file.InstallAttrRawCB(
        Attr::Type::Data,
        [](const Attr::HeaderCommon& /*head*/, bool& /*discard*/) {}));
    REQUIRE(file.ParseAttrs());
    CHECK(file.GetAttr(Attr::Type::Data).size() == 1);
  }
}

TEMPLATE_TEST_CASE_SIG(
    "TraverseSubEntries recovery reports an orphaned entry whose named record "
    "is in use under the entry's sequence number",
    "[file-record][index-block][characterization]", ((Cache::Strategy S), S),
    Cache::Strategy::NoCache, Cache::Strategy::FullCache) {
  const std::vector<std::wstring> names = RootNames<S>(
      NtfsBrowserTests::BuildFakeNtfsImageWithOrphanedEntryNamedRecord(
          NtfsBrowserTests::FakeNamedRecordState::InUseSameSequence),
      recover_only);
  CheckNames(names, {NtfsBrowserTests::orphaned_block_reachable_name,
                     NtfsBrowserTests::orphaned_block_orphan_name});
}

TEMPLATE_TEST_CASE_SIG(
    "TraverseSubEntries recovery drops an orphaned entry whose named record is "
    "freed, unless include_deleted is on",
    "[file-record][index-block][characterization]", ((Cache::Strategy S), S),
    Cache::Strategy::NoCache, Cache::Strategy::FullCache) {
  const auto freed_image = [] {
    return NtfsBrowserTests::BuildFakeNtfsImageWithOrphanedEntryNamedRecord(
        NtfsBrowserTests::FakeNamedRecordState::Deleted);
  };

  CheckNames(RootNames<S>(freed_image(), recover_only),
             {NtfsBrowserTests::orphaned_block_reachable_name});
  CheckNames(RootNames<S>(freed_image(), recover_keep_deleted),
             {NtfsBrowserTests::orphaned_block_reachable_name,
              NtfsBrowserTests::orphaned_block_orphan_name});
}

TEMPLATE_TEST_CASE_SIG(
    "TraverseSubEntries recovery scans only the declared number of index "
    "blocks",
    "[file-record][index-block][characterization]", ((Cache::Strategy S), S),
    Cache::Strategy::NoCache, Cache::Strategy::FullCache) {
  const std::vector<std::wstring> names = RootNames<S>(
      NtfsBrowserTests::BuildFakeNtfsImageWithIndexAllocationShape(
          NtfsBrowserTests::FakeIndexAllocShape{.declared_block_count = 1}),
      recover_keep_deleted);
  CheckNames(names, {NtfsBrowserTests::orphaned_block_reachable_name});
}

TEMPLATE_TEST_CASE_SIG(
    "TraverseSubEntries recovery maps no index block when the allocation's "
    "last VCN precedes its start VCN",
    "[file-record][index-block][characterization]", ((Cache::Strategy S), S),
    Cache::Strategy::NoCache, Cache::Strategy::FullCache) {
  const std::vector<std::wstring> names = RootNames<S>(
      NtfsBrowserTests::BuildFakeNtfsImageWithIndexAllocationShape(
          NtfsBrowserTests::FakeIndexAllocShape{.start_vcn = 1, .last_vcn = 0}),
      recover_keep_deleted);
  CheckNames(names, {});
}

TEMPLATE_TEST_CASE_SIG(
    "TraverseSubEntries recovery caps the orphan scan at its block limit when "
    "the mapped and declared counts both exceed it",
    "[file-record][index-block][characterization]", ((Cache::Strategy S), S),
    Cache::Strategy::NoCache, Cache::Strategy::FullCache) {
  constexpr ULONGLONG huge_block_count = 70000;
  auto reader = std::make_unique<MemoryDiskReader>(
      NtfsBrowserTests::BuildFakeNtfsImageWithIndexAllocationShape(
          NtfsBrowserTests::FakeIndexAllocShape{
              .last_vcn = huge_block_count - 1,
              .mapped_clusters = huge_block_count,
              .declared_block_count = huge_block_count}));

  const NtfsVolume<S> volume(std::move(reader), recover_keep_deleted);
  REQUIRE(volume.IsVolumeOK());

  FileRecord<S> root(volume);
  root.SetAttrMask(Attr::Mask::IndexRoot | Attr::Mask::IndexAllocation);
  REQUIRE(root.ParseFileRecord(static_cast<ULONGLONG>(Mft::Idx::Root)));
  REQUIRE(root.ParseAttrs());

  (void)TakeCapturedLog();
  std::vector<std::wstring> names;
  root.TraverseSubEntries(
      [](const IndexEntryView& index_entry, void* context) {
        static_cast<std::vector<std::wstring>*>(context)->emplace_back(
            index_entry.GetFilename());
      },
      &names);

  CheckNames(names, {NtfsBrowserTests::orphaned_block_reachable_name,
                     NtfsBrowserTests::orphaned_block_orphan_name});
  CHECK_THAT(TakeCapturedLog(),
             ContainsSubstring("orphan scan capped at 65536 of 70000"));
}

TEMPLATE_TEST_CASE_SIG(
    "FileRecord reports each STANDARD_INFORMATION flag accessor only for its "
    "own permission bit, and none when the bits are clear",
    "[file-record][characterization]", ((Cache::Strategy S), S),
    Cache::Strategy::NoCache, Cache::Strategy::FullCache) {
  using Getter = bool (FileRecord<S>::*)() const noexcept;
  const std::array<std::pair<Getter, StdInfoPermission>, 13> flags{{
      {&FileRecord<S>::IsReadOnly, StdInfoPermission::ReadOnly},
      {&FileRecord<S>::IsHidden, StdInfoPermission::Hidden},
      {&FileRecord<S>::IsSystem, StdInfoPermission::System},
      {&FileRecord<S>::IsArchive, StdInfoPermission::Archive},
      {&FileRecord<S>::IsDevice, StdInfoPermission::Device},
      {&FileRecord<S>::IsNormal, StdInfoPermission::Normal},
      {&FileRecord<S>::IsTemporary, StdInfoPermission::Temp},
      {&FileRecord<S>::IsCompressed, StdInfoPermission::Compressed},
      {&FileRecord<S>::IsOffline, StdInfoPermission::Offline},
      {&FileRecord<S>::IsNotContentIndexed, StdInfoPermission::Nci},
      {&FileRecord<S>::IsEncrypted, StdInfoPermission::Encrypted},
      {&FileRecord<S>::IsSparse, StdInfoPermission::Sparse},
      {&FileRecord<S>::IsReparsePoint, StdInfoPermission::Reparse},
  }};

  const auto parse_with = [](StdInfoPermission permission) {
    std::vector<BYTE> image = NtfsBrowserTests::BuildFakeNtfsImageWithMftTree();
    SetStdInfoPermission(image, NtfsBrowserTests::mft_tree_report_idx,
                         permission);
    return image;
  };

  for (const auto& [unused_getter, set_bit] : flags) {
    auto reader = std::make_unique<MemoryDiskReader>(parse_with(set_bit));
    const NtfsVolume<S> volume(std::move(reader));
    REQUIRE(volume.IsVolumeOK());

    FileRecord<S> record(volume);
    REQUIRE(record.ParseFileRecord(NtfsBrowserTests::mft_tree_report_idx));
    REQUIRE(record.ParseAttrs());
    for (const auto& [getter, bit] : flags) {
      CHECK(((record.*getter)() == (bit == set_bit)));
    }
  }

  auto clear_reader =
      std::make_unique<MemoryDiskReader>(parse_with(StdInfoPermission{}));
  const NtfsVolume<S> clear_volume(std::move(clear_reader));
  REQUIRE(clear_volume.IsVolumeOK());

  FileRecord<S> clear_record(clear_volume);
  REQUIRE(clear_record.ParseFileRecord(NtfsBrowserTests::mft_tree_report_idx));
  REQUIRE(clear_record.ParseAttrs());
  for (const auto& [getter, bit] : flags) {
    CHECK_FALSE((clear_record.*getter)());
  }
}

TEMPLATE_TEST_CASE_SIG(
    "IsDirectory and IsDeleted read the record header, and default to false "
    "before a record is parsed",
    "[file-record][characterization]", ((Cache::Strategy S), S),
    Cache::Strategy::NoCache, Cache::Strategy::FullCache) {
  {
    auto reader = std::make_unique<MemoryDiskReader>(
        NtfsBrowserTests::BuildFakeNtfsImage());
    const NtfsVolume<S> volume(std::move(reader));
    REQUIRE(volume.IsVolumeOK());

    FileRecord<S> unparsed(volume);
    CHECK_FALSE(unparsed.IsDirectory());
    CHECK_FALSE(unparsed.IsDeleted());

    FileRecord<S> root(volume);
    REQUIRE(root.ParseFileRecord(static_cast<ULONGLONG>(Mft::Idx::Root)));
    CHECK(root.IsDirectory());
    CHECK_FALSE(root.IsDeleted());
  }
  {
    auto reader = std::make_unique<MemoryDiskReader>(
        NtfsBrowserTests::BuildFakeNtfsImageWithMftTree());
    const NtfsVolume<S> volume(std::move(reader));
    REQUIRE(volume.IsVolumeOK());

    FileRecord<S> file(volume);
    REQUIRE(file.ParseFileRecord(NtfsBrowserTests::mft_tree_report_idx));
    CHECK_FALSE(file.IsDirectory());
    CHECK_FALSE(file.IsDeleted());
  }
  {
    auto reader = std::make_unique<MemoryDiskReader>(
        NtfsBrowserTests::BuildFakeNtfsImageWithDeletedVolumeRecord());
    const NtfsVolume<S> volume(std::move(reader));
    REQUIRE(volume.IsVolumeOK());

    FileRecord<S> freed(volume);
    REQUIRE(freed.ParseFileRecord(static_cast<ULONGLONG>(Mft::Idx::Volume)));
    CHECK(freed.IsDeleted());
    CHECK_FALSE(freed.IsDirectory());
  }
}
