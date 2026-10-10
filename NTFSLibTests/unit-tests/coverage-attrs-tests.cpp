#include <ntfs-browser/win-types.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstring>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

#include <catch2/catch_template_test_macros.hpp>
#include <catch2/catch_test_macros.hpp>
#include <spdlog/pattern_formatter.h>
#include <spdlog/spdlog.h>

#include <ntfs-browser/attr/base.h>  // IWYU pragma: keep
#include <ntfs-browser/attr/type.h>
#include <ntfs-browser/cache/strategy.h>
#include <ntfs-browser/filename.h>
#include <ntfs-browser/io/file-record.h>
#include <ntfs-browser/log/log.h>
#include <ntfs-browser/ntfs-volume.h>  // IWYU pragma: keep

#include "attr/bitmap.h"
#include "attr/file-name.h"
#include "attr/non-resident.h"
#include "attr/resident.h"
#include "attr/std-info.h"
#include "data/attribute-list.h"
#include "data/file-record-flag.h"
#include "data/file-record-header.h"
#include "data/filename-flag.h"
#include "data/filename-namespace.h"
#include "data/filename.h"
#include "data/header-non-resident.h"
#include "data/header-resident.h"
#include "data/standard-information.h"
#include "data/std-info-permission.h"
#include "fake-ntfs-image.h"
#include "file-record-header-edit.h"
#include "log/ntfs-common.h"
#include "memory-disk-reader.h"
#include "record/header.h"
#include "test-log-sink.h"
#include "upcase/upcase.h"

namespace Attr = NtfsBrowser::Attr;
namespace Cache = NtfsBrowser::Cache;
namespace Data = NtfsBrowser::Data;
namespace Log = NtfsBrowser::Log;
using NtfsBrowser::Filename;
using NtfsBrowser::NtfsVolume;
using NtfsBrowser::Io::FileRecord;

namespace {

// MFT index of the record each test builds. Clear of the fixtures' records.
constexpr ULONGLONG test_record_idx = 12;
// MFT index left zeroed, so a list entry naming it cannot be parsed.
constexpr ULONGLONG zero_record_idx = 13;
// MFT index of an extension record holding an undersized attribute.
constexpr ULONGLONG broken_ext_record_idx = 14;
// Cluster the non-resident $BITMAP maps. The image builder leaves it zero.
constexpr ULONGLONG bitmap_lcn = 50;
// Offset of the update sequence array in the test records: past every
// attribute, as in the fake images.
constexpr WORD us_offset = 1018;
// Offset of the update sequence array in the fixup-only buffers, which carry no
// attributes.
constexpr WORD fixup_us_offset = 48;
// Attributes start right after the 48-byte file record header.
constexpr WORD attr_offset = 48;
// Alignment every attribute's total size is padded to.
constexpr size_t attr_alignment = 8;
// Bytes of one $ATTRIBUTE_LIST entry as the tests write it: the whole struct.
constexpr size_t attr_list_entry_size = sizeof(Data::AttributeList);
// Timestamps written into the $STANDARD_INFORMATION and $FILE_NAME bodies.
// Arbitrary, distinct.
constexpr ULONGLONG std_create_time = 0x0102030405060708ULL;
constexpr ULONGLONG std_alter_time = 0x1112131415161718ULL;
constexpr ULONGLONG std_mft_time = 0x2122232425262728ULL;
constexpr ULONGLONG std_read_time = 0x3132333435363738ULL;
// Real size of the non-resident $BITMAP. Under one cluster, so a whole-cluster
// read comes back short.
constexpr ULONGLONG short_bitmap_size = 100;
// Resident $BITMAP bytes: bit 0 set, every other bit clear.
constexpr std::array<BYTE, 2> bitmap_bytes{0x01, 0x00};
// Run header of the one data run a test attribute maps: one length byte, one
// offset byte.
constexpr BYTE run_header_one_byte_each = 0x11;
// Length of that run, in clusters.
constexpr BYTE run_one_cluster = 1;
// Ends a data run list.
constexpr BYTE run_list_end = 0x00;
// An attribute type no $ATTRIBUTE_LIST entry may carry: not a multiple of 0x10
// and above the known types.
constexpr DWORD attr_list_type_invalid = 0x1234;
// Code point past U+10FFFF. No name can hold it, but a std::wstring can.
constexpr wchar_t beyond_unicode = static_cast<wchar_t>(0x110000);
// Size of a $FILE_NAME body too short for its own fixed fields.
constexpr size_t short_file_name_body = 16;
// Name length a $FILE_NAME claims while its body holds only one character.
constexpr BYTE oversized_name_length = 40;
// Allocated size the test $FILE_NAME claims.
constexpr ULONGLONG file_name_alloc_size = 0x2000;
// Real size the test $FILE_NAME claims.
constexpr ULONGLONG file_name_real_size = 0x1000;
// Parent directory the test $FILE_NAME is filed under (the root).
constexpr ULONGLONG file_name_parent_ref = 5;
// Length of the name the test $FILE_NAME carries.
constexpr BYTE file_name_length = 5;
// Byte offset of the first byte of the fake record that the update sequence
// array's second word holds.
constexpr size_t second_sector_end = 1022;
// Byte offset of the first sector's last word.
constexpr size_t first_sector_end = 510;

// Byte offset of MFT record idx in a fake image: past the boot cluster and
// every earlier record.
size_t RecordOffset(ULONGLONG idx) {
  return NtfsBrowserTests::fake_cluster_size +
         static_cast<size_t>(idx) * NtfsBrowserTests::fake_file_record_size;
}

// Rounds size up to the next multiple of attr_alignment.
size_t AlignAttr(size_t size) {
  return (size + attr_alignment - 1) / attr_alignment * attr_alignment;
}

// Builds a file record in the fake image layout, with attrs after its header.
std::vector<BYTE> MakeRecord(Data::FileRecordFlag flags, WORD seq,
                             ULONGLONG base_ref, std::span<const BYTE> attrs) {
  std::vector<BYTE> record(NtfsBrowserTests::fake_file_record_size, 0);

  NtfsBrowserTests::EditFileRecordHeader(
      record, [&](Data::FileRecordHeader& header) {
        header.magic = Data::FileRecordHeader::file_record_magic;
        header.offset_of_us = us_offset;
        header.size_of_us = 3;
        header.offset_of_attr = attr_offset;
        header.flags = flags;
        header.seq_no = seq;
        header.ref_to_base = base_ref;
        header.real_size = static_cast<DWORD>(attr_offset + attrs.size() + 4);
        header.alloc_size = NtfsBrowserTests::fake_file_record_size;
      });

  if (!attrs.empty()) {
    std::memcpy(&record.at(attr_offset), attrs.data(), attrs.size());
  }
  const auto end_marker = static_cast<DWORD>(Attr::Type::All);
  std::memcpy(&record.at(attr_offset + attrs.size()), &end_marker,
              sizeof(end_marker));
  return record;
}

// Resident attribute of the given type, holding body.
std::vector<BYTE> ResidentAttr(Attr::Type type, std::span<const BYTE> body) {
  const size_t header_size = sizeof(Data::HeaderResident);
  std::vector<BYTE> attr(AlignAttr(header_size + body.size()), 0);

  Data::HeaderResident header{};
  header.header.type = type;
  header.header.total_size = static_cast<DWORD>(attr.size());
  header.attr_size = static_cast<DWORD>(body.size());
  header.attr_offset = static_cast<WORD>(header_size);
  std::memcpy(attr.data(), &header, header_size);
  if (!body.empty()) {
    std::memcpy(attr.data() + header_size, body.data(), body.size());
  }
  return attr;
}

// Non-resident attribute of the given type, mapping one cluster at bitmap_lcn.
std::vector<BYTE> NonResidentAttr(Attr::Type type, ULONGLONG real_size) {
  const std::array<BYTE, 4> runs{run_header_one_byte_each, run_one_cluster,
                                 static_cast<BYTE>(bitmap_lcn), run_list_end};
  const size_t header_size = sizeof(Data::HeaderNonResident);
  std::vector<BYTE> attr(AlignAttr(header_size + runs.size()), 0);

  Data::HeaderNonResident header{};
  header.header.type = type;
  header.header.total_size = static_cast<DWORD>(attr.size());
  header.header.non_resident = 1;
  header.last_vcn = 0;
  header.data_run_offset = static_cast<WORD>(header_size);
  header.alloc_size = NtfsBrowserTests::fake_cluster_size;
  header.real_size = real_size;
  header.ini_size = real_size;
  std::memcpy(attr.data(), &header, header_size);
  std::memcpy(attr.data() + header_size, runs.data(), runs.size());
  return attr;
}

// Body of a $STANDARD_INFORMATION carrying permission.
std::vector<BYTE> StdInfoBody(Data::StdInfoPermission permission) {
  Data::StandardInformation info{};
  info.create_time = std_create_time;
  info.alter_time = std_alter_time;
  info.mft_time = std_mft_time;
  info.read_time = std_read_time;
  info.permission = permission;

  std::vector<BYTE> body(sizeof(info));
  std::memcpy(body.data(), &info, sizeof(info));
  return body;
}

// Body of a $FILE_NAME with the given flags, name and name length field.
std::vector<BYTE> FileNameBody(Data::FilenameFlag flags, std::wstring_view name,
                               BYTE name_length_field) {
  Data::Filename fixed{};
  fixed.parent_ref = file_name_parent_ref;
  fixed.create_time = std_create_time;
  fixed.alter_time = std_alter_time;
  fixed.mft_time = std_mft_time;
  fixed.read_time = std_read_time;
  fixed.alloc_size = file_name_alloc_size;
  fixed.real_size = file_name_real_size;
  fixed.flags = flags;
  fixed.name_length = name_length_field;
  fixed.name_space = Data::FilenameNamespace::Win32;

  const size_t name_offset = offsetof(Data::Filename, name);
  std::vector<BYTE> body(name_offset + name.size() * sizeof(WORD), 0);
  std::memcpy(body.data(), &fixed, name_offset);
  for (size_t i = 0; i < name.size(); i++) {
    const auto unit = static_cast<WORD>(name[i]);
    std::memcpy(body.data() + name_offset + i * sizeof(WORD), &unit,
                sizeof(unit));
  }
  return body;
}

// One $ATTRIBUTE_LIST entry: the record that holds an attribute of type.
std::vector<BYTE> AttrListEntry(Attr::Type type, ULONGLONG record_ref,
                                WORD record_seq) {
  Data::AttributeList entry{};
  entry.attr_type = type;
  entry.record_size = static_cast<WORD>(attr_list_entry_size);
  entry.base_ref.segment_number = record_ref;
  entry.base_ref.sequence_number = record_seq;

  std::vector<BYTE> bytes(attr_list_entry_size);
  std::memcpy(bytes.data(), &entry, attr_list_entry_size);
  return bytes;
}

// Fake volume image, with record written at idx and every other record as
// built.
std::vector<BYTE> ImageWith(ULONGLONG idx, const std::vector<BYTE>& record) {
  std::vector<BYTE> image = NtfsBrowserTests::BuildFakeNtfsImage();
  std::memcpy(&image.at(RecordOffset(idx)), record.data(), record.size());
  return image;
}

// Writes record at idx into image, in place.
void PutRecord(std::vector<BYTE>& image, ULONGLONG idx,
               const std::vector<BYTE>& record) {
  std::memcpy(&image.at(RecordOffset(idx)), record.data(), record.size());
}

// Resident wrapper class the strategy S uses.
template <Cache::Strategy S>
using ResidentFor =
    std::conditional_t<S == Cache::Strategy::NoCache, Attr::AttrResidentNoCache,
                       Attr::AttrResidentFullCache>;

// Reaches the protected sector size of a resident attribute.
template <Cache::Strategy S>
class SectorProbe final : public ResidentFor<S> {
 public:
  SectorProbe(const Attr::HeaderCommon& ahc,
              const NtfsBrowser::Io::FileRecord<S>& record)
      : ResidentFor<S>(ahc, record) {}

  [[nodiscard]] WORD Sector() const noexcept { return this->GetSectorSize(); }
};

// Puts the capture sink back when it leaves scope, after a test reconfigured
// the logger.
class RestoreCaptureSink final {
 public:
  RestoreCaptureSink() = default;
  RestoreCaptureSink(RestoreCaptureSink&&) = delete;
  RestoreCaptureSink(const RestoreCaptureSink&) = delete;
  RestoreCaptureSink& operator=(RestoreCaptureSink&&) = delete;
  RestoreCaptureSink& operator=(const RestoreCaptureSink&) = delete;

  ~RestoreCaptureSink() { NtfsBrowserTests::InstallCaptureSink(); }
};

// Record buffer whose update sequence array sits at us_offset, with the given
// sector-end words.
std::vector<BYTE> FixupBuffer(WORD first_end, WORD second_end) {
  std::vector<BYTE> buffer(NtfsBrowserTests::fake_file_record_size, 0);
  NtfsBrowserTests::EditFileRecordHeader(
      buffer, [](Data::FileRecordHeader& header) {
        header.magic = Data::FileRecordHeader::file_record_magic;
        header.offset_of_us = fixup_us_offset;
        header.size_of_us = 3;
      });

  constexpr WORD usn = 1;
  constexpr WORD first_original = 5;
  constexpr WORD second_original = 6;
  const auto put = [&buffer](size_t offset, WORD value) {
    std::memcpy(&buffer.at(offset), &value, sizeof(value));
  };
  put(fixup_us_offset, usn);
  put(fixup_us_offset + 2, first_original);
  put(fixup_us_offset + 4, second_original);
  put(first_sector_end, first_end);
  put(second_sector_end, second_end);
  return buffer;
}

// Builds a Record::Header over a buffer too small for a file record header.
void ConstructHeaderOver(std::span<const BYTE> buffer) {
  const NtfsBrowser::Record::HeaderImpl<Cache::Strategy::NoCache> header(
      buffer);
  static_cast<void>(header.buffer_size);
}

}  // namespace

TEMPLATE_TEST_CASE_SIG(
    "a resident bitmap answers from its own bytes and past its end",
    "[cov-attrs]", ((Cache::Strategy S), S), Cache::Strategy::NoCache,
    Cache::Strategy::FullCache) {
  const auto attr = ResidentAttr(Attr::Type::Bitmap, bitmap_bytes);
  const NtfsVolume<S> volume(
      std::make_unique<NtfsBrowserTests::MemoryDiskReader>(ImageWith(
          test_record_idx,
          MakeRecord(NtfsBrowser::Data::FileRecordFlag::InUse, 1, 0, attr))));
  REQUIRE(volume.IsVolumeOK());

  NtfsBrowser::Io::FileRecord<S> record(volume);
  REQUIRE(record.ParseFileRecord(test_record_idx));
  REQUIRE(record.ParseAttrs());
  const auto& bitmaps = record.GetAttr(Attr::Type::Bitmap);
  REQUIRE(bitmaps.size() == 1);
  auto& bitmap = static_cast<Attr::AttrBitmap<ResidentFor<S>, S>&>(*bitmaps[0]);

  CHECK_FALSE(bitmap.IsClusterFree(0));
  CHECK(bitmap.IsClusterFree(1));
  CHECK(bitmap.IsClusterFree(8));
  // Bit 128 lies past the two bitmap bytes: reported free.
  CHECK(bitmap.IsClusterFree(128));
}

TEMPLATE_TEST_CASE_SIG(
    "a resident bitmap with no bytes reports no free cluster", "[cov-attrs]",
    ((Cache::Strategy S), S), Cache::Strategy::NoCache,
    Cache::Strategy::FullCache) {
  const auto attr = ResidentAttr(Attr::Type::Bitmap, std::span<const BYTE>{});
  const NtfsVolume<S> volume(
      std::make_unique<NtfsBrowserTests::MemoryDiskReader>(ImageWith(
          test_record_idx,
          MakeRecord(NtfsBrowser::Data::FileRecordFlag::InUse, 1, 0, attr))));
  REQUIRE(volume.IsVolumeOK());

  NtfsBrowser::Io::FileRecord<S> record(volume);
  REQUIRE(record.ParseFileRecord(test_record_idx));
  REQUIRE(record.ParseAttrs());
  const auto& bitmaps = record.GetAttr(Attr::Type::Bitmap);
  REQUIRE(bitmaps.size() == 1);
  auto& bitmap = static_cast<Attr::AttrBitmap<ResidentFor<S>, S>&>(*bitmaps[0]);

  CHECK_FALSE(bitmap.IsClusterFree(0));
}

TEMPLATE_TEST_CASE_SIG(
    "a non-resident bitmap whose cluster read comes back short reports no free "
    "cluster",
    "[cov-attrs]", ((Cache::Strategy S), S), Cache::Strategy::NoCache,
    Cache::Strategy::FullCache) {
  const auto attr = NonResidentAttr(Attr::Type::Bitmap, short_bitmap_size);
  const NtfsVolume<S> volume(
      std::make_unique<NtfsBrowserTests::MemoryDiskReader>(ImageWith(
          test_record_idx,
          MakeRecord(NtfsBrowser::Data::FileRecordFlag::InUse, 1, 0, attr))));
  REQUIRE(volume.IsVolumeOK());

  NtfsBrowser::Io::FileRecord<S> record(volume);
  REQUIRE(record.ParseFileRecord(test_record_idx));
  REQUIRE(record.ParseAttrs());
  const auto& bitmaps = record.GetAttr(Attr::Type::Bitmap);
  REQUIRE(bitmaps.size() == 1);
  auto& bitmap =
      static_cast<Attr::AttrBitmap<Attr::AttrNonResident<S>, S>&>(*bitmaps[0]);

  CHECK_FALSE(bitmap.IsClusterFree(0));
  CHECK_FALSE(bitmap.IsClusterFree(0));
}

TEMPLATE_TEST_CASE_SIG(
    "the standard information permissions and times reach the file record",
    "[cov-attrs]", ((Cache::Strategy S), S), Cache::Strategy::NoCache,
    Cache::Strategy::FullCache) {
  const auto perms =
      Data::StdInfoPermission::Device | Data::StdInfoPermission::Temp |
      Data::StdInfoPermission::Offline | Data::StdInfoPermission::Nci |
      Data::StdInfoPermission::Reparse;
  const auto attr =
      ResidentAttr(Attr::Type::StandardInformation, StdInfoBody(perms));
  const NtfsVolume<S> volume(
      std::make_unique<NtfsBrowserTests::MemoryDiskReader>(ImageWith(
          test_record_idx,
          MakeRecord(NtfsBrowser::Data::FileRecordFlag::InUse, 1, 0, attr))));
  REQUIRE(volume.IsVolumeOK());

  NtfsBrowser::Io::FileRecord<S> record(volume);
  REQUIRE(record.ParseFileRecord(test_record_idx));
  REQUIRE(record.ParseAttrs());

  CHECK(record.IsDevice());
  CHECK(record.IsTemporary());
  CHECK(record.IsOffline());
  CHECK(record.IsNotContentIndexed());
  CHECK(record.IsReparsePoint());
  CHECK_FALSE(record.IsNormal());

  FILETIME write{};
  FILETIME create{};
  FILETIME access{};
  FILETIME change{};
  record.GetFileTime(&write, &create, &access, &change);
#ifndef _WIN32
  // Off Windows the times stay UTC, so the low and high words come back as
  // stored.
  CHECK(write.dwLowDateTime == static_cast<DWORD>(std_alter_time));
  CHECK(write.dwHighDateTime == static_cast<DWORD>(std_alter_time >> 32U));
#endif
  record.GetFileTime(nullptr, &create, nullptr, nullptr);
  record.GetFileTime(&write, nullptr, nullptr, nullptr);
}

TEMPLATE_TEST_CASE_SIG(
    "a standard information attribute reads its name, sector size and an empty "
    "body",
    "[cov-attrs]", ((Cache::Strategy S), S), Cache::Strategy::NoCache,
    Cache::Strategy::FullCache) {
  const auto attr = ResidentAttr(Attr::Type::StandardInformation,
                                 StdInfoBody(Data::StdInfoPermission::Normal));
  const NtfsVolume<S> volume(
      std::make_unique<NtfsBrowserTests::MemoryDiskReader>(ImageWith(
          test_record_idx,
          MakeRecord(NtfsBrowser::Data::FileRecordFlag::InUse, 1, 0, attr))));
  REQUIRE(volume.IsVolumeOK());

  NtfsBrowser::Io::FileRecord<S> record(volume);
  REQUIRE(record.ParseFileRecord(test_record_idx));
  REQUIRE(record.ParseAttrs());
  const auto& infos = record.GetAttr(Attr::Type::StandardInformation);
  REQUIRE(infos.size() == 1);

  const SectorProbe<S> probe(infos[0]->GetAttrHeader(), record);
  CHECK(probe.Sector() == NtfsBrowserTests::fake_bytes_per_sector);
  CHECK(probe.GetAttrName().empty());

  std::span<BYTE> nothing;
  const auto read = probe.ReadData(0, nothing);
  REQUIRE(read.has_value());
  CHECK(*read == 0);
}

TEMPLATE_TEST_CASE_SIG(
    "a file name attribute exposes its flags, name and times", "[cov-attrs]",
    ((Cache::Strategy S), S), Cache::Strategy::NoCache,
    Cache::Strategy::FullCache) {
  const auto flags = Data::FilenameFlag::Directory |
                     Data::FilenameFlag::ReadOnly | Data::FilenameFlag::Hidden |
                     Data::FilenameFlag::System | Data::FilenameFlag::Archive |
                     Data::FilenameFlag::Compressed |
                     Data::FilenameFlag::Encrypted | Data::FilenameFlag::Sparse;
  const auto attr = ResidentAttr(
      Attr::Type::FileName, FileNameBody(flags, L"Probe", file_name_length));
  const NtfsVolume<S> volume(
      std::make_unique<NtfsBrowserTests::MemoryDiskReader>(ImageWith(
          test_record_idx,
          MakeRecord(NtfsBrowser::Data::FileRecordFlag::InUse, 1, 0, attr))));
  REQUIRE(volume.IsVolumeOK());

  NtfsBrowser::Io::FileRecord<S> record(volume);
  REQUIRE(record.ParseFileRecord(test_record_idx));
  REQUIRE(record.ParseAttrs());
  const auto& names = record.GetAttr(Attr::Type::FileName);
  REQUIRE(names.size() == 1);
  const Filename& name =
      static_cast<const Attr::AttrFileName<ResidentFor<S>, S>&>(*names[0]);

  CHECK(name.GetFilePermission() == flags);
  CHECK(name.IsReadOnly());
  CHECK(name.IsHidden());
  CHECK(name.IsSystem());
  CHECK(name.IsArchive());
  CHECK(name.IsDirectory());
  CHECK(name.IsCompressed());
  CHECK(name.IsEncrypted());
  CHECK(name.IsSparse());
  CHECK(name.HasName());
  CHECK(name.IsWin32Name());
  CHECK(name.GetFilename() == L"Probe");

  FILETIME write{};
  FILETIME create{};
  FILETIME access{};
  FILETIME change{};
  name.GetFileTime(&write, &create, &access, &change);
  name.GetFileTime(nullptr, nullptr, nullptr);
}

TEST_CASE("a file name with no attribute behind it reports nothing",
          "[cov-attrs]") {
  const Filename empty;

  CHECK(empty.GetFilePermission() == Data::FilenameFlag::None);
  CHECK_FALSE(empty.IsReadOnly());
  CHECK_FALSE(empty.IsHidden());
  CHECK_FALSE(empty.IsSystem());
  CHECK_FALSE(empty.IsArchive());
  CHECK_FALSE(empty.IsDirectory());
  CHECK_FALSE(empty.IsCompressed());
  CHECK_FALSE(empty.IsEncrypted());
  CHECK_FALSE(empty.IsSparse());
  CHECK_FALSE(empty.HasName());
  CHECK_FALSE(empty.IsWin32Name());

  FILETIME write{};
  empty.GetFileTime(&write, nullptr, nullptr, nullptr);
}

TEMPLATE_TEST_CASE_SIG(
    "a file name shorter than its fixed header, or naming past its body, is "
    "refused",
    "[cov-attrs]", ((Cache::Strategy S), S), Cache::Strategy::NoCache,
    Cache::Strategy::FullCache) {
  const std::vector<BYTE> short_body(short_file_name_body, 0);
  const auto short_attr = ResidentAttr(Attr::Type::FileName, short_body);
  const NtfsVolume<S> short_volume(
      std::make_unique<NtfsBrowserTests::MemoryDiskReader>(ImageWith(
          test_record_idx, MakeRecord(NtfsBrowser::Data::FileRecordFlag::InUse,
                                      1, 0, short_attr))));
  REQUIRE(short_volume.IsVolumeOK());

  NtfsBrowser::Io::FileRecord<S> short_record(short_volume);
  REQUIRE(short_record.ParseFileRecord(test_record_idx));
  static_cast<void>(short_record.ParseAttrs());
  CHECK(short_record.GetAttr(Attr::Type::FileName).empty());

  const auto long_attr = ResidentAttr(
      Attr::Type::FileName,
      FileNameBody(Data::FilenameFlag::None, L"P", oversized_name_length));
  const NtfsVolume<S> long_volume(
      std::make_unique<NtfsBrowserTests::MemoryDiskReader>(ImageWith(
          test_record_idx, MakeRecord(NtfsBrowser::Data::FileRecordFlag::InUse,
                                      1, 0, long_attr))));
  REQUIRE(long_volume.IsVolumeOK());

  NtfsBrowser::Io::FileRecord<S> long_record(long_volume);
  REQUIRE(long_record.ParseFileRecord(test_record_idx));
  static_cast<void>(long_record.ParseAttrs());
  CHECK(long_record.GetAttr(Attr::Type::FileName).empty());
}

TEMPLATE_TEST_CASE_SIG(
    "an attribute list entry naming an invalid attribute type is refused",
    "[cov-attrs]", ((Cache::Strategy S), S), Cache::Strategy::NoCache,
    Cache::Strategy::FullCache) {
  auto entry =
      AttrListEntry(Attr::Type::StandardInformation, zero_record_idx, 0);
  const auto bad_type = static_cast<DWORD>(attr_list_type_invalid);
  std::memcpy(entry.data(), &bad_type, sizeof(bad_type));
  const auto attr = ResidentAttr(Attr::Type::AttributeList, entry);
  const NtfsVolume<S> volume(
      std::make_unique<NtfsBrowserTests::MemoryDiskReader>(ImageWith(
          test_record_idx,
          MakeRecord(NtfsBrowser::Data::FileRecordFlag::InUse, 1, 0, attr))));
  REQUIRE(volume.IsVolumeOK());

  NtfsBrowser::Io::FileRecord<S> record(volume);
  REQUIRE(record.ParseFileRecord(test_record_idx));
  CHECK_FALSE(record.ParseAttrs());
}

TEMPLATE_TEST_CASE_SIG(
    "an attribute list naming a record that cannot be parsed is refused",
    "[cov-attrs]", ((Cache::Strategy S), S), Cache::Strategy::NoCache,
    Cache::Strategy::FullCache) {
  const auto entry =
      AttrListEntry(Attr::Type::StandardInformation, zero_record_idx, 0);
  const auto attr = ResidentAttr(Attr::Type::AttributeList, entry);
  const NtfsVolume<S> volume(
      std::make_unique<NtfsBrowserTests::MemoryDiskReader>(ImageWith(
          test_record_idx,
          MakeRecord(NtfsBrowser::Data::FileRecordFlag::InUse, 1, 0, attr))));
  REQUIRE(volume.IsVolumeOK());

  NtfsBrowser::Io::FileRecord<S> record(volume);
  REQUIRE(record.ParseFileRecord(test_record_idx));
  CHECK_FALSE(record.ParseAttrs());
}

TEMPLATE_TEST_CASE_SIG(
    "an attribute list whose extension record holds an undersized attribute is "
    "refused",
    "[cov-attrs]", ((Cache::Strategy S), S), Cache::Strategy::NoCache,
    Cache::Strategy::FullCache) {
  // Type $STANDARD_INFORMATION, total size 8: below any attribute header.
  const std::array<BYTE, 8> undersized{0x10, 0x00, 0x00, 0x00,
                                       0x08, 0x00, 0x00, 0x00};
  const auto ext = MakeRecord(NtfsBrowser::Data::FileRecordFlag::InUse, 1,
                              test_record_idx, undersized);
  const auto entry =
      AttrListEntry(Attr::Type::StandardInformation, broken_ext_record_idx, 1);
  const auto attr = ResidentAttr(Attr::Type::AttributeList, entry);
  auto image = ImageWith(
      test_record_idx,
      MakeRecord(NtfsBrowser::Data::FileRecordFlag::InUse, 1, 0, attr));
  PutRecord(image, broken_ext_record_idx, ext);

  const NtfsVolume<S> volume(
      std::make_unique<NtfsBrowserTests::MemoryDiskReader>(std::move(image)));
  REQUIRE(volume.IsVolumeOK());

  NtfsBrowser::Io::FileRecord<S> record(volume);
  REQUIRE(record.ParseFileRecord(test_record_idx));
  CHECK_FALSE(record.ParseAttrs());
}

TEST_CASE("a file record buffer shorter than its header is refused",
          "[cov-attrs]") {
  const std::vector<BYTE> tiny(16, 0);
  REQUIRE_THROWS_AS(ConstructHeaderOver(tiny), std::runtime_error);
}

TEST_CASE(
    "a sector end that does not carry the update sequence fails the fixup",
    "[cov-attrs]") {
  // Second sector end matches; the first carries neither the USN nor its
  // original.
  auto buffer = FixupBuffer(2, 1);
  NtfsBrowser::Record::HeaderImpl<Cache::Strategy::NoCache> header(buffer);
  CHECK_FALSE(header.PatchUS());
}

TEST_CASE("a sector end that carries the update sequence is restored",
          "[cov-attrs]") {
  auto buffer = FixupBuffer(1, 1);
  NtfsBrowser::Record::HeaderImpl<Cache::Strategy::NoCache> header(buffer);
  CHECK(header.PatchUS());
  CHECK(buffer[first_sector_end] == 5);
  CHECK(buffer[second_sector_end] == 6);
}

TEST_CASE("a code point past U+10FFFF compares as the replacement character",
          "[cov-attrs]") {
  if constexpr (sizeof(wchar_t) > sizeof(char16_t)) {
    const std::wstring beyond(1, beyond_unicode);
    CHECK(NtfsBrowser::UpCase::Table::BuiltIn().Compare(beyond, L"a") > 0);
  } else {
    SKIP("wchar_t is 16 bits here: no code point past the BMP exists");
  }
}

TEST_CASE("an exception message loses its trailing line breaks",
          "[cov-attrs]") {
  static_cast<void>(NtfsBrowserTests::TakeCapturedLog());

  Log::Exception(std::runtime_error("parse failed\r\n"));

  const std::string text = NtfsBrowserTests::TakeCapturedLog();
  const std::string_view message = "parse failed";
  const size_t message_start = text.find(message);
  REQUIRE(message_start != std::string::npos);
  // Only the sink's own line ending follows the message: one newline.
  const std::string tail = text.substr(message_start + message.size());
  CHECK(std::ranges::count(tail, '\n') == 1);
}

TEST_CASE("a message that cannot be formatted is logged as such",
          "[cov-attrs]") {
  static_cast<void>(NtfsBrowserTests::TakeCapturedLog());

  // A negative dynamic width makes std::format throw.
  Log::Info("width {:{}}", "x", -1);

  const std::string text = NtfsBrowserTests::TakeCapturedLog();
  CHECK(text.find("Log message could not be formatted") != std::string::npos);
}

TEST_CASE("a formatter set on the library logger reaches its console sink",
          "[cov-attrs]") {
  const RestoreCaptureSink restore;

  Log::Config config;
  config.console_level = Log::Level::Error;
  config.file_level = Log::Level::Off;
  REQUIRE(Log::Configure(config));

  const auto logger = spdlog::get(std::string(Log::logger_name));
  REQUIRE(logger != nullptr);
  logger->set_formatter(std::make_unique<spdlog::pattern_formatter>("%v"));

  // Below the console's error level, so nothing reaches the streams.
  Log::Info("formatter probe");
}

TEST_CASE(
    "ParseOption refuses a path on the console target and a foreign prefix",
    "[cov-attrs]") {
  Log::Config config;

  CHECK_FALSE(Log::ParseOption("--log=console:info:path.log", config));
  CHECK_FALSE(Log::ParseOption("-log=console:info", config));
  CHECK(config.console_level == Log::Level::Warn);
  CHECK(config.file_level == Log::Level::Off);
}
