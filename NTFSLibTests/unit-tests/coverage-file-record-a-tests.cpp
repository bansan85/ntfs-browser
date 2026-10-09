#include <ntfs-browser/win-types.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstring>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include <catch2/catch_template_test_macros.hpp>
#include <catch2/catch_test_macros.hpp>
#include <gsl/narrow>

#include <ntfs-browser/file-record.h>
#include <ntfs-browser/index-entry.h>
#include <ntfs-browser/mask.h>
#include <ntfs-browser/mft-idx.h>
#include <ntfs-browser/ntfs-volume.h>
#include <ntfs-browser/strategy.h>
#include <ntfs-browser/volume-options.h>

#include "data/file-record-flag.h"
#include "data/file-record-header.h"
#include "data/filename-namespace.h"
#include "data/filename.h"
#include "data/header-non-resident.h"
#include "data/header-resident.h"
#include "data/index-block.h"
#include "data/index-entry.h"
#include "data/index-root.h"
#include "fake-ntfs-image.h"
#include "file-record-header-edit.h"
#include "memory-disk-reader.h"

namespace NtfsBrowser {

template <Cache::Strategy S>
class AttrBase;

}  // namespace NtfsBrowser

namespace Attr = NtfsBrowser::Attr;
namespace Cache = NtfsBrowser::Cache;
namespace Data = NtfsBrowser::Data;
namespace Mft = NtfsBrowser::Mft;
using NtfsBrowser::AttrBase;
using NtfsBrowser::FileRecord;
using NtfsBrowser::IndexEntryView;
using NtfsBrowser::NtfsVolume;
using NtfsBrowser::VolumeOptions;
using NtfsBrowserTests::MemoryDiskReader;

namespace {

// Bytes in one file record, as BuildFakeNtfsImage() lays them out.
constexpr size_t record_bytes = 1024;
// $MFT slot the scratch records fill: free in the base image and below
// Mft::Idx::User, so it is read straight from disk.
constexpr ULONGLONG scratch_idx = 6;
// Byte offset of scratch_idx: $MFT starts at LCN 1, with 1 KiB clusters.
constexpr size_t scratch_offset = 1024 + scratch_idx * record_bytes;
// Byte offset of the root directory record (#5) in the base image.
constexpr size_t root_offset = 1024 + 5 * record_bytes;
// Where the update sequence array sits. Its usn and replacement words are
// zero, so a record that is zero at the block ends still verifies.
constexpr WORD usa_offset = 40;
// First attribute of a scratch record, past the header and the array.
constexpr WORD first_attr_offset = 56;
// Start of an attribute after a 960-byte one: its type fits in the record, its
// header does not.
constexpr WORD tail_attr_offset = 1016;
// Byte that ends the first 512-byte block. Keep it zero in scratch records.
constexpr size_t first_block_end = 510;
// Bytes in a resident attribute header. Its body follows immediately.
constexpr DWORD resident_header_size = 24;
// Bytes in a non-resident attribute header. Its run list follows it.
constexpr DWORD non_resident_header_size = 64;
// Total size of a non-resident attribute here: header plus a one-byte run
// list terminator, padded to 8.
constexpr DWORD non_resident_total_size = 72;
// Declared size that runs past the end of a scratch record.
constexpr DWORD overrunning_total_size = 2000;
// Word placed where a block end must hold the update sequence number.
constexpr WORD bad_fixup_word = 0x1234;
// Offset of total_size inside an attribute header, after its type.
constexpr size_t total_size_field_offset = 4;
// Attribute type no table entry matches.
constexpr auto unknown_attr_type = static_cast<Attr::Type>(0x1234);
// Byte size of one index block in the base image (1 cluster of 1 KiB).
constexpr size_t index_block_bytes = 1024;
// LCN of the index block gap_collation_search_name lives in.
constexpr size_t gap_collation_block_lcn = 20;
// First LCN of the three index blocks of the orphaned-block fixture.
constexpr size_t orphaned_blocks_first_lcn = 200;
// Value an index block's INDX signature is zeroed to, making it unreadable.
constexpr DWORD zero_signature = 0;
// Entries the gap-collation root reports: its one named sub-node entry.
constexpr size_t one_entry = 1;
// Sequence number the scratch record has. Any non-zero value works.
constexpr WORD scratch_sequence = 1;
// Extension record reference used by the link test: record 6, sequence 2.
constexpr ULONGLONG extension_base_ref = 6U | (2ULL << 48U);

using RecordBytes = std::array<BYTE, record_bytes>;

// Zeroed in-use record, its first attribute at offset_of_attr.
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

// Serves a MemoryDiskReader's image, but fails every read once fail is set.
class SwitchableReader : public MemoryDiskReader {
 public:
  using MemoryDiskReader::MemoryDiskReader;

  [[nodiscard]] bool ReadInto(LARGE_INTEGER& addr,
                              std::span<BYTE> dest) const override {
    return !fail && MemoryDiskReader::ReadInto(addr, dest);
  }

  bool fail = false;
};

// Byte offset of the first attribute of type in the record at record_offset.
std::optional<size_t> FindAttributeOffset(const std::vector<BYTE>& image,
                                          size_t record_offset,
                                          Attr::Type type) {
  Data::FileRecordHeader header{};
  std::memcpy(&header, &image.at(record_offset),
              Data::FileRecordHeader::min_file_record_header_size);
  size_t offset = record_offset + header.offset_of_attr;
  while (true) {
    Attr::Type current{};
    std::memcpy(&current, &image.at(offset), sizeof(current));
    if (current == Attr::Type::All) {
      return std::nullopt;
    }
    if (current == type) {
      return offset;
    }
    DWORD total = 0;
    std::memcpy(&total, &image.at(offset + total_size_field_offset),
                sizeof(total));
    if (total == 0) {
      return std::nullopt;
    }
    offset += total;
  }
}

// Byte view of a value, for a resident attribute body.
template <typename T>
std::span<const BYTE> AsBytes(const T& value) {
  return {reinterpret_cast<const BYTE*>(&value), sizeof(T)};
}

// Checks that the walk over record ends where the test expects, in both modes.
template <Cache::Strategy S>
void CheckWalkEndsEarly(const RecordBytes& record,
                        size_t kept_when_recovering) {
  for (const bool recover : {false, true}) {
    auto reader = std::make_unique<MemoryDiskReader>(ImageWithScratch(record));
    const NtfsVolume<S> volume(std::move(reader),
                               VolumeOptions{.recover_errors = recover});
    REQUIRE(volume.IsVolumeOK());

    FileRecord<S> file(volume);
    REQUIRE(file.ParseFileRecord(scratch_idx));
    // Strict drops the whole record. Recovery keeps the walk so far.
    CHECK(file.ParseAttrs() == recover);
    CHECK(file.GetAttr(Attr::Type::Data).size() ==
          (recover ? kept_when_recovering : 0));
  }
}

}  // namespace

TEMPLATE_TEST_CASE_SIG("FileRecord move constructor keeps the parsed record",
                       "[cov-fra]", ((Cache::Strategy S), S),
                       Cache::Strategy::NoCache, Cache::Strategy::FullCache) {
  auto reader = std::make_unique<MemoryDiskReader>(
      NtfsBrowserTests::BuildFakeNtfsImage());
  const NtfsVolume<S> volume(std::move(reader));
  REQUIRE(volume.IsVolumeOK());

  FileRecord<S> record(volume);
  REQUIRE(record.ParseFileRecord(static_cast<ULONGLONG>(Mft::Idx::Root)));

  FileRecord<S> moved(std::move(record));
  CHECK(moved.GetFileReference() ==
        std::optional<ULONGLONG>(static_cast<ULONGLONG>(Mft::Idx::Root)));
  CHECK(moved.IsDirectory());
}

TEMPLATE_TEST_CASE_SIG(
    "ParseFileRecord rejects a record whose update sequence does not verify "
    "or whose header cannot be built",
    "[cov-fra]", ((Cache::Strategy S), S), Cache::Strategy::NoCache,
    Cache::Strategy::FullCache) {
  RecordBytes bad_usn = MakeScratchRecord(first_attr_offset);
  PutEndMarker(bad_usn, first_attr_offset);
  PutAt(bad_usn, first_block_end, bad_fixup_word);

  RecordBytes bad_header = MakeScratchRecord(first_attr_offset);
  PutEndMarker(bad_header, first_attr_offset);
  NtfsBrowserTests::EditFileRecordHeader(
      bad_header,
      [](Data::FileRecordHeader& header) { header.offset_of_us = 0xFFFF; });

  for (const RecordBytes* record : {&bad_usn, &bad_header}) {
    auto reader = std::make_unique<MemoryDiskReader>(ImageWithScratch(*record));
    const NtfsVolume<S> volume(std::move(reader));
    REQUIRE(volume.IsVolumeOK());

    FileRecord<S> file(volume);
    CHECK_FALSE(file.ParseFileRecord(scratch_idx));
  }
}

TEST_CASE("ParseFileRecord reports a record read that the disk refuses",
          "[cov-fra]") {
  auto reader = std::make_unique<SwitchableReader>(
      NtfsBrowserTests::BuildFakeNtfsImage());
  SwitchableReader* disk = reader.get();
  const NtfsVolume<Cache::Strategy::NoCache> volume(std::move(reader));
  REQUIRE(volume.IsVolumeOK());

  disk->fail = true;
  FileRecord<Cache::Strategy::NoCache> file(volume);
  CHECK_FALSE(file.ParseFileRecord(scratch_idx));
}

TEMPLATE_TEST_CASE_SIG(
    "Attribute walk ends early when a header does not fit in the record",
    "[cov-fra]", ((Cache::Strategy S), S), Cache::Strategy::NoCache,
    Cache::Strategy::FullCache) {
  SECTION("only the type fits at the end of the record") {
    RecordBytes record = MakeScratchRecord(first_attr_offset);
    const std::vector<BYTE> body(tail_attr_offset - first_attr_offset -
                                 resident_header_size);
    PutResidentAttr(record, first_attr_offset, Attr::Type::Data, body);
    PutAt(record, tail_attr_offset, static_cast<DWORD>(Attr::Type::Data));
    CheckWalkEndsEarly<S>(record, 1);
  }

  SECTION("an attribute declares a size past the record") {
    RecordBytes record = MakeScratchRecord(first_attr_offset);
    const std::array<BYTE, 4> body{};
    PutResidentAttr(record, first_attr_offset, Attr::Type::Data, body);
    PutAt(record, first_attr_offset + total_size_field_offset,
          overrunning_total_size);
    CheckWalkEndsEarly<S>(record, 0);
  }
}

TEMPLATE_TEST_CASE_SIG(
    "Resident $INDEX_ALLOCATION rejects the record in both modes", "[cov-fra]",
    ((Cache::Strategy S), S), Cache::Strategy::NoCache,
    Cache::Strategy::FullCache) {
  RecordBytes record = MakeScratchRecord(first_attr_offset);
  DWORD offset = first_attr_offset;
  offset += PutResidentAttr(record, offset, Attr::Type::IndexAllocation, {});
  PutEndMarker(record, offset);

  for (const bool recover : {false, true}) {
    auto reader = std::make_unique<MemoryDiskReader>(ImageWithScratch(record));
    const NtfsVolume<S> volume(std::move(reader),
                               VolumeOptions{.recover_errors = recover});
    REQUIRE(volume.IsVolumeOK());

    FileRecord<S> file(volume);
    REQUIRE(file.ParseFileRecord(scratch_idx));
    CHECK_FALSE(file.ParseAttrs());
  }
}

TEMPLATE_TEST_CASE_SIG(
    "Bitmap and unhandled attributes parse in both residency forms",
    "[cov-fra]", ((Cache::Strategy S), S), Cache::Strategy::NoCache,
    Cache::Strategy::FullCache) {
  RecordBytes record = MakeScratchRecord(first_attr_offset);
  const std::array<BYTE, 1> bitmap_body{0xFF};
  const std::array<BYTE, 4> object_id_body{};
  DWORD offset = first_attr_offset;
  offset += PutResidentAttr(record, offset, Attr::Type::Bitmap, bitmap_body);
  offset += PutNonResidentAttr(record, offset, Attr::Type::Bitmap);
  offset += PutNonResidentAttr(record, offset, Attr::Type::ObjectId);
  offset += PutNonResidentAttr(record, offset, Attr::Type::LoggedUtilityStream);
  offset +=
      PutResidentAttr(record, offset, Attr::Type::ObjectId, object_id_body);
  PutEndMarker(record, offset);

  auto reader = std::make_unique<MemoryDiskReader>(ImageWithScratch(record));
  const NtfsVolume<S> volume(std::move(reader));
  REQUIRE(volume.IsVolumeOK());

  FileRecord<S> file(volume);
  REQUIRE(file.ParseFileRecord(scratch_idx));
  REQUIRE(file.ParseAttrs());

  CHECK(file.GetAttr(Attr::Type::Bitmap).size() == 2);
  CHECK(file.GetAttr(Attr::Type::ObjectId).size() == 2);
  CHECK(file.GetAttr(Attr::Type::LoggedUtilityStream).size() == 1);
}

TEMPLATE_TEST_CASE_SIG(
    "TraverseAttrs visits parsed attributes, honours stop and the mask",
    "[cov-fra]", ((Cache::Strategy S), S), Cache::Strategy::NoCache,
    Cache::Strategy::FullCache) {
  RecordBytes record = MakeScratchRecord(first_attr_offset);
  const std::array<BYTE, 4> data_body{};
  const std::array<BYTE, 1> bitmap_body{0xFF};
  DWORD offset = first_attr_offset;
  offset += PutResidentAttr(record, offset, Attr::Type::Data, data_body);
  offset += PutResidentAttr(record, offset, Attr::Type::Bitmap, bitmap_body);
  PutEndMarker(record, offset);

  auto reader = std::make_unique<MemoryDiskReader>(ImageWithScratch(record));
  const NtfsVolume<S> volume(std::move(reader));
  REQUIRE(volume.IsVolumeOK());

  FileRecord<S> file(volume);
  REQUIRE(file.ParseFileRecord(scratch_idx));
  REQUIRE(file.ParseAttrs());

  const typename FileRecord<S>::AttrsCallback empty_callback;
  file.TraverseAttrs(empty_callback, nullptr);

  const typename FileRecord<S>::AttrsCallback count_all =
      [](const AttrBase<S>& /*attr*/, void* context, bool* stop) {
        ++*static_cast<size_t*>(context);
        *stop = false;
      };
  size_t visits = 0;
  file.TraverseAttrs(count_all, &visits);
  CHECK(visits == 2);

  const typename FileRecord<S>::AttrsCallback stop_first =
      [](const AttrBase<S>& /*attr*/, void* context, bool* stop) {
        ++*static_cast<size_t*>(context);
        *stop = true;
      };
  visits = 0;
  file.TraverseAttrs(stop_first, &visits);
  CHECK(visits == 1);

  file.SetAttrMask(Attr::Mask::StandardInformation);
  visits = 0;
  file.TraverseAttrs(count_all, &visits);
  CHECK(visits == 0);
}

TEMPLATE_TEST_CASE_SIG(
    "Raw attribute callbacks ignore unknown types and clear on request",
    "[cov-fra]", ((Cache::Strategy S), S), Cache::Strategy::NoCache,
    Cache::Strategy::FullCache) {
  RecordBytes record = MakeScratchRecord(first_attr_offset);
  const std::array<BYTE, 4> data_body{};
  DWORD offset = first_attr_offset;
  offset += PutResidentAttr(record, offset, Attr::Type::Data, data_body);
  PutEndMarker(record, offset);

  auto reader = std::make_unique<MemoryDiskReader>(ImageWithScratch(record));
  const NtfsVolume<S> volume(std::move(reader));
  REQUIRE(volume.IsVolumeOK());

  FileRecord<S> file(volume);
  CHECK_FALSE(file.InstallAttrRawCB(unknown_attr_type,
                                    [](const Attr::HeaderCommon& /*head*/,
                                       bool& discard) { discard = true; }));
  REQUIRE(file.InstallAttrRawCB(Attr::Type::Data,
                                [](const Attr::HeaderCommon& /*head*/,
                                   bool& discard) { discard = true; }));
  REQUIRE(file.ParseFileRecord(scratch_idx));
  REQUIRE(file.ParseAttrs());
  CHECK(file.GetAttr(Attr::Type::Data).empty());

  file.ClearAttrRawCB();
  REQUIRE(file.ParseAttrs());
  CHECK(file.GetAttr(Attr::Type::Data).size() == 1);
}

TEMPLATE_TEST_CASE_SIG("Unparsed and unknown-type accessors return defaults",
                       "[cov-fra]", ((Cache::Strategy S), S),
                       Cache::Strategy::NoCache, Cache::Strategy::FullCache) {
  auto reader = std::make_unique<MemoryDiskReader>(
      NtfsBrowserTests::BuildFakeNtfsImage());
  const NtfsVolume<S> volume(std::move(reader));
  REQUIRE(volume.IsVolumeOK());

  FileRecord<S> file(volume);
  CHECK(file.GetSequenceNumber() == 0);
  CHECK(file.GetBaseRecordReference() == 0);
  CHECK_FALSE(file.IsExtensionRecord());
  CHECK(file.GetAttr(unknown_attr_type).empty());

  const FileRecord<S>& view = file;
  CHECK(view.GetAttr(unknown_attr_type).empty());

  RecordBytes record = MakeScratchRecord(first_attr_offset);
  NtfsBrowserTests::EditFileRecordHeader(
      record, [](Data::FileRecordHeader& header) {
        header.ref_to_base = extension_base_ref;
      });
  PutEndMarker(record, first_attr_offset);
  auto scratch_reader =
      std::make_unique<MemoryDiskReader>(ImageWithScratch(record));
  const NtfsVolume<S> scratch_volume(std::move(scratch_reader));
  REQUIRE(scratch_volume.IsVolumeOK());

  FileRecord<S> extension(scratch_volume);
  REQUIRE(extension.ParseFileRecord(scratch_idx));
  CHECK(extension.IsExtensionRecord());
  CHECK(extension.GetBaseRecordReference() == 6);
}

TEMPLATE_TEST_CASE_SIG(
    "Record without $STANDARD_INFORMATION reports default times and flags",
    "[cov-fra]", ((Cache::Strategy S), S), Cache::Strategy::NoCache,
    Cache::Strategy::FullCache) {
  RecordBytes record = MakeScratchRecord(first_attr_offset);
  PutEndMarker(record, first_attr_offset);

  auto reader = std::make_unique<MemoryDiskReader>(ImageWithScratch(record));
  const NtfsVolume<S> volume(std::move(reader));
  REQUIRE(volume.IsVolumeOK());

  FileRecord<S> file(volume);
  REQUIRE(file.ParseFileRecord(scratch_idx));
  REQUIRE(file.ParseAttrs());

  CHECK(file.GetFileName().empty());
  CHECK_FALSE(file.IsReadOnly());
  CHECK_FALSE(file.IsHidden());
  CHECK_FALSE(file.IsSystem());
  CHECK_FALSE(file.IsArchive());
  CHECK_FALSE(file.IsDevice());

  FILETIME write_time{};
  write_time.dwLowDateTime = 1;
  write_time.dwHighDateTime = 1;
  FILETIME create_time = write_time;
  FILETIME access_time = write_time;
  FILETIME change_time = write_time;
  file.GetFileTime(&write_time, &create_time, &access_time, &change_time);
  CHECK(write_time.dwLowDateTime == 0);
  CHECK(create_time.dwHighDateTime == 0);
  CHECK(access_time.dwLowDateTime == 0);
  CHECK(change_time.dwHighDateTime == 0);

  file.GetFileTime(nullptr, nullptr, nullptr, nullptr);
}

TEMPLATE_TEST_CASE_SIG(
    "Traversal and lookup skip an index that is not a file-name index",
    "[cov-fra]", ((Cache::Strategy S), S), Cache::Strategy::NoCache,
    Cache::Strategy::FullCache) {
  Data::IndexRoot root_body{};
  root_body.attr_type = Attr::Type::Data;

  RecordBytes record = MakeScratchRecord(first_attr_offset);
  DWORD offset = first_attr_offset;
  offset += PutResidentAttr(record, offset, Attr::Type::IndexRoot,
                            AsBytes(root_body));
  PutEndMarker(record, offset);

  auto reader = std::make_unique<MemoryDiskReader>(ImageWithScratch(record));
  const NtfsVolume<S> volume(std::move(reader),
                             VolumeOptions{.recover_errors = true});
  REQUIRE(volume.IsVolumeOK());

  FileRecord<S> file(volume);
  REQUIRE(file.ParseFileRecord(scratch_idx));
  REQUIRE(file.ParseAttrs());

  size_t entries = 0;
  file.TraverseSubEntries(
      [](const IndexEntryView& /*entry*/, void* context) {
        ++*static_cast<size_t*>(context);
      },
      &entries);
  CHECK(entries == 0);
  CHECK_FALSE(file.FindSubEntry(L"Anything").has_value());
}

TEMPLATE_TEST_CASE_SIG("Traversal and lookup on a record with no index at all",
                       "[cov-fra]", ((Cache::Strategy S), S),
                       Cache::Strategy::NoCache, Cache::Strategy::FullCache) {
  RecordBytes record = MakeScratchRecord(first_attr_offset);
  PutEndMarker(record, first_attr_offset);

  auto reader = std::make_unique<MemoryDiskReader>(ImageWithScratch(record));
  const NtfsVolume<S> volume(std::move(reader),
                             VolumeOptions{.recover_errors = true});
  REQUIRE(volume.IsVolumeOK());

  FileRecord<S> file(volume);
  REQUIRE(file.ParseFileRecord(scratch_idx));
  REQUIRE(file.ParseAttrs());

  size_t entries = 0;
  file.TraverseSubEntries(
      [](const IndexEntryView& /*entry*/, void* context) {
        ++*static_cast<size_t*>(context);
      },
      &entries);
  CHECK(entries == 0);
  CHECK_FALSE(file.FindSubEntry(L"Anything").has_value());
}

TEMPLATE_TEST_CASE_SIG(
    "Sub-node walks stop at a missing or unreadable $INDEX_ALLOCATION",
    "[cov-fra]", ((Cache::Strategy S), S), Cache::Strategy::NoCache,
    Cache::Strategy::FullCache) {
  std::vector<BYTE> image =
      NtfsBrowserTests::BuildFakeNtfsImageWithGapCollationSubNode();

  SECTION("the allocation attribute is absent") {
    const std::optional<size_t> alloc_offset =
        FindAttributeOffset(image, root_offset, Attr::Type::IndexAllocation);
    REQUIRE(alloc_offset.has_value());
    // A type with no index meaning leaves the root's sub-node pointer dangling.
    const auto replaced = static_cast<DWORD>(Attr::Type::ReparsePoint);
    std::memcpy(&image.at(*alloc_offset), &replaced, sizeof(replaced));
  }

  SECTION("the index block signature is unreadable") {
    const DWORD bad_signature = zero_signature;
    std::memcpy(&image.at(gap_collation_block_lcn * index_block_bytes),
                &bad_signature, sizeof(bad_signature));
  }

  auto reader = std::make_unique<MemoryDiskReader>(std::move(image));
  const NtfsVolume<S> volume(std::move(reader));
  REQUIRE(volume.IsVolumeOK());

  FileRecord<S> root(volume);
  REQUIRE(root.ParseFileRecord(static_cast<ULONGLONG>(Mft::Idx::Root)));
  REQUIRE(root.ParseAttrs());

  CHECK_FALSE(root.FindSubEntry(NtfsBrowserTests::gap_collation_search_name)
                  .has_value());

  size_t entries = 0;
  root.TraverseSubEntries(
      [](const IndexEntryView& /*entry*/, void* context) {
        ++*static_cast<size_t*>(context);
      },
      &entries);
  CHECK(entries == one_entry);
}

TEMPLATE_TEST_CASE_SIG("Orphan scan skips an index block it cannot parse",
                       "[cov-fra]", ((Cache::Strategy S), S),
                       Cache::Strategy::NoCache, Cache::Strategy::FullCache) {
  const VolumeOptions recover_keep_deleted{.include_deleted = true,
                                           .recover_errors = true};

  auto collect = [&](std::vector<BYTE> image) {
    auto reader = std::make_unique<MemoryDiskReader>(std::move(image));
    const NtfsVolume<S> volume(std::move(reader), recover_keep_deleted);
    REQUIRE(volume.IsVolumeOK());

    FileRecord<S> root(volume);
    REQUIRE(root.ParseFileRecord(static_cast<ULONGLONG>(Mft::Idx::Root)));
    REQUIRE(root.ParseAttrs());

    std::vector<std::wstring> names;
    root.TraverseSubEntries(
        [](const IndexEntryView& entry, void* context) {
          static_cast<std::vector<std::wstring>*>(context)->emplace_back(
              entry.GetFilename());
        },
        &names);
    return names;
  };

  const std::vector<std::wstring> intact =
      collect(NtfsBrowserTests::BuildFakeNtfsImageWithOrphanedIndexBlocks());
  CHECK(
      std::ranges::find(
          intact, std::wstring(NtfsBrowserTests::orphaned_block_orphan_name)) !=
      intact.end());

  std::vector<BYTE> image =
      NtfsBrowserTests::BuildFakeNtfsImageWithOrphanedIndexBlocks();
  const DWORD bad_signature = zero_signature;
  std::memcpy(&image.at((orphaned_blocks_first_lcn + 1) * index_block_bytes),
              &bad_signature, sizeof(bad_signature));
  const std::vector<std::wstring> damaged = collect(std::move(image));
  CHECK(std::ranges::find(
            damaged,
            std::wstring(NtfsBrowserTests::orphaned_block_orphan_name)) ==
        damaged.end());
  CHECK(std::ranges::find(
            damaged,
            std::wstring(NtfsBrowserTests::orphaned_block_reachable_name)) !=
        damaged.end());
}

TEMPLATE_TEST_CASE_SIG(
    "A record number past the MFT's reach is rejected without reading",
    "[cov-fra]", ((Cache::Strategy S), S), Cache::Strategy::NoCache,
    Cache::Strategy::FullCache) {
  // Record 0 without a $DATA attribute leaves the volume with no MFT data.
  RecordBytes mft = MakeScratchRecord(first_attr_offset);
  PutEndMarker(mft, first_attr_offset);
  std::vector<BYTE> image = NtfsBrowserTests::BuildFakeNtfsImage();
  // Record 0 of the MFT: its LCN is 1, so it sits one cluster in.
  constexpr size_t mft_record_offset = 1024;
  std::memcpy(&image.at(mft_record_offset), mft.data(), mft.size());

  auto reader = std::make_unique<MemoryDiskReader>(std::move(image));
  const NtfsVolume<S> volume(std::move(reader));

  FileRecord<S> file(volume);
  // Above 2^53 records, the byte offset of the record overflows a LONGLONG.
  CHECK_FALSE(file.ParseFileRecord(ULONGLONG{1} << 53U));
}

TEMPLATE_TEST_CASE_SIG(
    "TraverseSubEntries reports the entries of a file-name index root",
    "[cov-fra]", ((Cache::Strategy S), S), Cache::Strategy::NoCache,
    Cache::Strategy::FullCache) {
  auto reader = std::make_unique<MemoryDiskReader>(
      NtfsBrowserTests::BuildFakeNtfsImageWithRootIndexRootEntry());
  const NtfsVolume<S> volume(std::move(reader));
  REQUIRE(volume.IsVolumeOK());

  FileRecord<S> root(volume);
  REQUIRE(root.ParseFileRecord(static_cast<ULONGLONG>(Mft::Idx::Root)));
  REQUIRE(root.ParseAttrs());

  size_t entries = 0;
  root.TraverseSubEntries(
      [](const IndexEntryView& /*entry*/, void* context) {
        ++*static_cast<size_t*>(context);
      },
      &entries);
  CHECK(entries >= 1);
}

TEMPLATE_TEST_CASE_SIG(
    "Index block that points back to an earlier block ends the walk",
    "[cov-fra]", ((Cache::Strategy S), S), Cache::Strategy::NoCache,
    Cache::Strategy::FullCache) {
  std::vector<BYTE> image =
      NtfsBrowserTests::BuildFakeNtfsImageWithDeepIndexBlockChain();

  // LCN of the chain's first block, one cluster per block.
  constexpr size_t chain_first_lcn = 100;
  const size_t block_one = (chain_first_lcn + 1) * index_block_bytes;
  const std::array<BYTE, sizeof(ULONGLONG)> vcn_two{2, 0, 0, 0, 0, 0, 0, 0};
  const auto block_begin =
      image.begin() + static_cast<std::ptrdiff_t>(block_one);
  const auto found = std::search(block_begin, block_begin + index_block_bytes,
                                 vcn_two.begin(), vcn_two.end());
  REQUIRE(found != block_begin + index_block_bytes);
  // Redirects block 1's only sub-node pointer back to VCN 0.
  std::fill_n(found, sizeof(ULONGLONG), BYTE{0});

  auto reader = std::make_unique<MemoryDiskReader>(std::move(image));
  const NtfsVolume<S> volume(std::move(reader));
  REQUIRE(volume.IsVolumeOK());

  FileRecord<S> root(volume);
  REQUIRE(root.ParseFileRecord(static_cast<ULONGLONG>(Mft::Idx::Root)));
  REQUIRE(root.ParseAttrs());

  size_t entries = 0;
  root.TraverseSubEntries(
      [](const IndexEntryView& /*entry*/, void* context) {
        ++*static_cast<size_t*>(context);
      },
      &entries);
  CHECK(entries == 0);
  CHECK_FALSE(root.FindSubEntry(NtfsBrowserTests::index_block_chain_leaf_name)
                  .has_value());
}

TEMPLATE_TEST_CASE_SIG(
    "Unnamed sub-node pointer leads the lookup to a leaf entry deep in the "
    "index",
    "[cov-fra]", ((Cache::Strategy S), S), Cache::Strategy::NoCache,
    Cache::Strategy::FullCache) {
  std::vector<BYTE> image =
      NtfsBrowserTests::BuildFakeNtfsImageWithDeepIndexBlockChain();

  // LCN of the chain's first block, one cluster per block.
  constexpr size_t chain_first_lcn = 100;
  const size_t block_zero = chain_first_lcn * index_block_bytes;
  const size_t entry_at = block_zero + sizeof(Data::IndexBlock);
  Data::IndexEntry entry{};
  std::memcpy(&entry, &image.at(entry_at), sizeof(entry));
  // Last 8 bytes of block 0's only entry hold its sub-node VCN.
  const size_t vcn_slot = entry_at + entry.size - sizeof(ULONGLONG);
  const ULONGLONG leaf_vcn = NtfsBrowserTests::index_block_chain_length - 1;
  std::memcpy(&image.at(vcn_slot), &leaf_vcn, sizeof(leaf_vcn));

  auto reader = std::make_unique<MemoryDiskReader>(std::move(image));
  const NtfsVolume<S> volume(std::move(reader));
  REQUIRE(volume.IsVolumeOK());

  FileRecord<S> root(volume);
  REQUIRE(root.ParseFileRecord(static_cast<ULONGLONG>(Mft::Idx::Root)));
  REQUIRE(root.ParseAttrs());

  const std::optional<NtfsBrowser::IndexEntry> found =
      root.FindSubEntry(NtfsBrowserTests::index_block_chain_leaf_name);
  REQUIRE(found.has_value());
  CHECK(found->GetFileReference() ==
        NtfsBrowserTests::index_block_chain_leaf_mft_ref);
}
