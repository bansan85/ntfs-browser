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

// One sector per record, so the USN fixup needs only one slot.
constexpr WORD kBytesPerSector = kFakeFileRecordSize;
// One sector per cluster keeps addressing simple for the fake image.
constexpr BYTE kSectorsPerCluster = 1;
constexpr DWORD kClusterSize = kBytesPerSector * kSectorsPerCluster;
// $MFT sits in the first cluster after the boot sector.
constexpr ULONGLONG kMftLcn = 1;
// Right after FileRecordHeader::Data's fixed header fields.
constexpr WORD kAttrOffset = 48;

// Points the fixup slot at the record's own last 4 bytes, so PatchUS()
// succeeds without a real fixup array.
constexpr WORD kOffsetOfUs = kFakeFileRecordSize - 4;

static_assert(kAttrOffset + sizeof(NtfsBrowser::Attr::HeaderNonResident) + 8 <
                  kOffsetOfUs,
              "attribute data must not reach into the fixup slot");

using FakeRecord = std::array<BYTE, kFakeFileRecordSize>;

// Builds a bare file-record header with the given attribute offset and
// flags.
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

// Writes the AttrType::ALL end-of-attributes marker at offset.
void WriteEndOfAttributesMarker(FakeRecord& record, DWORD offset)
{
  const DWORD marker = static_cast<DWORD>(AttrType::ALL);
  std::memcpy(&record[offset], &marker, sizeof(marker));
}

// Builds a fake $MFT record: one non-resident DATA attribute whose
// real_size reports kSentinelRecordCount fake records, with an empty
// data run (GetRecordsCount() only reads real_size).
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

  record[kAttrOffset + sizeof(attr)] = 0x00;

  WriteEndOfAttributesMarker(record, kAttrOffset + attr.header.total_size);
  return record;
}

// Builds a fake $Volume record: a resident VOLUME_INFORMATION attribute
// reporting NTFS 3.1, the minimum NtfsVolume<S>::Init() accepts.
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

// Builds a bare root-directory record; ParseFileRecord() never looks at
// attributes, so an empty one is enough to exercise a second read.
FakeRecord MakeRootRecord()
{
  return MakeRecordHeader(kAttrOffset, NtfsBrowser::Flag::FileRecord::INUSE |
                                           NtfsBrowser::Flag::FileRecord::DIR);
}

// Directory whose only attribute is a resident $ATTRIBUTE_LIST relocating
// $INDEX_ROOT to the extension record kIndexExtensionIdx, reproducing a
// directory that outgrew its base record (eg. C:\Windows).
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

// Extension record holding the resident $INDEX_ROOT that
// kAttributeListDirIdx's $ATTRIBUTE_LIST points to: a single "Foo" entry
// (file reference 20), plus the terminating nameless entry.
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

}

std::vector<BYTE> BuildFakeNtfsImage()
{
  NtfsBrowser::Data::NtfsBpb bpb{};
  std::memcpy(bpb.signature, NTFS_SIGNATURE, sizeof(bpb.signature));
  bpb.bytes_per_sector = kBytesPerSector;
  bpb.sectors_per_cluster = kSectorsPerCluster;
  bpb.lcn_mft = kMftLcn;
  bpb.clusters_per_file_record = 1;
  bpb.clusters_per_index_block = 1;
  bpb.x_aa = 0xAA;
  bpb.x_55 = 0x55;

  const DWORD mftAddr = static_cast<DWORD>(kMftLcn) * kClusterSize;
  const size_t recordsEnd =
      mftAddr + static_cast<size_t>(kFakeFileRecordSize) *
                    (static_cast<size_t>(MftIdx::ROOT) + 1);
  // FULL_CACHE always reads a 64 KiB block regardless of length requested.
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

}
