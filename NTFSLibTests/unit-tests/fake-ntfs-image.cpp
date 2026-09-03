#include "fake-ntfs-image.h"

#include <array>
#include <cstring>
#include <fstream>
#include <random>
#include <vector>

#include <windows.h>

#include <ntfs-browser/data/attr-header-common.h>
#include <ntfs-browser/data/attr-type.h>
#include <ntfs-browser/data/file-record-header.h>
#include <ntfs-browser/flag/file-record.h>
#include <ntfs-browser/mft-idx.h>

#include "attr/attribute-list.h"
#include "attr/filename.h"
#include "attr/header-non-resident.h"
#include "attr/header-resident.h"
#include "attr/index-root.h"
#include "attr/volume-information.h"
#include "data/index-entry.h"
#include "data/ntfs-bpb.h"
#include "flag/filename-namespace.h"
#include "flag/filename.h"
#include "flag/index-entry.h"

namespace NtfsBrowserTests
{

using NtfsBrowser::AttrType;
using NtfsBrowser::FileRecordHeader;
using NtfsBrowser::kFileRecordMagic;
using NtfsBrowser::Enum::MftIdx;

namespace
{

// One sector per file record: the Update Sequence fixup only has a single
// slot then, at the record's last 2 bytes.
constexpr WORD kBytesPerSector = kFakeFileRecordSize;
constexpr BYTE kSectorsPerCluster = 1;
constexpr DWORD kClusterSize = kBytesPerSector * kSectorsPerCluster;
constexpr ULONGLONG kMftLcn = 1;  // $MFT starts right after the boot sector
constexpr WORD kAttrOffset =
    48;  // right after FileRecordHeader::Data's header fields

// Point offset_of_us at the fixup slot itself (last 2 bytes of the record):
// FileRecordHeader's ctor then "captures" that same slot as the value to
// restore, so PatchUS() trivially succeeds without a real fixup array.
constexpr WORD kOffsetOfUs = kFakeFileRecordSize - 4;

static_assert(kAttrOffset + sizeof(NtfsBrowser::Attr::HeaderNonResident) + 8 <
                  kOffsetOfUs,
              "attribute data must not reach into the fixup slot");

using FakeRecord = std::array<BYTE, kFakeFileRecordSize>;

FakeRecord MakeRecordHeader(WORD offsetOfAttr,
                            NtfsBrowser::Flag::FileRecord flags)
{
  FakeRecord record{};

  auto& header = *reinterpret_cast<FileRecordHeader::Data*>(record.data());
  header.magic = kFileRecordMagic;
  header.offset_of_us = kOffsetOfUs;
  header.size_of_us = 2;
  header.offset_of_attr = offsetOfAttr;
  header.flags = flags;

  return record;
}

void WriteEndOfAttributesMarker(FakeRecord& record, DWORD offset)
{
  const DWORD marker = static_cast<DWORD>(AttrType::ALL);
  std::memcpy(&record[offset], &marker, sizeof(marker));
}

// $MFT (#0): a single non-resident DATA attribute whose real_size encodes
// kSentinelRecordCount file records, with an empty (immediately-terminated)
// data run - GetRecordsCount() only reads real_size, never the data run.
FakeRecord MakeMftRecord()
{
  FakeRecord record =
      MakeRecordHeader(kAttrOffset, NtfsBrowser::Flag::FileRecord::INUSE);

  auto& attr = *reinterpret_cast<NtfsBrowser::Attr::HeaderNonResident*>(
      &record[kAttrOffset]);
  attr.header.type = AttrType::DATA;
  attr.header.non_resident = 1;
  attr.header.name_length = 0;
  attr.header.flags = 0;
  attr.header.id = 0;
  attr.start_vcn = 0;
  attr.last_vcn = 0;
  attr.data_run_offset = static_cast<WORD>(sizeof(attr));
  attr.comp_unit_size = 0;
  attr.real_size = kSentinelRecordCount * kFakeFileRecordSize;
  attr.alloc_size = attr.real_size;
  attr.ini_size = attr.real_size;
  attr.header.total_size = static_cast<DWORD>(sizeof(attr)) + 8;

  // Data run: a single 0x00 byte terminates the run list immediately.
  record[kAttrOffset + sizeof(attr)] = 0x00;

  WriteEndOfAttributesMarker(record, kAttrOffset + attr.header.total_size);
  return record;
}

// $Volume (#3): a resident VOLUME_INFORMATION attribute reporting NTFS 3.1,
// the minimum NtfsVolume<S>::Init() requires to accept the volume.
FakeRecord MakeVolumeRecord()
{
  FakeRecord record =
      MakeRecordHeader(kAttrOffset, NtfsBrowser::Flag::FileRecord::INUSE);

  auto& attr = *reinterpret_cast<NtfsBrowser::Attr::HeaderResident*>(
      &record[kAttrOffset]);
  attr.header.type = AttrType::VOLUME_INFORMATION;
  attr.header.non_resident = 0;
  attr.header.name_length = 0;
  attr.header.flags = 0;
  attr.header.id = 0;
  attr.attr_size = sizeof(NtfsBrowser::Attr::VolumeInformation);
  attr.attr_offset = static_cast<WORD>(sizeof(attr));
  attr.header.total_size = static_cast<DWORD>(sizeof(attr)) + attr.attr_size;

  auto& volInfo = *reinterpret_cast<NtfsBrowser::Attr::VolumeInformation*>(
      &record[kAttrOffset + attr.attr_offset]);
  volInfo.major_version = 3;
  volInfo.minor_version = 1;

  WriteEndOfAttributesMarker(record, kAttrOffset + attr.header.total_size);
  return record;
}

// Root directory (#5): a bare, valid file record. ParseFileRecord() never
// looks at attributes, so this is enough to reproduce a second ParseFileRecord
// call taking the same "direct disk allocation" path as $MFT and $Volume.
FakeRecord MakeRootRecord()
{
  return MakeRecordHeader(kAttrOffset, NtfsBrowser::Flag::FileRecord::INUSE |
                                           NtfsBrowser::Flag::FileRecord::DIR);
}

// Directory (#kAttributeListDirIdx): its only attribute is a resident
// $ATTRIBUTE_LIST with a single record relocating $INDEX_ROOT to
// #kIndexExtensionIdx - reproduces a directory whose base MFT record no
// longer holds $INDEX_ROOT/$INDEX_ALLOCATION directly, as real directories
// like C:\Windows do once they outgrow the base record.
FakeRecord MakeAttributeListOnlyDirRecord()
{
  FakeRecord record =
      MakeRecordHeader(kAttrOffset, NtfsBrowser::Flag::FileRecord::INUSE |
                                        NtfsBrowser::Flag::FileRecord::DIR);

  auto& attr = *reinterpret_cast<NtfsBrowser::Attr::HeaderResident*>(
      &record[kAttrOffset]);
  attr.header.type = AttrType::ATTRIBUTE_LIST;
  attr.header.non_resident = 0;
  attr.header.name_length = 0;
  attr.header.flags = 0;
  attr.header.id = 0;
  attr.attr_size = sizeof(NtfsBrowser::Attr::AttributeList);
  attr.attr_offset = static_cast<WORD>(sizeof(attr));
  attr.header.total_size = static_cast<DWORD>(sizeof(attr)) + attr.attr_size;

  auto& alEntry = *reinterpret_cast<NtfsBrowser::Attr::AttributeList*>(
      &record[kAttrOffset + attr.attr_offset]);
  alEntry.attr_type = AttrType::INDEX_ROOT;
  alEntry.record_size = static_cast<WORD>(sizeof(alEntry));
  alEntry.name_length = 0;
  alEntry.name_offset = 0;
  alEntry.start_vcn = 0;
  alEntry.base_ref.segment_number = kIndexExtensionIdx;
  alEntry.base_ref.sequence_number = 0;
  alEntry.attr_id = 0;

  WriteEndOfAttributesMarker(record, kAttrOffset + attr.header.total_size);
  return record;
}

// Extension record (#kIndexExtensionIdx): holds the resident $INDEX_ROOT
// that #kAttributeListDirIdx's $ATTRIBUTE_LIST points to - a single "Foo"
// filename entry (file reference 20), plus the terminating nameless entry.
FakeRecord MakeIndexRootExtensionRecord()
{
  FakeRecord record =
      MakeRecordHeader(kAttrOffset, NtfsBrowser::Flag::FileRecord::INUSE);

  auto& attr = *reinterpret_cast<NtfsBrowser::Attr::HeaderResident*>(
      &record[kAttrOffset]);
  attr.header.type = AttrType::INDEX_ROOT;
  attr.header.non_resident = 0;
  attr.header.name_length = 0;
  attr.header.flags = 0;
  attr.header.id = 0;
  attr.attr_offset = static_cast<WORD>(sizeof(attr));

  BYTE* body = &record[kAttrOffset + attr.attr_offset];
  auto& root = *reinterpret_cast<NtfsBrowser::Attr::IndexRoot*>(body);
  root.attr_type = AttrType::FILE_NAME;
  root.coll_rule = 0;
  root.ib_size = kFakeFileRecordSize;
  root.clusters_per_ib = 1;
  root.entry_offset =
      static_cast<DWORD>((body + sizeof(NtfsBrowser::Attr::IndexRoot)) -
                         reinterpret_cast<BYTE*>(&root.entry_offset));

  // Entry 1: "Foo", a regular (non-directory) file, reference 20.
  auto& e1 = *reinterpret_cast<NtfsBrowser::Data::IndexEntry*>(
      body + sizeof(NtfsBrowser::Attr::IndexRoot));
  e1.mft_index = 20;
  e1.mft_sn = 1;

  auto& fn = *reinterpret_cast<NtfsBrowser::Attr::Filename*>(&e1.stream);
  fn.parent_ref = kAttributeListDirIdx;
  fn.flags = NtfsBrowser::Flag::Filename::NONE;
  fn.name_length = 3;
  fn.name_space = NtfsBrowser::Flag::FilenameNamespace::WIN_32;
  constexpr wchar_t kFooName[] = L"Foo";
  for (BYTE i = 0; i < fn.name_length; i++)
  {
    fn.name[i] = static_cast<WORD>(kFooName[i]);
  }

  e1.stream_size = static_cast<WORD>(reinterpret_cast<BYTE*>(&fn.name[3]) -
                                     reinterpret_cast<BYTE*>(&fn));
  e1.size = static_cast<WORD>(reinterpret_cast<BYTE*>(&e1.stream) -
                              reinterpret_cast<BYTE*>(&e1) + e1.stream_size);

  // Entry 2: the terminating entry - no name, no sub-node.
  auto& e2 = *reinterpret_cast<NtfsBrowser::Data::IndexEntry*>(
      body + sizeof(NtfsBrowser::Attr::IndexRoot) + e1.size);
  e2.flags = NtfsBrowser::Flag::IndexEntry::LAST;
  e2.stream_size = 0;
  e2.size = static_cast<WORD>(reinterpret_cast<BYTE*>(&e2.stream) -
                              reinterpret_cast<BYTE*>(&e2));

  root.total_entry_size = static_cast<DWORD>(e1.size) + e2.size;
  root.alloc_entry_size = root.total_entry_size;
  root.flags = 0;

  attr.attr_size = static_cast<DWORD>(sizeof(NtfsBrowser::Attr::IndexRoot)) +
                   e1.size + e2.size;
  attr.header.total_size = static_cast<DWORD>(sizeof(attr)) + attr.attr_size;

  WriteEndOfAttributesMarker(record, kAttrOffset + attr.header.total_size);
  return record;
}

// Record (#kUndersizedAttrRecordIdx): a single REPARSE_POINT attribute whose
// declared total_size (17) is smaller than sizeof(Attr::HeaderResident)
// (24). REPARSE_POINT is deliberately not one of the types FileRecord's
// AllocAttr special-cases: it falls through to the "default" branch, which
// already checks ahc.non_resident, keeping this fixture focused on F1's
// actual defect - FileRecord::ParseAttrs() never checks total_size against
// the header size before AttrResident{No,Full}Cache's ctor reinterprets the
// raw bytes as Attr::HeaderResident and reads attr_size (relative offset
// 16-19) / attr_offset (20-21) - fields that partly or wholly lie past this
// attribute's declared 17-byte extent.
//
// attr_size/attr_offset (absolute bytes 64-69) are left at FakeRecord's
// zero-initialized default, past total_size's declared end (absolute
// 48-64) - the point being that the current code reads them at all. Byte 72
// (the high byte of the *next* AttrHeaderCommon::total_size, reinterpreted
// starting right after this 17-byte attribute) is set to a large sentinel
// so ParseAttrs' own "does this next attribute fit in 1024 bytes" check
// fails and the loop exits cleanly, instead of misparsing further bytes -
// keeping the fixture's outcome deterministic instead of depending on
// whatever garbage a real disk would leave there.
FakeRecord MakeUndersizedResidentAttrRecord()
{
  FakeRecord record =
      MakeRecordHeader(kAttrOffset, NtfsBrowser::Flag::FileRecord::INUSE);

  constexpr DWORD kUndersizedTotalSize = 17;
  static_assert(
      kUndersizedTotalSize < sizeof(NtfsBrowser::Attr::HeaderResident),
      "total_size must be smaller than a resident attribute header to "
      "reproduce F1");

  auto& attr = *reinterpret_cast<NtfsBrowser::Attr::HeaderResident*>(
      &record[kAttrOffset]);
  attr.header.type = AttrType::REPARSE_POINT;
  attr.header.non_resident = 0;
  attr.header.name_length = 0;
  attr.header.flags = 0;
  attr.header.id = 0;
  attr.header.total_size = kUndersizedTotalSize;

  record[kAttrOffset + sizeof(NtfsBrowser::Attr::HeaderResident)] = 0xFF;

  return record;
}

}  // namespace

std::vector<BYTE> BuildFakeNtfsImage()
{
  NtfsBrowser::Data::NtfsBpb bpb{};
  std::memcpy(bpb.signature, NTFS_SIGNATURE, sizeof(bpb.signature));
  bpb.bytes_per_sector = kBytesPerSector;
  bpb.sectors_per_cluster = kSectorsPerCluster;
  bpb.lcn_mft = kMftLcn;
  bpb.clusters_per_file_record = 1;  // positive sz -> cluster_size * 1
  bpb.clusters_per_index_block = 1;  // unused by these tests
  bpb.x_aa = 0xAA;
  bpb.x_55 = 0x55;

  const DWORD mftAddr = static_cast<DWORD>(kMftLcn) * kClusterSize;
  const size_t recordsEnd =
      mftAddr + static_cast<size_t>(kFakeFileRecordSize) *
                    (static_cast<size_t>(MftIdx::ROOT) + 1);
  // FULL_CACHE's FileReader::Read() always pulls in a full 64KB block from
  // FILE_BEGIN, regardless of the length actually requested (used for the
  // boot sector read) - the image must be at least that large.
  constexpr size_t kFullCacheReadBlockSize = 64 * 1024;
  const size_t imageSize = max(recordsEnd, kFullCacheReadBlockSize);
  std::vector<BYTE> image(imageSize, 0);
  std::memcpy(image.data(), &bpb, sizeof(bpb));

  const auto putRecord = [&](MftIdx idx, const FakeRecord& record)
  {
    const size_t offset = mftAddr + static_cast<size_t>(kFakeFileRecordSize) *
                                        static_cast<size_t>(idx);
    std::memcpy(image.data() + offset, record.data(), record.size());
  };
  putRecord(MftIdx::MFT, MakeMftRecord());
  putRecord(MftIdx::VOLUME, MakeVolumeRecord());
  putRecord(MftIdx::ROOT, MakeRootRecord());

  return image;
}

std::vector<BYTE> BuildFakeNtfsImageWithAttributeListDirectory()
{
  std::vector<BYTE> image = BuildFakeNtfsImage();

  const DWORD mftAddr = static_cast<DWORD>(kMftLcn) * kClusterSize;
  const auto putRecord = [&](ULONGLONG idx, const FakeRecord& record)
  {
    const size_t offset = mftAddr + static_cast<size_t>(kFakeFileRecordSize) *
                                        static_cast<size_t>(idx);
    std::memcpy(image.data() + offset, record.data(), record.size());
  };

  putRecord(kAttributeListDirIdx, MakeAttributeListOnlyDirRecord());
  putRecord(kIndexExtensionIdx, MakeIndexRootExtensionRecord());

  return image;
}

std::vector<BYTE> BuildFakeNtfsImageWithUndersizedAttribute()
{
  std::vector<BYTE> image = BuildFakeNtfsImage();

  const DWORD mftAddr = static_cast<DWORD>(kMftLcn) * kClusterSize;
  const size_t offset =
      mftAddr + static_cast<size_t>(kFakeFileRecordSize) *
                    static_cast<size_t>(kUndersizedAttrRecordIdx);
  const FakeRecord record = MakeUndersizedResidentAttrRecord();
  std::memcpy(image.data() + offset, record.data(), record.size());

  return image;
}

std::filesystem::path WriteFakeNtfsImage()
{
  const std::vector<BYTE> image = BuildFakeNtfsImage();

  std::random_device rd;
  const std::filesystem::path path =
      std::filesystem::temp_directory_path() /
      (L"ntfsbrowser-fake-volume-" + std::to_wstring(rd()) + L".img");

  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  out.write(reinterpret_cast<const char*>(image.data()),
            static_cast<std::streamsize>(image.size()));

  return path;
}

}  // namespace NtfsBrowserTests
