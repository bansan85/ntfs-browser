#include "fake-ntfs-image.h"

#include <array>
#include <cassert>
#include <cstring>
#include <fstream>
#include <random>
#include <span>
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
#include "attr/standard-information.h"
#include "attr/volume-information.h"
#include "data/index-block.h"
#include "data/index-entry.h"
#include "efs/efs-context.h"
#include "data/ntfs-bpb.h"
#include "flag/filename-namespace.h"
#include "flag/filename.h"
#include "flag/index-entry.h"
#include "flag/std-info-permission.h"
#include "lznt1/decompress.h"

namespace NtfsBrowserTests
{

using NtfsBrowser::AttrType;
using NtfsBrowser::FileRecordHeader;
using NtfsBrowser::kFileRecordMagic;
using NtfsBrowser::Enum::MftIdx;

namespace
{

// Reuses fake-ntfs-image.h's geometry, so fixture constants declared there
// (eg. kCompressionUnitSize) can be sized in real clusters.
constexpr WORD kBytesPerSector = kFakeBytesPerSector;
constexpr BYTE kSectorsPerCluster = kFakeSectorsPerCluster;
constexpr DWORD kClusterSize = kFakeClusterSize;
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

// Builds a fake $MFT record whose DATA attribute has a real, non-empty
// data run of clusters clusters starting at lcn, unlike MakeMftRecord()'s
// empty one.
FakeRecord MakeMftRecordWithRealDataRun(DWORD lcn, DWORD clusters)
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
  attr.last_vcn = clusters - 1;
  attr.data_run_offset = static_cast<WORD>(sizeof(attr));
  attr.comp_unit_size = 0;
  attr.real_size = clusters * kClusterSize;
  attr.alloc_size = attr.real_size;
  attr.ini_size = attr.real_size;

  BYTE* dataRun = &record[kAttrOffset + attr.data_run_offset];
  DWORD runLen = 0;
  // High nibble = LCN offset field size, low nibble = length field size.
  dataRun[runLen++] = 0x41;
  dataRun[runLen++] = static_cast<BYTE>(clusters);
  std::memcpy(&dataRun[runLen], &lcn, sizeof(lcn));
  runLen += sizeof(lcn);
  dataRun[runLen++] = 0x00;  // terminate the run list

  attr.header.total_size = static_cast<DWORD>(sizeof(attr)) + runLen;

  WriteEndOfAttributesMarker(record, kAttrOffset + attr.header.total_size);
  return record;
}

// Builds a record with valid magic but offset_of_us == kFakeFileRecordSize,
// which FileRecordHeader's ctor rejects outright.
FakeRecord MakeInvalidOffsetOfUsRecord()
{
  FakeRecord record{};

  auto& header = *reinterpret_cast<FileRecordHeader::Data*>(record.data());
  header.magic = kFileRecordMagic;
  header.offset_of_us = kFakeFileRecordSize;
  header.size_of_us = 2;

  return record;
}

// Builds a fake $Volume record whose VOLUME_INFORMATION attribute declares
// attrSize bytes, reporting NTFS 3.1.
FakeRecord MakeVolumeRecordSized(WORD attrSize)
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
  attr.attr_size = attrSize;
  attr.attr_offset = static_cast<WORD>(sizeof(attr));
  attr.header.total_size = static_cast<DWORD>(sizeof(attr)) + attr.attr_size;

  auto& volInfo = *reinterpret_cast<NtfsBrowser::Attr::VolumeInformation*>(
      &record[kAttrOffset + attr.attr_offset]);
  volInfo.major_version = 3;
  volInfo.minor_version = 1;

  WriteEndOfAttributesMarker(record, kAttrOffset + attr.header.total_size);
  return record;
}

FakeRecord MakeVolumeRecord()
{
  return MakeVolumeRecordSized(
      static_cast<WORD>(sizeof(NtfsBrowser::Attr::VolumeInformation)));
}

// Builds a fake $Volume record carrying both VOLUME_INFORMATION (so the
// volume reports NTFS 3.1) and a VOLUME_NAME holding name.
FakeRecord MakeVolumeRecordWithName(std::wstring_view name)
{
  FakeRecord record = MakeVolumeRecord();

  auto& volInfo = *reinterpret_cast<NtfsBrowser::Attr::HeaderResident*>(
      &record[kAttrOffset]);
  const DWORD nameOffset = kAttrOffset + volInfo.header.total_size;

  auto& attr = *reinterpret_cast<NtfsBrowser::Attr::HeaderResident*>(
      &record[nameOffset]);
  attr.header.type = AttrType::VOLUME_NAME;
  attr.header.non_resident = 0;
  attr.header.name_length = 0;
  attr.header.flags = 0;
  attr.header.id = 1;
  attr.attr_size = static_cast<DWORD>(name.size() * sizeof(wchar_t));
  attr.attr_offset = static_cast<WORD>(sizeof(attr));
  attr.header.total_size = static_cast<DWORD>(sizeof(attr)) + attr.attr_size;

  std::memcpy(&record[nameOffset + attr.attr_offset], name.data(),
              attr.attr_size);

  WriteEndOfAttributesMarker(record, nameOffset + attr.header.total_size);
  return record;
}

// Builds a resident $STANDARD_INFORMATION attribute exactly attrSize bytes
// long, so a fixture can pin it to NTFS 1.2's real minimum size.
FakeRecord MakeStandardInformationRecordSized(WORD attrSize)
{
  FakeRecord record =
      MakeRecordHeader(kAttrOffset, NtfsBrowser::Flag::FileRecord::INUSE);

  auto& attr = *reinterpret_cast<NtfsBrowser::Attr::HeaderResident*>(
      &record[kAttrOffset]);
  attr.header.type = AttrType::STANDARD_INFORMATION;
  attr.header.non_resident = 0;
  attr.header.name_length = 0;
  attr.header.flags = 0;
  attr.header.id = 0;
  attr.attr_size = attrSize;
  attr.attr_offset = static_cast<WORD>(sizeof(attr));
  attr.header.total_size = static_cast<DWORD>(sizeof(attr)) + attr.attr_size;

  auto& stdInfo = *reinterpret_cast<NtfsBrowser::Attr::StandardInformation*>(
      &record[kAttrOffset + attr.attr_offset]);
  stdInfo.create_time = 0x0102030405060708ULL;
  stdInfo.alter_time = 0x1112131415161718ULL;
  stdInfo.mft_time = 0x2122232425262728ULL;
  stdInfo.read_time = 0x3132333435363738ULL;
  stdInfo.permission = NtfsBrowser::Flag::StdInfoPermission::READONLY;
  stdInfo.max_version_no = 0;
  stdInfo.version_no = 0;
  stdInfo.class_id = 0;

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
  attr.attr_size =
      static_cast<DWORD>(NtfsBrowser::Attr::kAttributeListEntryHeaderSize);
  attr.attr_offset = static_cast<WORD>(sizeof(attr));
  attr.header.total_size = static_cast<DWORD>(sizeof(attr)) + attr.attr_size;

  auto& alEntry = *reinterpret_cast<NtfsBrowser::Attr::AttributeList*>(
      &record[kAttrOffset + attr.attr_offset]);
  alEntry.attr_type = AttrType::INDEX_ROOT;
  alEntry.record_size =
      static_cast<WORD>(NtfsBrowser::Attr::kAttributeListEntryHeaderSize);
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

// Directory whose $ATTRIBUTE_LIST has two entries naming the same
// extension record, once for $INDEX_ROOT and once for $INDEX_ALLOCATION.
FakeRecord MakeAttributeListTwoTypesDirRecord()
{
  FakeRecord record =
      MakeRecordHeader(kAttrOffset, NtfsBrowser::Flag::FileRecord::INUSE |
                                        NtfsBrowser::Flag::FileRecord::DIR);

  constexpr WORD kEntrySize =
      static_cast<WORD>(NtfsBrowser::Attr::kAttributeListEntryHeaderSize);

  auto& attr = *reinterpret_cast<NtfsBrowser::Attr::HeaderResident*>(
      &record[kAttrOffset]);
  attr.header.type = AttrType::ATTRIBUTE_LIST;
  attr.header.non_resident = 0;
  attr.header.name_length = 0;
  attr.header.flags = 0;
  attr.header.id = 0;
  attr.attr_size = static_cast<DWORD>(kEntrySize) * 2;
  attr.attr_offset = static_cast<WORD>(sizeof(attr));
  attr.header.total_size = static_cast<DWORD>(sizeof(attr)) + attr.attr_size;

  BYTE* body = &record[kAttrOffset + attr.attr_offset];

  auto& e0 = *reinterpret_cast<NtfsBrowser::Attr::AttributeList*>(body);
  e0.attr_type = AttrType::INDEX_ROOT;
  e0.record_size = kEntrySize;
  e0.name_length = 0;
  e0.name_offset = 0;
  e0.start_vcn = 0;
  e0.base_ref.segment_number = kMultiTypeExtensionIdx;
  e0.base_ref.sequence_number = 0;
  e0.attr_id = 0;

  auto& e1 =
      *reinterpret_cast<NtfsBrowser::Attr::AttributeList*>(body + kEntrySize);
  e1.attr_type = AttrType::INDEX_ALLOCATION;
  e1.record_size = kEntrySize;
  e1.name_length = 0;
  e1.name_offset = 0;
  e1.start_vcn = 0;
  e1.base_ref.segment_number = kMultiTypeExtensionIdx;
  e1.base_ref.sequence_number = 0;
  e1.attr_id = 0;

  WriteEndOfAttributesMarker(record, kAttrOffset + attr.header.total_size);
  return record;
}

// Extension record holding both a resident $INDEX_ROOT (the same single
// "Foo" entry as MakeIndexRootExtensionRecord()) and a minimal
// non-resident $INDEX_ALLOCATION right after it.
FakeRecord MakeIndexRootAndAllocExtensionRecord()
{
  FakeRecord record =
      MakeRecordHeader(kAttrOffset, NtfsBrowser::Flag::FileRecord::INUSE);

  auto& rootAttr = *reinterpret_cast<NtfsBrowser::Attr::HeaderResident*>(
      &record[kAttrOffset]);
  rootAttr.header.type = AttrType::INDEX_ROOT;
  rootAttr.header.non_resident = 0;
  rootAttr.header.name_length = 0;
  rootAttr.header.flags = 0;
  rootAttr.header.id = 0;
  rootAttr.attr_offset = static_cast<WORD>(sizeof(rootAttr));

  BYTE* body = &record[kAttrOffset + rootAttr.attr_offset];
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
  fn.parent_ref = kAttrListMultiTypeDirIdx;
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

  rootAttr.attr_size =
      static_cast<DWORD>(sizeof(NtfsBrowser::Attr::IndexRoot)) + e1.size +
      e2.size;
  rootAttr.header.total_size =
      static_cast<DWORD>(sizeof(rootAttr)) + rootAttr.attr_size;

  const DWORD allocAttrOffset = kAttrOffset + rootAttr.header.total_size;
  auto& allocAttr = *reinterpret_cast<NtfsBrowser::Attr::HeaderNonResident*>(
      &record[allocAttrOffset]);
  allocAttr.header.type = AttrType::INDEX_ALLOCATION;
  allocAttr.header.non_resident = 1;
  allocAttr.header.name_length = 0;
  allocAttr.header.flags = 0;
  allocAttr.header.id = 0;
  allocAttr.start_vcn = 0;
  allocAttr.last_vcn = 0;
  allocAttr.data_run_offset = static_cast<WORD>(sizeof(allocAttr));
  allocAttr.comp_unit_size = 0;
  allocAttr.real_size = 0;
  allocAttr.alloc_size = 0;
  allocAttr.ini_size = 0;
  allocAttr.header.total_size = static_cast<DWORD>(sizeof(allocAttr)) + 8;

  // Data run: a single 0x00 byte terminates the run list immediately -
  // nothing reads through it in this fixture.
  record[allocAttrOffset + sizeof(allocAttr)] = 0x00;

  WriteEndOfAttributesMarker(record,
                             allocAttrOffset + allocAttr.header.total_size);
  return record;
}

FakeRecord MakeUndersizedResidentAttrRecord()
{
  FakeRecord record =
      MakeRecordHeader(kAttrOffset, NtfsBrowser::Flag::FileRecord::INUSE);

  constexpr DWORD kUndersizedTotalSize = 17;
  static_assert(kUndersizedTotalSize <
                    sizeof(NtfsBrowser::Attr::HeaderResident),
                "total_size must be smaller than a resident attribute header");

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

// LCN for the forged index block, placed past every record's cluster range.
constexpr DWORD kForgedIndexBlockLcn = 20;

// LCN where BuildFakeNtfsImageWithGapCollationSubNode() writes its own
// index block, past every record's cluster range.
constexpr DWORD kGapCollationIndexBlockLcn = 20;

// Builds a directory record whose $INDEX_ALLOCATION references a single
// forged index block, reached through TraverseSubNode() without real
// B+-tree comparisons.
FakeRecord MakeIndexAllocDirRecord()
{
  FakeRecord record =
      MakeRecordHeader(kAttrOffset, NtfsBrowser::Flag::FileRecord::INUSE |
                                        NtfsBrowser::Flag::FileRecord::DIR);

  DWORD offset = kAttrOffset;

  auto& rootAttr =
      *reinterpret_cast<NtfsBrowser::Attr::HeaderResident*>(&record[offset]);
  rootAttr.header.type = AttrType::INDEX_ROOT;
  rootAttr.header.non_resident = 0;
  rootAttr.header.name_length = 0;
  rootAttr.header.flags = 0;
  rootAttr.header.id = 0;
  rootAttr.attr_offset = static_cast<WORD>(sizeof(rootAttr));

  BYTE* body = &record[offset + rootAttr.attr_offset];
  auto& root = *reinterpret_cast<NtfsBrowser::Attr::IndexRoot*>(body);
  root.attr_type = AttrType::FILE_NAME;
  root.coll_rule = 0;
  root.ib_size = kForgedIndexBlockSize;
  root.clusters_per_ib =
      static_cast<BYTE>(kForgedIndexBlockSize / kClusterSize);
  root.entry_offset =
      static_cast<DWORD>((body + sizeof(NtfsBrowser::Attr::IndexRoot)) -
                         reinterpret_cast<BYTE*>(&root.entry_offset));

  auto& e1 = *reinterpret_cast<NtfsBrowser::Data::IndexEntry*>(
      body + sizeof(NtfsBrowser::Attr::IndexRoot));
  e1.mft_index = 0;
  e1.mft_sn = 0;
  e1.stream_size = 0;
  e1.flags = NtfsBrowser::Flag::IndexEntry::SUBNODE |
             NtfsBrowser::Flag::IndexEntry::LAST;
  // Size covers the header up to stream, plus the 8-byte subnode VCN.
  e1.size = static_cast<WORD>(offsetof(NtfsBrowser::Data::IndexEntry, stream) +
                              sizeof(ULONGLONG));
  auto& subNodeVcn = *reinterpret_cast<ULONGLONG*>(
      reinterpret_cast<BYTE*>(&e1) + e1.size - sizeof(ULONGLONG));
  subNodeVcn = 0;

  root.total_entry_size = e1.size;
  root.alloc_entry_size = e1.size;
  root.flags = 0;

  rootAttr.attr_size =
      static_cast<DWORD>(sizeof(NtfsBrowser::Attr::IndexRoot)) + e1.size;
  rootAttr.header.total_size =
      static_cast<DWORD>(sizeof(rootAttr)) + rootAttr.attr_size;

  offset += rootAttr.header.total_size;

  auto& allocAttr =
      *reinterpret_cast<NtfsBrowser::Attr::HeaderNonResident*>(&record[offset]);
  allocAttr.header.type = AttrType::INDEX_ALLOCATION;
  allocAttr.header.non_resident = 1;
  allocAttr.header.name_length = 0;
  allocAttr.header.flags = 0;
  allocAttr.header.id = 0;
  allocAttr.start_vcn = 0;
  allocAttr.last_vcn =
      static_cast<ULONGLONG>(kForgedIndexBlockSize / kClusterSize) - 1;
  allocAttr.data_run_offset = static_cast<WORD>(sizeof(allocAttr));
  allocAttr.comp_unit_size = 0;
  allocAttr.real_size = kForgedIndexBlockSize;
  allocAttr.alloc_size = kForgedIndexBlockSize;
  allocAttr.ini_size = kForgedIndexBlockSize;

  BYTE* dataRun = &record[offset + allocAttr.data_run_offset];
  DWORD runLen = 0;
  // High nibble = 4-byte LCN offset field; low nibble = 1-byte run length.
  dataRun[runLen++] = 0x41;
  dataRun[runLen++] = static_cast<BYTE>(kForgedIndexBlockSize / kClusterSize);
  {
    const DWORD lcn = kForgedIndexBlockLcn;
    std::memcpy(&dataRun[runLen], &lcn, sizeof(lcn));
    runLen += sizeof(lcn);
  }
  dataRun[runLen++] = 0x00;  // terminate the run list

  allocAttr.header.total_size = static_cast<DWORD>(sizeof(allocAttr)) + runLen;

  offset += allocAttr.header.total_size;

  WriteEndOfAttributesMarker(record, offset);
  return record;
}

// Extension record: a minimal non-resident $INDEX_ALLOCATION whose
// real_size is the given sentinel.
FakeRecord MakeIndexAllocationOnlyExtensionRecord(DWORD realSize)
{
  FakeRecord record =
      MakeRecordHeader(kAttrOffset, NtfsBrowser::Flag::FileRecord::INUSE);

  auto& allocAttr = *reinterpret_cast<NtfsBrowser::Attr::HeaderNonResident*>(
      &record[kAttrOffset]);
  allocAttr.header.type = AttrType::INDEX_ALLOCATION;
  allocAttr.header.non_resident = 1;
  allocAttr.header.name_length = 0;
  allocAttr.header.flags = 0;
  allocAttr.header.id = 0;
  allocAttr.start_vcn = 0;
  allocAttr.last_vcn = 0;
  allocAttr.data_run_offset = static_cast<WORD>(sizeof(allocAttr));
  allocAttr.comp_unit_size = 0;
  allocAttr.real_size = realSize;
  allocAttr.alloc_size = realSize;
  allocAttr.ini_size = realSize;
  allocAttr.header.total_size = static_cast<DWORD>(sizeof(allocAttr)) + 8;

  // Data run: a single 0x00 byte terminates the run list immediately.
  record[kAttrOffset + sizeof(allocAttr)] = 0x00;

  WriteEndOfAttributesMarker(record, kAttrOffset + allocAttr.header.total_size);
  return record;
}

// Directory whose $ATTRIBUTE_LIST has four entries, each naming a
// different extension record for the same attribute type.
FakeRecord MakeFragmentedAttributeListDirRecord()
{
  FakeRecord record =
      MakeRecordHeader(kAttrOffset, NtfsBrowser::Flag::FileRecord::INUSE |
                                        NtfsBrowser::Flag::FileRecord::DIR);

  constexpr WORD kEntrySize =
      static_cast<WORD>(NtfsBrowser::Attr::kAttributeListEntryHeaderSize);

  auto& attr = *reinterpret_cast<NtfsBrowser::Attr::HeaderResident*>(
      &record[kAttrOffset]);
  attr.header.type = AttrType::ATTRIBUTE_LIST;
  attr.header.non_resident = 0;
  attr.header.name_length = 0;
  attr.header.flags = 0;
  attr.header.id = 0;
  attr.attr_size = static_cast<DWORD>(kEntrySize) * 4;
  attr.attr_offset = static_cast<WORD>(sizeof(attr));
  attr.header.total_size = static_cast<DWORD>(sizeof(attr)) + attr.attr_size;

  const std::array<ULONGLONG, 4> extensionIdxs{
      kUafExtensionIdx0, kUafExtensionIdx1, kUafExtensionIdx2,
      kUafExtensionIdx3};

  BYTE* body = &record[kAttrOffset + attr.attr_offset];
  for (size_t i = 0; i < extensionIdxs.size(); i++)
  {
    auto& entry = *reinterpret_cast<NtfsBrowser::Attr::AttributeList*>(
        body + i * kEntrySize);
    entry.attr_type = AttrType::INDEX_ALLOCATION;
    entry.record_size = kEntrySize;
    entry.name_length = 0;
    entry.name_offset = 0;
    entry.start_vcn = 0;
    entry.base_ref.segment_number = extensionIdxs[i];
    entry.base_ref.sequence_number = 0;
    entry.attr_id = 0;
  }

  WriteEndOfAttributesMarker(record, kAttrOffset + attr.header.total_size);
  return record;
}

// A single resident $DATA attribute whose name_offset/name_length point
// past its own declared total_size, while still landing on known,
// deterministic bytes inside the record buffer.
FakeRecord MakeAttrNameExceedsTotalSizeRecord()
{
  FakeRecord record =
      MakeRecordHeader(kAttrOffset, NtfsBrowser::Flag::FileRecord::INUSE);

  constexpr DWORD kBodySize = 4;

  auto& attr = *reinterpret_cast<NtfsBrowser::Attr::HeaderResident*>(
      &record[kAttrOffset]);
  attr.header.type = AttrType::DATA;
  attr.header.non_resident = 0;
  attr.header.flags = 0;
  attr.header.id = 0;
  attr.attr_size = kBodySize;
  attr.attr_offset = static_cast<WORD>(sizeof(attr));
  attr.header.total_size = static_cast<DWORD>(sizeof(attr)) + kBodySize;

  static_assert(static_cast<DWORD>(kAttrNameBoundsNameOffset) +
                        2 * static_cast<DWORD>(kAttrNameBoundsNameLength) >
                    sizeof(NtfsBrowser::Attr::HeaderResident) + kBodySize,
                "name must exceed total_size");
  static_assert(sizeof(kAttrNameBoundsSentinel) - sizeof(wchar_t) ==
                    static_cast<size_t>(kAttrNameBoundsNameLength) *
                        sizeof(wchar_t),
                "sentinel length must match name_length exactly");

  attr.header.name_length = kAttrNameBoundsNameLength;
  attr.header.name_offset = kAttrNameBoundsNameOffset;

  // Past total_size (28), but still inside the 1024-byte record buffer.
  std::memcpy(&record[kAttrOffset + kAttrNameBoundsNameOffset],
              kAttrNameBoundsSentinel,
              static_cast<size_t>(kAttrNameBoundsNameLength) * sizeof(wchar_t));

  WriteEndOfAttributesMarker(record, kAttrOffset + attr.header.total_size);
  return record;
}

// Builds a root-directory replacement holding a single, well-formed
// resident $DATA attribute whose body is exactly kSmallResidentDataContent.
FakeRecord MakeSmallResidentDataRecord()
{
  FakeRecord record =
      MakeRecordHeader(kAttrOffset, NtfsBrowser::Flag::FileRecord::INUSE);

  auto& attr = *reinterpret_cast<NtfsBrowser::Attr::HeaderResident*>(
      &record[kAttrOffset]);
  attr.header.type = AttrType::DATA;
  attr.header.non_resident = 0;
  attr.header.name_length = 0;
  attr.header.flags = 0;
  attr.header.id = 0;
  attr.attr_size = static_cast<DWORD>(kSmallResidentDataContent.size());
  attr.attr_offset = static_cast<WORD>(sizeof(attr));
  attr.header.total_size = static_cast<DWORD>(sizeof(attr)) + attr.attr_size;

  std::memcpy(&record[kAttrOffset + attr.attr_offset],
              kSmallResidentDataContent.data(),
              kSmallResidentDataContent.size());

  WriteEndOfAttributesMarker(record, kAttrOffset + attr.header.total_size);
  return record;
}

// Builds a root-directory replacement whose resident $ATTRIBUTE_LIST holds
// one full, self-referencing entry, followed by a truncated partial one.
FakeRecord MakeAttributeListShortReadRecord()
{
  FakeRecord record =
      MakeRecordHeader(kAttrOffset, NtfsBrowser::Flag::FileRecord::INUSE |
                                        NtfsBrowser::Flag::FileRecord::DIR);

  constexpr DWORD kBodySize =
      static_cast<DWORD>(NtfsBrowser::Attr::kAttributeListEntryHeaderSize) + 10;
  static_assert(kBodySize % NtfsBrowser::Attr::kAttributeListEntryHeaderSize !=
                    0,
                "body size must not be an exact multiple of the entry size, "
                "to reproduce a short final ReadData()");

  auto& attr = *reinterpret_cast<NtfsBrowser::Attr::HeaderResident*>(
      &record[kAttrOffset]);
  attr.header.type = AttrType::ATTRIBUTE_LIST;
  attr.header.non_resident = 0;
  attr.header.name_length = 0;
  attr.header.flags = 0;
  attr.header.id = 0;
  attr.attr_size = kBodySize;
  attr.attr_offset = static_cast<WORD>(sizeof(attr));
  attr.header.total_size = static_cast<DWORD>(sizeof(attr)) + attr.attr_size;

  auto& e1 = *reinterpret_cast<NtfsBrowser::Attr::AttributeList*>(
      &record[kAttrOffset + attr.attr_offset]);
  e1.attr_type = AttrType::DATA;
  e1.record_size =
      static_cast<WORD>(NtfsBrowser::Attr::kAttributeListEntryHeaderSize);
  e1.name_length = 0;
  e1.name_offset = 0;
  e1.start_vcn = 0;
  e1.base_ref.segment_number = static_cast<ULONGLONG>(MftIdx::ROOT);
  e1.base_ref.sequence_number = 0;
  e1.attr_id = 0;

  WriteEndOfAttributesMarker(record, kAttrOffset + attr.header.total_size);
  return record;
}

// Builds a directory record whose resident $ATTRIBUTE_LIST, via a single
// full entry, relocates to targetIdx.
FakeRecord MakeAttributeListCycleRecord(ULONGLONG targetIdx)
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
  attr.attr_size =
      static_cast<DWORD>(NtfsBrowser::Attr::kAttributeListEntryHeaderSize);
  attr.attr_offset = static_cast<WORD>(sizeof(attr));
  attr.header.total_size = static_cast<DWORD>(sizeof(attr)) + attr.attr_size;

  auto& e1 = *reinterpret_cast<NtfsBrowser::Attr::AttributeList*>(
      &record[kAttrOffset + attr.attr_offset]);
  e1.attr_type = AttrType::ATTRIBUTE_LIST;
  e1.record_size =
      static_cast<WORD>(NtfsBrowser::Attr::kAttributeListEntryHeaderSize);
  e1.name_length = 0;
  e1.name_offset = 0;
  e1.start_vcn = 0;
  e1.base_ref.segment_number = targetIdx;
  e1.base_ref.sequence_number = 0;
  e1.attr_id = 0;

  WriteEndOfAttributesMarker(record, kAttrOffset + attr.header.total_size);
  return record;
}

// Builds a directory record whose resident $ATTRIBUTE_LIST packs two
// entries at the real on-disk stride (kAttributeListRealEntrySize), not
// sizeof(Attr::AttributeList).
FakeRecord MakeAttributeListTightlyPackedDirRecord()
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
  attr.attr_size = static_cast<DWORD>(kAttributeListRealEntrySize) * 2;
  attr.attr_offset = static_cast<WORD>(sizeof(attr));
  attr.header.total_size = static_cast<DWORD>(sizeof(attr)) + attr.attr_size;

  BYTE* body = &record[kAttrOffset + attr.attr_offset];

  auto& e1 = *reinterpret_cast<NtfsBrowser::Attr::AttributeList*>(body);
  e1.attr_type = AttrType::INDEX_ROOT;
  e1.record_size = kAttributeListRealEntrySize;
  e1.name_length = 0;
  e1.name_offset = 0;
  e1.start_vcn = 0;
  e1.base_ref.segment_number = kAttrListTightPackExtIdxA;
  e1.base_ref.sequence_number = 0;
  e1.attr_id = 0;

  auto& e2 = *reinterpret_cast<NtfsBrowser::Attr::AttributeList*>(
      body + kAttributeListRealEntrySize);
  e2.attr_type = AttrType::INDEX_ALLOCATION;
  e2.record_size = kAttributeListRealEntrySize;
  e2.name_length = 0;
  e2.name_offset = 0;
  e2.start_vcn = 0;
  e2.base_ref.segment_number = kAttrListTightPackExtIdxB;
  e2.base_ref.sequence_number = 0;
  e2.attr_id = 1;

  WriteEndOfAttributesMarker(record, kAttrOffset + attr.header.total_size);
  return record;
}

// Builds a root-directory replacement whose sole attribute is a resident,
// named $DATA stream (an ADS) holding kNamedDataStreamContent under
// kNamedDataStreamName.
FakeRecord MakeNamedDataStreamRecord()
{
  FakeRecord record =
      MakeRecordHeader(kAttrOffset, NtfsBrowser::Flag::FileRecord::INUSE);

  auto& attr = *reinterpret_cast<NtfsBrowser::Attr::HeaderResident*>(
      &record[kAttrOffset]);
  attr.header.type = AttrType::DATA;
  attr.header.non_resident = 0;
  attr.header.flags = 0;
  attr.header.id = 0;
  attr.header.name_length = kNamedDataStreamNameLength;
  attr.header.name_offset = static_cast<WORD>(sizeof(attr));
  attr.attr_size = static_cast<DWORD>(kNamedDataStreamContent.size());
  attr.attr_offset = static_cast<WORD>(
      sizeof(attr) +
      static_cast<size_t>(kNamedDataStreamNameLength) * sizeof(wchar_t));
  attr.header.total_size =
      static_cast<DWORD>(attr.attr_offset) + attr.attr_size;

  std::memcpy(
      &record[kAttrOffset + attr.header.name_offset], kNamedDataStreamName,
      static_cast<size_t>(kNamedDataStreamNameLength) * sizeof(wchar_t));
  std::memcpy(&record[kAttrOffset + attr.attr_offset],
              kNamedDataStreamContent.data(), kNamedDataStreamContent.size());

  WriteEndOfAttributesMarker(record, kAttrOffset + attr.header.total_size);
  return record;
}

// Builds a directory record whose resident $INDEX_ROOT holds one real
// FILE_NAME entry (name, mftIndex, parentRef) plus the terminating entry.
FakeRecord MakeIndexRootDirRecord(std::wstring_view name, ULONGLONG mftIndex,
                                  ULONGLONG parentRef)
{
  FakeRecord record =
      MakeRecordHeader(kAttrOffset, NtfsBrowser::Flag::FileRecord::INUSE |
                                        NtfsBrowser::Flag::FileRecord::DIR);

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

  // Entry 1: the single real FILE_NAME entry this variant declares.
  auto& e1 = *reinterpret_cast<NtfsBrowser::Data::IndexEntry*>(
      body + sizeof(NtfsBrowser::Attr::IndexRoot));
  e1.mft_index = mftIndex;
  e1.mft_sn = 1;

  auto& fn = *reinterpret_cast<NtfsBrowser::Attr::Filename*>(&e1.stream);
  fn.parent_ref = parentRef;
  fn.flags = NtfsBrowser::Flag::Filename::NONE;
  fn.name_length = static_cast<BYTE>(name.size());
  fn.name_space = NtfsBrowser::Flag::FilenameNamespace::WIN_32;
  for (BYTE i = 0; i < fn.name_length; i++)
  {
    fn.name[i] = static_cast<WORD>(name[i]);
  }

  e1.stream_size =
      static_cast<WORD>(reinterpret_cast<BYTE*>(&fn.name[fn.name_length]) -
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

// Builds a root-directory replacement whose own $INDEX_ROOT holds one
// real, non-terminal FILE_NAME entry that is also a sub-node pointer into
// a real $INDEX_ALLOCATION index block.
FakeRecord MakeRootRecordWithGapCollationSubNode()
{
  FakeRecord record =
      MakeRecordHeader(kAttrOffset, NtfsBrowser::Flag::FileRecord::INUSE |
                                        NtfsBrowser::Flag::FileRecord::DIR);

  DWORD offset = kAttrOffset;

  // $INDEX_ROOT
  auto& rootAttr =
      *reinterpret_cast<NtfsBrowser::Attr::HeaderResident*>(&record[offset]);
  rootAttr.header.type = AttrType::INDEX_ROOT;
  rootAttr.header.non_resident = 0;
  rootAttr.header.name_length = 0;
  rootAttr.header.flags = 0;
  rootAttr.header.id = 0;
  rootAttr.attr_offset = static_cast<WORD>(sizeof(rootAttr));

  BYTE* body = &record[offset + rootAttr.attr_offset];
  auto& root = *reinterpret_cast<NtfsBrowser::Attr::IndexRoot*>(body);
  root.attr_type = AttrType::FILE_NAME;
  root.coll_rule = 0;
  root.ib_size = kFakeFileRecordSize;
  root.clusters_per_ib = 1;
  root.entry_offset =
      static_cast<DWORD>((body + sizeof(NtfsBrowser::Attr::IndexRoot)) -
                         reinterpret_cast<BYTE*>(&root.entry_offset));

  // Entry 1 ("A_"): non-terminal, so it carries both a name and a sub-node
  // VCN right after it.
  auto& e1 = *reinterpret_cast<NtfsBrowser::Data::IndexEntry*>(
      body + sizeof(NtfsBrowser::Attr::IndexRoot));
  e1.mft_index = kGapCollationNonTerminalMftRef;
  e1.mft_sn = 1;

  auto& fn1 = *reinterpret_cast<NtfsBrowser::Attr::Filename*>(&e1.stream);
  fn1.parent_ref = static_cast<ULONGLONG>(MftIdx::ROOT);
  fn1.flags = NtfsBrowser::Flag::Filename::DIRECTORY;
  constexpr wchar_t kNonTerminalName[] = L"A_";
  fn1.name_length = 2;
  fn1.name_space = NtfsBrowser::Flag::FilenameNamespace::WIN_32;
  for (BYTE i = 0; i < fn1.name_length; i++)
  {
    fn1.name[i] = static_cast<WORD>(kNonTerminalName[i]);
  }

  e1.stream_size =
      static_cast<WORD>(reinterpret_cast<BYTE*>(&fn1.name[fn1.name_length]) -
                        reinterpret_cast<BYTE*>(&fn1));
  e1.flags = NtfsBrowser::Flag::IndexEntry::SUBNODE;
  e1.size = static_cast<WORD>(reinterpret_cast<BYTE*>(&e1.stream) -
                              reinterpret_cast<BYTE*>(&e1) + e1.stream_size +
                              sizeof(ULONGLONG));
  auto& subNodeVcn = *reinterpret_cast<ULONGLONG*>(
      reinterpret_cast<BYTE*>(&e1) + e1.size - sizeof(ULONGLONG));
  subNodeVcn = 0;

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

  rootAttr.attr_size =
      static_cast<DWORD>(sizeof(NtfsBrowser::Attr::IndexRoot)) + e1.size +
      e2.size;
  rootAttr.header.total_size =
      static_cast<DWORD>(sizeof(rootAttr)) + rootAttr.attr_size;

  offset += rootAttr.header.total_size;

  // $INDEX_ALLOCATION
  auto& allocAttr =
      *reinterpret_cast<NtfsBrowser::Attr::HeaderNonResident*>(&record[offset]);
  allocAttr.header.type = AttrType::INDEX_ALLOCATION;
  allocAttr.header.non_resident = 1;
  allocAttr.header.name_length = 0;
  allocAttr.header.flags = 0;
  allocAttr.header.id = 0;
  allocAttr.start_vcn = 0;
  allocAttr.last_vcn = 0;  // single cluster -> VCN 0 only
  allocAttr.data_run_offset = static_cast<WORD>(sizeof(allocAttr));
  allocAttr.comp_unit_size = 0;
  allocAttr.real_size = kClusterSize;
  allocAttr.alloc_size = kClusterSize;
  allocAttr.ini_size = kClusterSize;

  BYTE* dataRun = &record[offset + allocAttr.data_run_offset];
  DWORD runLen = 0;
  // High nibble = LCN offset field size, low nibble = length field size.
  dataRun[runLen++] = 0x41;
  dataRun[runLen++] = 1;  // 1 cluster
  {
    const DWORD lcn = kGapCollationIndexBlockLcn;
    std::memcpy(&dataRun[runLen], &lcn, sizeof(lcn));
    runLen += sizeof(lcn);
  }
  dataRun[runLen++] = 0x00;  // terminate the run list

  allocAttr.header.total_size = static_cast<DWORD>(sizeof(allocAttr)) + runLen;

  offset += allocAttr.header.total_size;

  WriteEndOfAttributesMarker(record, offset);
  return record;
}

// LCN where BuildFakeNtfsImageWithDeepIndexBlockChain() writes its chained
// index blocks, kept clear of every other fixture's placement in this file.
constexpr DWORD kIndexBlockChainLcn = 100;

// Builds a root-directory replacement whose $INDEX_ROOT sub-node pointer
// leads into a kIndexBlockChainLength-block chained $INDEX_ALLOCATION.
FakeRecord MakeIndexBlockChainRootRecord()
{
  FakeRecord record =
      MakeRecordHeader(kAttrOffset, NtfsBrowser::Flag::FileRecord::INUSE |
                                        NtfsBrowser::Flag::FileRecord::DIR);

  DWORD offset = kAttrOffset;

  // $INDEX_ROOT
  auto& rootAttr =
      *reinterpret_cast<NtfsBrowser::Attr::HeaderResident*>(&record[offset]);
  rootAttr.header.type = AttrType::INDEX_ROOT;
  rootAttr.header.non_resident = 0;
  rootAttr.header.name_length = 0;
  rootAttr.header.flags = 0;
  rootAttr.header.id = 0;
  rootAttr.attr_offset = static_cast<WORD>(sizeof(rootAttr));

  BYTE* body = &record[offset + rootAttr.attr_offset];
  auto& root = *reinterpret_cast<NtfsBrowser::Attr::IndexRoot*>(body);
  root.attr_type = AttrType::FILE_NAME;
  root.coll_rule = 0;
  root.ib_size = kClusterSize;
  root.clusters_per_ib = 1;
  root.entry_offset =
      static_cast<DWORD>((body + sizeof(NtfsBrowser::Attr::IndexRoot)) -
                         reinterpret_cast<BYTE*>(&root.entry_offset));

  auto& e1 = *reinterpret_cast<NtfsBrowser::Data::IndexEntry*>(
      body + sizeof(NtfsBrowser::Attr::IndexRoot));
  e1.mft_index = 0;
  e1.mft_sn = 0;
  e1.stream_size = 0;
  e1.flags = NtfsBrowser::Flag::IndexEntry::SUBNODE |
             NtfsBrowser::Flag::IndexEntry::LAST;
  // Header plus the 8-byte subnode VCN that replaces "stream" when empty.
  e1.size = static_cast<WORD>(offsetof(NtfsBrowser::Data::IndexEntry, stream) +
                              sizeof(ULONGLONG));
  auto& subNodeVcn = *reinterpret_cast<ULONGLONG*>(
      reinterpret_cast<BYTE*>(&e1) + e1.size - sizeof(ULONGLONG));
  subNodeVcn = 0;

  root.total_entry_size = e1.size;
  root.alloc_entry_size = e1.size;
  root.flags = 0;

  rootAttr.attr_size =
      static_cast<DWORD>(sizeof(NtfsBrowser::Attr::IndexRoot)) + e1.size;
  rootAttr.header.total_size =
      static_cast<DWORD>(sizeof(rootAttr)) + rootAttr.attr_size;

  offset += rootAttr.header.total_size;

  // $INDEX_ALLOCATION: one data run, kIndexBlockChainLength clusters starting
  // at kIndexBlockChainLcn.
  auto& allocAttr =
      *reinterpret_cast<NtfsBrowser::Attr::HeaderNonResident*>(&record[offset]);
  allocAttr.header.type = AttrType::INDEX_ALLOCATION;
  allocAttr.header.non_resident = 1;
  allocAttr.header.name_length = 0;
  allocAttr.header.flags = 0;
  allocAttr.header.id = 0;
  allocAttr.start_vcn = 0;
  allocAttr.last_vcn = kIndexBlockChainLength - 1;
  allocAttr.data_run_offset = static_cast<WORD>(sizeof(allocAttr));
  allocAttr.comp_unit_size = 0;
  allocAttr.real_size = kIndexBlockChainLength * kClusterSize;
  allocAttr.alloc_size = allocAttr.real_size;
  allocAttr.ini_size = allocAttr.real_size;

  BYTE* dataRun = &record[offset + allocAttr.data_run_offset];
  DWORD runLen = 0;
  // Data run header byte: high nibble = LCN offset field size (4 bytes),
  // low nibble = length field size (1 byte) - standard NTFS run encoding
  // (AttrNonResident::PickData, src/attr-non-resident.cpp).
  dataRun[runLen++] = 0x41;
  dataRun[runLen++] = static_cast<BYTE>(kIndexBlockChainLength);
  {
    const DWORD lcn = kIndexBlockChainLcn;
    std::memcpy(&dataRun[runLen], &lcn, sizeof(lcn));
    runLen += sizeof(lcn);
  }
  dataRun[runLen++] = 0x00;  // terminate the run list

  allocAttr.header.total_size = static_cast<DWORD>(sizeof(allocAttr)) + runLen;

  offset += allocAttr.header.total_size;

  WriteEndOfAttributesMarker(record, offset);
  return record;
}

////////////////////////////////////////////////////////////////////////////
// NTFS compression fixtures (see fake-ntfs-image.h for what each builds)
////////////////////////////////////////////////////////////////////////////

// AttrHeaderCommon::flags bit 0 ("compressed"); unread by the library itself
// but set here since a real compressed attribute always sets it too.
constexpr WORD kAttrFlagCompressed = 0x0001;

// Encodes "runs" into NTFS' real, delta-LCN run-list format at "dataRun"
// (terminated by 0x00), and returns the byte count written.
DWORD EncodeDataRuns(BYTE* dataRun, const std::vector<FakeDataRun>& runs)
{
  DWORD runLen = 0;
  DWORD previousLcn = 0;

  for (const FakeDataRun& run : runs)
  {
    if (run.lcn)
    {
      dataRun[runLen++] = 0x41;
      dataRun[runLen++] = static_cast<BYTE>(run.clusters);
      const LONG delta =
          static_cast<LONG>(*run.lcn) - static_cast<LONG>(previousLcn);
      std::memcpy(&dataRun[runLen], &delta, sizeof(delta));
      runLen += sizeof(delta);
      previousLcn = *run.lcn;
    }
    else
    {
      dataRun[runLen++] = 0x01;
      dataRun[runLen++] = static_cast<BYTE>(run.clusters);
    }
  }

  dataRun[runLen++] = 0x00;  // terminate the run list
  return runLen;
}

// Writes one resident $STANDARD_INFORMATION attribute at record[offset]
// with the given DOS permission bits, and returns its total_size.
DWORD WriteStandardInformationAttr(
    FakeRecord& record, DWORD offset,
    NtfsBrowser::Flag::StdInfoPermission permission)
{
  auto& attr =
      *reinterpret_cast<NtfsBrowser::Attr::HeaderResident*>(&record[offset]);
  attr.header.type = AttrType::STANDARD_INFORMATION;
  attr.header.non_resident = 0;
  attr.header.name_length = 0;
  attr.header.flags = 0;
  attr.header.id = 0;
  attr.attr_size =
      static_cast<DWORD>(sizeof(NtfsBrowser::Attr::StandardInformation));
  attr.attr_offset = static_cast<WORD>(sizeof(attr));
  attr.header.total_size = static_cast<DWORD>(sizeof(attr)) + attr.attr_size;

  auto& stdInfo = *reinterpret_cast<NtfsBrowser::Attr::StandardInformation*>(
      &record[offset + attr.attr_offset]);
  stdInfo.create_time = 0x0102030405060708ULL;
  stdInfo.alter_time = 0x1112131415161718ULL;
  stdInfo.mft_time = 0x2122232425262728ULL;
  stdInfo.read_time = 0x3132333435363738ULL;
  stdInfo.permission = permission;

  return attr.header.total_size;
}

// Header fields the deliberately malformed compression fixtures forge -
// values a well-formed builder never derives from its own run list.
struct FakeNonResidentOverrides
{
  // Non-zero models one $ATTRIBUTE_LIST fragment of a split attribute.
  ULONGLONG start_vcn = 0;
  // Declares a VCN range wider than the runs map - ParseDataRun()'s state
  // after stopping early on a decode error.
  std::optional<ULONGLONG> last_vcn;
  // Overrides "header + run list bytes" as the attribute's total_size.
  std::optional<DWORD> total_size;
  // A stream name, written between the header and the run list.
  std::wstring_view name;
  // Overrides the flags a compressed/plain attribute would get.
  std::optional<WORD> flags;
};

// Writes one non-resident attribute at record[offset] and returns its
// declared total_size. compUnitSize == 0 is ordinary; non-zero adds the
// trailing 8-byte CompressedSize field.
DWORD WriteNonResidentAttr(FakeRecord& record, DWORD offset, AttrType type,
                           WORD compUnitSize, ULONGLONG realSize,
                           const std::vector<FakeDataRun>& runs,
                           const FakeNonResidentOverrides& overrides = {})
{
  auto& attr =
      *reinterpret_cast<NtfsBrowser::Attr::HeaderNonResident*>(&record[offset]);
  attr.header.type = type;
  attr.header.non_resident = 1;
  assert(overrides.name.size() <= 255 && "on-disk name_length is one byte");
  attr.header.name_length = static_cast<BYTE>(overrides.name.size());
  attr.header.flags = overrides.flags.value_or(
      (compUnitSize != 0) ? kAttrFlagCompressed : static_cast<WORD>(0));
  attr.header.id = 0;

  ULONGLONG totalClusters = 0;
  ULONGLONG realClusters = 0;
  for (const FakeDataRun& run : runs)
  {
    totalClusters += run.clusters;
    if (run.lcn)
    {
      realClusters += run.clusters;
    }
  }

  attr.start_vcn = overrides.start_vcn;
  attr.last_vcn = overrides.last_vcn.value_or(
      overrides.start_vcn + ((totalClusters == 0) ? 0 : totalClusters - 1));
  attr.comp_unit_size = compUnitSize;
  attr.alloc_size = totalClusters * kClusterSize;
  attr.real_size = realSize;
  attr.ini_size = realSize;

  const auto headerSize = static_cast<WORD>(
      sizeof(attr) +
      ((compUnitSize != 0) ? NtfsBrowser::Attr::kCompressedSizeFieldSize : 0));
  const size_t nameBytes = overrides.name.size() * sizeof(wchar_t);
  if (nameBytes != 0)
  {
    attr.header.name_offset = headerSize;
    std::memcpy(&record[offset + headerSize], overrides.name.data(), nameBytes);
  }
  const auto runOffset = static_cast<WORD>(headerSize + nameBytes);
  attr.data_run_offset = runOffset;

  if (compUnitSize != 0)
  {
    // CompressedSize: total allocated size of the attribute's compressed
    // clusters, i.e. everything actually on disk (the sparse padding of each
    // compressed unit excluded).
    const ULONGLONG compressedSize = realClusters * kClusterSize;
    std::memcpy(&record[offset + sizeof(attr)], &compressedSize,
                sizeof(compressedSize));
  }

  const DWORD runLen = EncodeDataRuns(&record[offset + runOffset], runs);
  attr.header.total_size =
      overrides.total_size.value_or(static_cast<DWORD>(runOffset) + runLen);
  return attr.header.total_size;
}

// File record (root, #5): resident $STANDARD_INFORMATION("permission") plus
// one non-resident $DATA attribute described by compUnitSize/realSize/runs.
FakeRecord
    MakeNonResidentDataRecord(NtfsBrowser::Flag::StdInfoPermission permission,
                              WORD compUnitSize, ULONGLONG realSize,
                              const std::vector<FakeDataRun>& runs,
                              const FakeNonResidentOverrides& overrides = {})
{
  FakeRecord record =
      MakeRecordHeader(kAttrOffset, NtfsBrowser::Flag::FileRecord::INUSE);

  DWORD offset = kAttrOffset;
  offset += WriteStandardInformationAttr(record, offset, permission);
  offset += WriteNonResidentAttr(record, offset, AttrType::DATA, compUnitSize,
                                 realSize, runs, overrides);

  WriteEndOfAttributesMarker(record, offset);
  return record;
}

// One leaf $FILE_NAME index entry a fixture writes: a name, the MFT record
// it points at, and whether that record is a directory.
struct FakeIndexName
{
  std::wstring_view name;
  ULONGLONG mft_ref;
  bool directory;
};

// A UTF-16 code unit is one on-disk name character; wchar_t must match it
// for a name to be copied over unit by unit.
static_assert(sizeof(wchar_t) == sizeof(WORD),
              "fixtures copy wchar_t names into 16-bit on-disk units");

// Writes "name" as a leaf index entry at "dest" and returns its size in
// bytes. Entries are packed with no padding, like every other fixture here.
WORD WriteFilenameEntry(BYTE* dest, const FakeIndexName& name)
{
  auto& entry = *reinterpret_cast<NtfsBrowser::Data::IndexEntry*>(dest);
  entry.mft_index = name.mft_ref;
  entry.mft_sn = 1;

  auto& fn = *reinterpret_cast<NtfsBrowser::Attr::Filename*>(&entry.stream);
  fn.parent_ref = static_cast<ULONGLONG>(MftIdx::ROOT);
  fn.flags = name.directory ? NtfsBrowser::Flag::Filename::DIRECTORY
                            : NtfsBrowser::Flag::Filename::NONE;
  fn.name_length = static_cast<BYTE>(name.name.size());
  fn.name_space = NtfsBrowser::Flag::FilenameNamespace::WIN_32;
  for (size_t i = 0; i < name.name.size(); i++)
  {
    fn.name[i] = static_cast<WORD>(name.name[i]);
  }

  entry.stream_size =
      static_cast<WORD>(reinterpret_cast<BYTE*>(&fn.name[fn.name_length]) -
                        reinterpret_cast<BYTE*>(&fn));
  entry.size =
      static_cast<WORD>(reinterpret_cast<BYTE*>(&entry.stream) -
                        reinterpret_cast<BYTE*>(&entry) + entry.stream_size);
  return entry.size;
}

// Directory record (root, #5): resident $STANDARD_INFORMATION("permission"),
// a resident $INDEX_ROOT holding "rootNames" as leaf entries followed by a
// nameless SUBNODE-only entry, and a non-resident $INDEX_ALLOCATION - lets
// fixtures target an attribute FuzzOnce() actually ReadData()s, unlike plain
// $DATA.
FakeRecord MakeIndexAllocationDirRecord(
    NtfsBrowser::Flag::StdInfoPermission permission, WORD compUnitSize,
    ULONGLONG realSize, const std::vector<FakeDataRun>& runs,
    const FakeNonResidentOverrides& overrides = {},
    std::span<const FakeIndexName> rootNames = {})
{
  FakeRecord record =
      MakeRecordHeader(kAttrOffset, NtfsBrowser::Flag::FileRecord::INUSE |
                                        NtfsBrowser::Flag::FileRecord::DIR);

  DWORD offset = kAttrOffset;
  offset += WriteStandardInformationAttr(record, offset, permission);

  // $INDEX_ROOT
  auto& rootAttr =
      *reinterpret_cast<NtfsBrowser::Attr::HeaderResident*>(&record[offset]);
  rootAttr.header.type = AttrType::INDEX_ROOT;
  rootAttr.header.non_resident = 0;
  rootAttr.header.name_length = 0;
  rootAttr.header.flags = 0;
  rootAttr.header.id = 0;
  rootAttr.attr_offset = static_cast<WORD>(sizeof(rootAttr));

  BYTE* body = &record[offset + rootAttr.attr_offset];
  auto& root = *reinterpret_cast<NtfsBrowser::Attr::IndexRoot*>(body);
  root.attr_type = AttrType::FILE_NAME;
  root.coll_rule = 0;
  root.ib_size = kFakeFileRecordSize;
  root.clusters_per_ib = 1;
  root.entry_offset =
      static_cast<DWORD>((body + sizeof(NtfsBrowser::Attr::IndexRoot)) -
                         reinterpret_cast<BYTE*>(&root.entry_offset));

  BYTE* const entries = body + sizeof(NtfsBrowser::Attr::IndexRoot);
  DWORD leafBytes = 0;
  for (const FakeIndexName& name : rootNames)
  {
    leafBytes += WriteFilenameEntry(entries + leafBytes, name);
  }

  auto& e1 =
      *reinterpret_cast<NtfsBrowser::Data::IndexEntry*>(entries + leafBytes);
  e1.mft_index = 0;
  e1.mft_sn = 0;
  e1.stream_size = 0;
  e1.flags = NtfsBrowser::Flag::IndexEntry::SUBNODE |
             NtfsBrowser::Flag::IndexEntry::LAST;
  e1.size = static_cast<WORD>(offsetof(NtfsBrowser::Data::IndexEntry, stream) +
                              sizeof(ULONGLONG));
  auto& subNodeVcn = *reinterpret_cast<ULONGLONG*>(
      reinterpret_cast<BYTE*>(&e1) + e1.size - sizeof(ULONGLONG));
  subNodeVcn = 0;

  root.total_entry_size = leafBytes + e1.size;
  root.alloc_entry_size = leafBytes + e1.size;
  root.flags = 0;

  rootAttr.attr_size =
      static_cast<DWORD>(sizeof(NtfsBrowser::Attr::IndexRoot)) + leafBytes +
      e1.size;
  rootAttr.header.total_size =
      static_cast<DWORD>(sizeof(rootAttr)) + rootAttr.attr_size;

  offset += rootAttr.header.total_size;
  offset += WriteNonResidentAttr(record, offset, AttrType::INDEX_ALLOCATION,
                                 compUnitSize, realSize, runs, overrides);

  WriteEndOfAttributesMarker(record, offset);
  return record;
}

// Lays "clusterBytes" down over the real runs of "runs", in run order, growing
// "image" to hold them; sparse runs consume no bytes and stay zero-filled.
void LayRunBytes(std::vector<BYTE>& image, const std::vector<FakeDataRun>& runs,
                 const std::vector<BYTE>& clusterBytes)
{
  // Grow the image so every real run fits, rounded up to FULL_CACHE's whole
  // 64KiB read block - same reasoning as
  // BuildFakeNtfsImageWithDeepIndexBlockChain().
  constexpr size_t kFullCacheReadBlockSize = 64 * 1024;
  size_t highestEnd = image.size();
  for (const FakeDataRun& run : runs)
  {
    if (run.lcn)
    {
      const size_t end = (static_cast<size_t>(*run.lcn) + run.clusters) *
                         static_cast<size_t>(kClusterSize);
      highestEnd = (end > highestEnd) ? end : highestEnd;
    }
  }
  const size_t alignedEnd =
      ((highestEnd + kFullCacheReadBlockSize - 1) / kFullCacheReadBlockSize) *
      kFullCacheReadBlockSize;
  if (image.size() < alignedEnd)
  {
    image.resize(alignedEnd, 0);
  }

  size_t written = 0;
  for (const FakeDataRun& run : runs)
  {
    if (!run.lcn || written >= clusterBytes.size())
    {
      continue;
    }

    const size_t capacity =
        static_cast<size_t>(run.clusters) * static_cast<size_t>(kClusterSize);
    const size_t left = clusterBytes.size() - written;
    const size_t chunk = (left < capacity) ? left : capacity;
    std::memcpy(image.data() + static_cast<size_t>(*run.lcn) *
                                   static_cast<size_t>(kClusterSize),
                clusterBytes.data() + written, chunk);
    written += chunk;
  }
}

// Same volume as BuildFakeNtfsImage(), with the root record (#5) replaced by
// "record" and "clusterBytes" laid down over its real runs, in run order;
// sparse runs consume no bytes and stay zero-filled.
std::vector<BYTE> BuildCompressionImage(const FakeRecord& record,
                                        const std::vector<FakeDataRun>& runs,
                                        const std::vector<BYTE>& clusterBytes)
{
  std::vector<BYTE> image = BuildFakeNtfsImage();

  const DWORD mftAddr = static_cast<DWORD>(kMftLcn) * kClusterSize;
  const size_t rootOffset = mftAddr + static_cast<size_t>(kFakeFileRecordSize) *
                                          static_cast<size_t>(MftIdx::ROOT);
  std::memcpy(image.data() + rootOffset, record.data(), record.size());

  LayRunBytes(image, runs, clusterBytes);
  return image;
}

// The malformed LZNT1 bytes both corrupt-compression fixtures use: a
// well-formed compressed chunk header, then a compressed word whose
// displacement (1) reaches before anything has been decompressed yet.
std::vector<BYTE> MakeCorruptLznt1Chunk()
{
  return {0x02, 0xb0, 0x01, 0x00, 0x00};
}

// The 1024-byte "INDX"-signed index block a compressed $INDEX_ALLOCATION
// decompresses to: "names" as leaf FILE_NAME entries plus the terminating
// entry. Built standalone since it is the *decompressed* content, wrapped
// into an LZNT1 chunk by the caller.
std::vector<BYTE> MakeIndexBlockContent(std::span<const FakeIndexName> names)
{
  std::vector<BYTE> content(kFakeFileRecordSize, 0);

  BYTE* const blockStart = content.data();
  auto& block = *reinterpret_cast<NtfsBrowser::Data::IndexBlock*>(blockStart);
  block.magic = kIndexBlockMagic;
  // Points offset_of_us at the fixup slot itself, so PatchUS() trivially
  // succeeds - same technique as BuildFakeNtfsImageWithGapCollationSubNode().
  block.offset_of_us = static_cast<WORD>(kFakeFileRecordSize - 4);
  block.size_of_us = 2;
  block.vcn = 0;
  block.entry_offset =
      static_cast<DWORD>((blockStart + sizeof(NtfsBrowser::Data::IndexBlock)) -
                         reinterpret_cast<BYTE*>(&block.entry_offset));
  block.not_leaf = 0;

  BYTE* const body = blockStart + sizeof(NtfsBrowser::Data::IndexBlock);

  DWORD leafBytes = 0;
  for (const FakeIndexName& name : names)
  {
    leafBytes += WriteFilenameEntry(body + leafBytes, name);
  }

  auto& last =
      *reinterpret_cast<NtfsBrowser::Data::IndexEntry*>(body + leafBytes);
  last.flags = NtfsBrowser::Flag::IndexEntry::LAST;
  last.stream_size = 0;
  last.size = static_cast<WORD>(reinterpret_cast<BYTE*>(&last.stream) -
                                reinterpret_cast<BYTE*>(&last));

  block.total_entry_size = leafBytes + last.size;
  block.alloc_entry_size = block.total_entry_size;

  return content;
}

// The single "Comp" entry every compressed $INDEX_ALLOCATION fixture but
// the surrogate-pair one decompresses to.
std::vector<BYTE> MakeCompressedIndexBlockContent()
{
  const FakeIndexName comp{
      .name = std::wstring_view(kCompressedIndexEntryName,
                                kCompressedIndexEntryNameLength),
      .mft_ref = kCompressedIndexEntryMftRef,
      .directory = false};
  return MakeIndexBlockContent(std::span(&comp, 1));
}

}  // namespace

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

std::vector<BYTE> BuildFakeNtfsImageWithMinimalVolumeInformation()
{
  std::vector<BYTE> image = BuildFakeNtfsImage();

  const DWORD mftAddr = static_cast<DWORD>(kMftLcn) * kClusterSize;
  const size_t offset = mftAddr + static_cast<size_t>(kFakeFileRecordSize) *
                                      static_cast<size_t>(MftIdx::VOLUME);
  const FakeRecord record =
      MakeVolumeRecordSized(kMinimalVolumeInformationSize);
  std::memcpy(image.data() + offset, record.data(), record.size());

  return image;
}

std::vector<BYTE> BuildFakeNtfsImageWithVolumeName()
{
  std::vector<BYTE> image = BuildFakeNtfsImage();

  const DWORD mftAddr = static_cast<DWORD>(kMftLcn) * kClusterSize;
  const size_t offset = mftAddr + static_cast<size_t>(kFakeFileRecordSize) *
                                      static_cast<size_t>(MftIdx::VOLUME);
  const FakeRecord record = MakeVolumeRecordWithName(kFakeVolumeName);
  std::memcpy(image.data() + offset, record.data(), record.size());

  return image;
}

std::vector<BYTE> BuildFakeNtfsImageWithLegacyStandardInformation()
{
  std::vector<BYTE> image = BuildFakeNtfsImage();

  const DWORD mftAddr = static_cast<DWORD>(kMftLcn) * kClusterSize;
  const size_t offset =
      mftAddr + static_cast<size_t>(kFakeFileRecordSize) *
                    static_cast<size_t>(kLegacyStandardInformationRecordIdx);
  const FakeRecord record =
      MakeStandardInformationRecordSized(kLegacyStandardInformationSize);
  std::memcpy(image.data() + offset, record.data(), record.size());

  return image;
}

std::vector<BYTE> BuildFakeNtfsImageWithLegacyStandardInformationOnRoot()
{
  std::vector<BYTE> image = BuildFakeNtfsImage();

  const DWORD mftAddr = static_cast<DWORD>(kMftLcn) * kClusterSize;
  const size_t offset = mftAddr + static_cast<size_t>(kFakeFileRecordSize) *
                                      static_cast<size_t>(MftIdx::ROOT);
  const FakeRecord record =
      MakeStandardInformationRecordSized(kLegacyStandardInformationSize);
  std::memcpy(image.data() + offset, record.data(), record.size());

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

std::vector<BYTE> BuildFakeNtfsImageWithForgedIndexBlock()
{
  std::vector<BYTE> image = BuildFakeNtfsImage();

  // Only this fixture's directory needs an index block bigger than 1
  // cluster - patch the shared BPB in place (both fields are DWORD but
  // ntfs-volume.cpp::ParseBootSector() only ever consults their low byte,
  // truncated to a signed char) rather than duplicating
  // BuildFakeNtfsImage()'s whole boot sector setup.
  auto& bpb = *reinterpret_cast<NtfsBrowser::Data::NtfsBpb*>(image.data());
  bpb.clusters_per_index_block =
      static_cast<DWORD>(kForgedIndexBlockSize / kClusterSize);

  const DWORD mftAddr = static_cast<DWORD>(kMftLcn) * kClusterSize;
  const size_t dirOffset =
      mftAddr + static_cast<size_t>(kFakeFileRecordSize) * kIndexAllocDirIdx;
  const FakeRecord dirRecord = MakeIndexAllocDirRecord();
  std::memcpy(image.data() + dirOffset, dirRecord.data(), dirRecord.size());

  const size_t blockOffset =
      static_cast<size_t>(kForgedIndexBlockLcn) * kClusterSize;
  if (image.size() < blockOffset + kForgedIndexBlockSize)
  {
    image.resize(blockOffset + kForgedIndexBlockSize, 0);
  }

  auto& block = *reinterpret_cast<NtfsBrowser::Data::IndexBlock*>(image.data() +
                                                                  blockOffset);
  std::memset(&block, 0, sizeof(block));
  block.magic = kIndexBlockMagic;
  block.offset_of_us = kForgedIndexBlockOffsetOfUs;
  // size_of_us is never checked; this value only keeps the fixture plausible.
  block.size_of_us =
      static_cast<WORD>(kForgedIndexBlockSize / kBytesPerSector + 1);
  block.vcn = 0;
  block.not_leaf = 0;

  return image;
}

std::vector<BYTE> BuildFakeNtfsImageWithTinyIndexBlock()
{
  std::vector<BYTE> image = BuildFakeNtfsImage();

  // ParseBootSector() reads this DWORD field's low byte as a signed char.
  auto& bpb = *reinterpret_cast<NtfsBrowser::Data::NtfsBpb*>(image.data());
  bpb.clusters_per_index_block = kTinyClustersPerIndexBlock;

  return image;
}

std::vector<BYTE> BuildFakeNtfsImageWithOversizedIndexBlock()
{
  std::vector<BYTE> image = BuildFakeNtfsImage();

  // ParseBootSector() reads this DWORD field's low byte as a signed char.
  auto& bpb = *reinterpret_cast<NtfsBrowser::Data::NtfsBpb*>(image.data());
  bpb.clusters_per_index_block = kOversizedClustersPerIndexBlock;

  return image;
}

std::vector<BYTE> BuildFakeNtfsImageWithOversizedFileRecord()
{
  std::vector<BYTE> image = BuildFakeNtfsImage();

  // ParseBootSector() reads this DWORD field's low byte as a signed char.
  auto& bpb = *reinterpret_cast<NtfsBrowser::Data::NtfsBpb*>(image.data());
  bpb.clusters_per_file_record = kOversizedClustersPerFileRecord;

  return image;
}

std::vector<BYTE> BuildFakeNtfsImageWithFileRecordSizeTooBig()
{
  std::vector<BYTE> image = BuildFakeNtfsImage();

  // ParseBootSector() reads this DWORD field's low byte as a signed char.
  auto& bpb = *reinterpret_cast<NtfsBrowser::Data::NtfsBpb*>(image.data());
  bpb.clusters_per_file_record = kFileRecordSizeTooBigClustersPerFileRecord;

  return image;
}

std::vector<BYTE> BuildFakeNtfsImageWithHugeMftLcn()
{
  std::vector<BYTE> image = BuildFakeNtfsImage();

  // lcn_mft is otherwise only ever set once, in BuildFakeNtfsImage() itself.
  auto& bpb = *reinterpret_cast<NtfsBrowser::Data::NtfsBpb*>(image.data());
  bpb.lcn_mft = kHugeMftLcn;

  return image;
}

std::vector<BYTE> BuildFakeNtfsImageWithMultiTypeAttributeListDirectory()
{
  std::vector<BYTE> image = BuildFakeNtfsImage();

  const DWORD mftAddr = static_cast<DWORD>(kMftLcn) * kClusterSize;
  const auto putRecord = [&](ULONGLONG idx, const FakeRecord& record)
  {
    const size_t offset = mftAddr + static_cast<size_t>(kFakeFileRecordSize) *
                                        static_cast<size_t>(idx);
    std::memcpy(image.data() + offset, record.data(), record.size());
  };

  putRecord(kAttrListMultiTypeDirIdx, MakeAttributeListTwoTypesDirRecord());
  putRecord(kMultiTypeExtensionIdx, MakeIndexRootAndAllocExtensionRecord());

  return image;
}

std::vector<BYTE> BuildFakeNtfsImageWithAttributeListDirectoryChainReused()
{
  std::vector<BYTE> image = BuildFakeNtfsImageWithAttributeListDirectory();

  const DWORD mftAddr = static_cast<DWORD>(kMftLcn) * kClusterSize;
  const size_t offset =
      mftAddr + static_cast<size_t>(kFakeFileRecordSize) *
                    static_cast<size_t>(kAttributeListDirIdx2);
  const FakeRecord record = MakeAttributeListOnlyDirRecord();
  std::memcpy(image.data() + offset, record.data(), record.size());

  return image;
}

std::vector<BYTE> BuildFakeNtfsImageWithFragmentedAttributeListDirectory()
{
  std::vector<BYTE> image = BuildFakeNtfsImage();

  const DWORD mftAddr = static_cast<DWORD>(kMftLcn) * kClusterSize;
  const auto putRecord = [&](ULONGLONG idx, const FakeRecord& record)
  {
    const size_t offset = mftAddr + static_cast<size_t>(kFakeFileRecordSize) *
                                        static_cast<size_t>(idx);
    std::memcpy(image.data() + offset, record.data(), record.size());
  };

  putRecord(kUafAttrListDirIdx, MakeFragmentedAttributeListDirRecord());
  putRecord(kUafExtensionIdx0,
            MakeIndexAllocationOnlyExtensionRecord(kUafRealSizeSentinels[0]));
  putRecord(kUafExtensionIdx1,
            MakeIndexAllocationOnlyExtensionRecord(kUafRealSizeSentinels[1]));
  putRecord(kUafExtensionIdx2,
            MakeIndexAllocationOnlyExtensionRecord(kUafRealSizeSentinels[2]));
  putRecord(kUafExtensionIdx3,
            MakeIndexAllocationOnlyExtensionRecord(kUafRealSizeSentinels[3]));

  return image;
}

std::vector<BYTE> BuildFakeNtfsImageWithCorruptMftRecord()
{
  std::vector<BYTE> image = BuildFakeNtfsImage();

  // Zeroing the magic makes ParseFileRecord() fail on this record alone.
  const DWORD mftAddr = static_cast<DWORD>(kMftLcn) * kClusterSize;
  const size_t offset = mftAddr + static_cast<size_t>(kFakeFileRecordSize) *
                                      static_cast<size_t>(MftIdx::MFT);
  std::memset(image.data() + offset, 0, kFakeFileRecordSize);

  return image;
}

std::vector<BYTE> BuildFakeNtfsImageWithAttrNameExceedsTotalSize()
{
  std::vector<BYTE> image = BuildFakeNtfsImage();

  const DWORD mftAddr = static_cast<DWORD>(kMftLcn) * kClusterSize;
  const size_t offset =
      mftAddr + static_cast<size_t>(kFakeFileRecordSize) *
                    static_cast<size_t>(kAttrNameExceedsTotalSizeRecordIdx);
  const FakeRecord record = MakeAttrNameExceedsTotalSizeRecord();
  std::memcpy(image.data() + offset, record.data(), record.size());

  return image;
}

std::vector<BYTE> BuildFakeNtfsImageWithAttrOffsetOutOfBounds()
{
  std::vector<BYTE> image = BuildFakeNtfsImage();

  // offset_of_attr is per-record, unlike the BPB fields patched above.
  const DWORD mftAddr = static_cast<DWORD>(kMftLcn) * kClusterSize;
  const size_t rootOffset = mftAddr + static_cast<size_t>(kFakeFileRecordSize) *
                                          static_cast<size_t>(MftIdx::ROOT);
  auto& header = *reinterpret_cast<NtfsBrowser::FileRecordHeader::Data*>(
      &image[rootOffset]);
  header.offset_of_attr = kAttrOffsetOutOfBounds;

  return image;
}

std::vector<BYTE> BuildFakeNtfsImageWithSmallResidentData()
{
  std::vector<BYTE> image = BuildFakeNtfsImage();

  // Overwrites the whole root record (#5), not just a single field.
  const DWORD mftAddr = static_cast<DWORD>(kMftLcn) * kClusterSize;
  const size_t rootOffset = mftAddr + static_cast<size_t>(kFakeFileRecordSize) *
                                          static_cast<size_t>(MftIdx::ROOT);
  const FakeRecord record = MakeSmallResidentDataRecord();
  std::memcpy(image.data() + rootOffset, record.data(), record.size());

  return image;
}

std::vector<BYTE> BuildFakeNtfsImageWithAttributeListShortRead()
{
  std::vector<BYTE> image = BuildFakeNtfsImage();

  // Overwrites the whole root record (#5), not just a single field.
  const DWORD mftAddr = static_cast<DWORD>(kMftLcn) * kClusterSize;
  const size_t rootOffset = mftAddr + static_cast<size_t>(kFakeFileRecordSize) *
                                          static_cast<size_t>(MftIdx::ROOT);
  const FakeRecord record = MakeAttributeListShortReadRecord();
  std::memcpy(image.data() + rootOffset, record.data(), record.size());

  return image;
}

std::vector<BYTE> BuildFakeNtfsImageWithAttributeListCycle()
{
  std::vector<BYTE> image = BuildFakeNtfsImage();

  const DWORD mftAddr = static_cast<DWORD>(kMftLcn) * kClusterSize;
  const auto putRecord = [&](ULONGLONG idx, const FakeRecord& record)
  {
    const size_t offset = mftAddr + static_cast<size_t>(kFakeFileRecordSize) *
                                        static_cast<size_t>(idx);
    std::memcpy(image.data() + offset, record.data(), record.size());
  };

  putRecord(static_cast<ULONGLONG>(MftIdx::ROOT),
            MakeAttributeListCycleRecord(kAttrListCycleExtIdx));
  putRecord(kAttrListCycleExtIdx,
            MakeAttributeListCycleRecord(static_cast<ULONGLONG>(MftIdx::ROOT)));

  return image;
}

std::vector<BYTE> BuildFakeNtfsImageWithTightlyPackedAttributeListDirectory()
{
  std::vector<BYTE> image = BuildFakeNtfsImage();

  const DWORD mftAddr = static_cast<DWORD>(kMftLcn) * kClusterSize;
  const auto putRecord = [&](ULONGLONG idx, const FakeRecord& record)
  {
    const size_t offset = mftAddr + static_cast<size_t>(kFakeFileRecordSize) *
                                        static_cast<size_t>(idx);
    std::memcpy(image.data() + offset, record.data(), record.size());
  };

  putRecord(kAttrListTightPackDirIdx,
            MakeAttributeListTightlyPackedDirRecord());
  putRecord(kAttrListTightPackExtIdxA, MakeIndexRootExtensionRecord());
  putRecord(kAttrListTightPackExtIdxB,
            MakeIndexAllocationOnlyExtensionRecord(kAttrListTightPackRealSize));

  return image;
}

std::vector<BYTE> BuildFakeNtfsImageWithCorruptRootRecord()
{
  std::vector<BYTE> image = BuildFakeNtfsImage();

  // Zeroes the root directory's (#5) own record, so ParseFileRecord(ROOT)
  // fails while $Volume and $MFT stay valid.
  const DWORD mftAddr = static_cast<DWORD>(kMftLcn) * kClusterSize;
  const size_t rootOffset = mftAddr + static_cast<size_t>(kFakeFileRecordSize) *
                                          static_cast<size_t>(MftIdx::ROOT);
  std::memset(image.data() + rootOffset, 0, kFakeFileRecordSize);

  return image;
}

std::vector<BYTE> BuildFakeNtfsImageWithFragmentedMftInvalidRecord()
{
  std::vector<BYTE> image = BuildFakeNtfsImage();

  // VCN 0..kFragmentedMftInvalidRecordIdx, inclusive.
  constexpr DWORD kClusters =
      static_cast<DWORD>(kFragmentedMftInvalidRecordIdx) + 1;

  // Overwrites $MFT's own record with one whose DATA attribute has a real
  // data run.
  const DWORD mftAddr = static_cast<DWORD>(kMftLcn) * kClusterSize;
  const FakeRecord mftRecord =
      MakeMftRecordWithRealDataRun(kFragmentedMftDataRunLcn, kClusters);
  std::memcpy(image.data() + mftAddr, mftRecord.data(), mftRecord.size());

  // Written at the physical cluster the data run maps this record's VCN to.
  const size_t forgedOffset = (static_cast<size_t>(kFragmentedMftDataRunLcn) +
                               kFragmentedMftInvalidRecordIdx) *
                              kClusterSize;
  if (image.size() < forgedOffset + kFakeFileRecordSize)
  {
    image.resize(forgedOffset + kFakeFileRecordSize, 0);
  }
  const FakeRecord forgedRecord = MakeInvalidOffsetOfUsRecord();
  std::memcpy(image.data() + forgedOffset, forgedRecord.data(),
              forgedRecord.size());

  return image;
}

std::vector<BYTE> BuildFakeNtfsImageWithNamedDataStream()
{
  std::vector<BYTE> image = BuildFakeNtfsImage();

  // Overwrites the whole root record (#5), not just a single field.
  const DWORD mftAddr = static_cast<DWORD>(kMftLcn) * kClusterSize;
  const size_t rootOffset = mftAddr + static_cast<size_t>(kFakeFileRecordSize) *
                                          static_cast<size_t>(MftIdx::ROOT);
  const FakeRecord record = MakeNamedDataStreamRecord();
  std::memcpy(image.data() + rootOffset, record.data(), record.size());

  return image;
}

std::vector<BYTE> BuildFakeNtfsImageWithIndexRootVariants()
{
  std::vector<BYTE> image = BuildFakeNtfsImage();

  const DWORD mftAddr = static_cast<DWORD>(kMftLcn) * kClusterSize;
  const auto putRecord = [&](ULONGLONG idx, const FakeRecord& record)
  {
    const size_t offset = mftAddr + static_cast<size_t>(kFakeFileRecordSize) *
                                        static_cast<size_t>(idx);
    std::memcpy(image.data() + offset, record.data(), record.size());
  };

  putRecord(kIndexRootVariantADirIdx,
            MakeIndexRootDirRecord(kIndexRootVariantAName,
                                   kIndexRootVariantAMftRef,
                                   kIndexRootVariantADirIdx));
  putRecord(kIndexRootVariantBDirIdx,
            MakeIndexRootDirRecord(kIndexRootVariantBName,
                                   kIndexRootVariantBMftRef,
                                   kIndexRootVariantBDirIdx));

  return image;
}

std::vector<BYTE> BuildFakeNtfsImageWithRootIndexRootEntry()
{
  std::vector<BYTE> image = BuildFakeNtfsImage();

  // Overwrites the whole root record (#5), not just a single field.
  const DWORD mftAddr = static_cast<DWORD>(kMftLcn) * kClusterSize;
  const size_t rootOffset = mftAddr + static_cast<size_t>(kFakeFileRecordSize) *
                                          static_cast<size_t>(MftIdx::ROOT);
  const FakeRecord record = MakeIndexRootExtensionRecord();
  std::memcpy(image.data() + rootOffset, record.data(), record.size());

  return image;
}

std::vector<BYTE> BuildFakeNtfsImageWithGapCollationSubNode()
{
  std::vector<BYTE> image = BuildFakeNtfsImage();

  // Overwrites the whole root record (#5), not just a single field.
  const DWORD mftAddr = static_cast<DWORD>(kMftLcn) * kClusterSize;
  const size_t rootOffset = mftAddr + static_cast<size_t>(kFakeFileRecordSize) *
                                          static_cast<size_t>(MftIdx::ROOT);
  const FakeRecord record = MakeRootRecordWithGapCollationSubNode();
  std::memcpy(image.data() + rootOffset, record.data(), record.size());

  // The sub-node itself: one real index block holding
  // kGapCollationSearchName as its only leaf entry.
  const size_t blockOffset =
      static_cast<size_t>(kGapCollationIndexBlockLcn) * kClusterSize;
  if (image.size() < blockOffset + kClusterSize)
  {
    image.resize(blockOffset + kClusterSize, 0);
  }

  BYTE* const blockStart = image.data() + blockOffset;
  auto& block = *reinterpret_cast<NtfsBrowser::Data::IndexBlock*>(blockStart);
  std::memset(&block, 0, sizeof(block));
  block.magic = kIndexBlockMagic;
  // Points at the block's own last 2 bytes, so PatchUS() succeeds
  // trivially without a real fixup array.
  block.offset_of_us = static_cast<WORD>(kClusterSize - 4);
  block.size_of_us = 2;
  block.vcn = 0;
  block.entry_offset =
      static_cast<DWORD>((blockStart + sizeof(NtfsBrowser::Data::IndexBlock)) -
                         reinterpret_cast<BYTE*>(&block.entry_offset));
  block.not_leaf = 0;

  BYTE* body = blockStart + sizeof(NtfsBrowser::Data::IndexBlock);

  // Entry 1: the real leaf entry, a plain leaf with no sub-node.
  auto& e1 = *reinterpret_cast<NtfsBrowser::Data::IndexEntry*>(body);
  e1.mft_index = kGapCollationLeafMftRef;
  e1.mft_sn = 1;

  auto& fn1 = *reinterpret_cast<NtfsBrowser::Attr::Filename*>(&e1.stream);
  fn1.parent_ref = static_cast<ULONGLONG>(MftIdx::ROOT);
  fn1.flags = NtfsBrowser::Flag::Filename::NONE;
  fn1.name_length = kGapCollationSearchNameLength;
  fn1.name_space = NtfsBrowser::Flag::FilenameNamespace::WIN_32;
  for (BYTE i = 0; i < fn1.name_length; i++)
  {
    fn1.name[i] = static_cast<WORD>(kGapCollationSearchName[i]);
  }

  e1.stream_size =
      static_cast<WORD>(reinterpret_cast<BYTE*>(&fn1.name[fn1.name_length]) -
                        reinterpret_cast<BYTE*>(&fn1));
  e1.size = static_cast<WORD>(reinterpret_cast<BYTE*>(&e1.stream) -
                              reinterpret_cast<BYTE*>(&e1) + e1.stream_size);

  // Entry 2: the terminating entry - no name, no sub-node.
  auto& e2 = *reinterpret_cast<NtfsBrowser::Data::IndexEntry*>(body + e1.size);
  e2.flags = NtfsBrowser::Flag::IndexEntry::LAST;
  e2.stream_size = 0;
  e2.size = static_cast<WORD>(reinterpret_cast<BYTE*>(&e2.stream) -
                              reinterpret_cast<BYTE*>(&e2));

  block.total_entry_size = static_cast<DWORD>(e1.size) + e2.size;
  block.alloc_entry_size = block.total_entry_size;

  return image;
}

std::vector<BYTE> BuildFakeNtfsImageWithDeepIndexBlockChain()
{
  std::vector<BYTE> image = BuildFakeNtfsImage();

  // Replace the root directory's (#5) whole record in place.
  const DWORD mftAddr = static_cast<DWORD>(kMftLcn) * kClusterSize;
  const size_t rootOffset = mftAddr + static_cast<size_t>(kFakeFileRecordSize) *
                                          static_cast<size_t>(MftIdx::ROOT);
  const FakeRecord record = MakeIndexBlockChainRootRecord();
  std::memcpy(image.data() + rootOffset, record.data(), record.size());

  // The chain itself: kIndexBlockChainLength contiguous blocks starting at
  // kIndexBlockChainLcn, one cluster each.
  const size_t chainOffset =
      static_cast<size_t>(kIndexBlockChainLcn) * kClusterSize;
  const size_t chainBytes =
      static_cast<size_t>(kIndexBlockChainLength) * kClusterSize;
  // FULL_CACHE always reads a whole 64KiB-aligned block, so the image must
  // extend past the chain's real end or its last read fails outright.
  constexpr size_t kFullCacheReadBlockSize = 64 * 1024;
  const size_t chainEnd = chainOffset + chainBytes;
  const size_t alignedChainEnd =
      ((chainEnd + kFullCacheReadBlockSize - 1) / kFullCacheReadBlockSize) *
      kFullCacheReadBlockSize;
  if (image.size() < alignedChainEnd)
  {
    image.resize(alignedChainEnd, 0);
  }

  for (DWORD vcn = 0; vcn < kIndexBlockChainLength; vcn++)
  {
    BYTE* const blockStart =
        image.data() + chainOffset + static_cast<size_t>(vcn) * kClusterSize;
    auto& block = *reinterpret_cast<NtfsBrowser::Data::IndexBlock*>(blockStart);
    std::memset(&block, 0, sizeof(block));
    block.magic = kIndexBlockMagic;
    // Points offset_of_us at the fixup slot itself, valid for every block.
    block.offset_of_us = static_cast<WORD>(kClusterSize - 4);
    block.size_of_us = 2;
    block.vcn = vcn;
    block.entry_offset = static_cast<DWORD>(
        (blockStart + sizeof(NtfsBrowser::Data::IndexBlock)) -
        reinterpret_cast<BYTE*>(&block.entry_offset));

    BYTE* body = blockStart + sizeof(NtfsBrowser::Data::IndexBlock);
    auto& e1 = *reinterpret_cast<NtfsBrowser::Data::IndexEntry*>(body);

    const bool isLeaf = (vcn == kIndexBlockChainLength - 1);
    if (!isLeaf)
    {
      // Intermediate block: a lone, nameless entry pointing at the next VCN.
      block.not_leaf = 1;
      e1.mft_index = 0;
      e1.mft_sn = 0;
      e1.stream_size = 0;
      e1.flags = NtfsBrowser::Flag::IndexEntry::SUBNODE |
                 NtfsBrowser::Flag::IndexEntry::LAST;
      e1.size = static_cast<WORD>(
          offsetof(NtfsBrowser::Data::IndexEntry, stream) + sizeof(ULONGLONG));
      auto& subNodeVcn = *reinterpret_cast<ULONGLONG*>(
          reinterpret_cast<BYTE*>(&e1) + e1.size - sizeof(ULONGLONG));
      subNodeVcn = vcn + 1;
    }
    else
    {
      // Deepest block: the real, named leaf entry, reached by depth alone.
      block.not_leaf = 0;
      e1.mft_index = kIndexBlockChainLeafMftRef;
      e1.mft_sn = 1;

      auto& fn1 = *reinterpret_cast<NtfsBrowser::Attr::Filename*>(&e1.stream);
      fn1.parent_ref = static_cast<ULONGLONG>(MftIdx::ROOT);
      fn1.flags = NtfsBrowser::Flag::Filename::NONE;
      fn1.name_length = kIndexBlockChainLeafNameLength;
      fn1.name_space = NtfsBrowser::Flag::FilenameNamespace::WIN_32;
      for (BYTE i = 0; i < fn1.name_length; i++)
      {
        fn1.name[i] = static_cast<WORD>(kIndexBlockChainLeafName[i]);
      }

      e1.stream_size = static_cast<WORD>(
          reinterpret_cast<BYTE*>(&fn1.name[fn1.name_length]) -
          reinterpret_cast<BYTE*>(&fn1));
      e1.flags = NtfsBrowser::Flag::IndexEntry::LAST;
      e1.size =
          static_cast<WORD>(reinterpret_cast<BYTE*>(&e1.stream) -
                            reinterpret_cast<BYTE*>(&e1) + e1.stream_size);
    }

    block.total_entry_size = e1.size;
    block.alloc_entry_size = e1.size;
  }

  return image;
}

std::vector<BYTE> MakeUncompressedLznt1Chunk(std::span<const BYTE> payload)
{
  // [MS-XCA] section 2.5.3: input streams are compressed in units of 4096
  // bytes, so a single chunk never carries more than that (and a chunk with
  // no payload at all is not representable - the declared size is
  // payload.size() - 1).
  assert(!payload.empty() && payload.size() <= NtfsBrowser::Lznt1::kChunkSize);

  // Header: bit 15 clear (uncompressed), bits 14-12 == 3 (signature), bits
  // 11-0 == payload size - 1 (the whole chunk's size, header included, minus
  // three) - [MS-XCA] section 2.5.1.2.
  const auto header =
      static_cast<WORD>(0x3000U | static_cast<unsigned>(payload.size() - 1));

  std::vector<BYTE> chunk;
  chunk.reserve(payload.size() + 2);
  chunk.push_back(static_cast<BYTE>(header & 0xFFU));
  chunk.push_back(static_cast<BYTE>(header >> 8U));
  chunk.insert(chunk.end(), payload.begin(), payload.end());
  return chunk;
}

std::vector<BYTE> CompressionFixturePattern(size_t size)
{
  std::vector<BYTE> pattern(size, 0);
  for (size_t i = 0; i < size; i++)
  {
    // Deliberately not a byte-aligned cycle, so a fixture whose content got
    // shifted by a whole number of bytes/clusters still compares unequal.
    pattern[i] = static_cast<BYTE>((i * 31U + 7U) % 251U);
  }
  return pattern;
}

std::vector<BYTE> BuildFakeNtfsImageWithCompressedFile()
{
  // One real cluster (the [MS-XCA] section 3.3 worked example's 59
  // compressed bytes) plus a sparse pad up to a whole compression unit -
  // 1 < kCompressionUnitClusters real clusters is exactly what marks a unit
  // as compressed rather than stored.
  const std::vector<FakeDataRun> runs{{kCompressedDataLcn, 1},
                                      {{}, kCompressionUnitClusters - 1}};

  const FakeRecord record = MakeNonResidentDataRecord(
      NtfsBrowser::Flag::StdInfoPermission::ARCHIVE |
          NtfsBrowser::Flag::StdInfoPermission::COMPRESSED,
      kCompressionUnitSizeShift, kXcaLznt1ExampleDecompressedSize, runs);

  const std::vector<BYTE> clusterBytes(kXcaLznt1ExampleCompressed.begin(),
                                       kXcaLznt1ExampleCompressed.end());
  return BuildCompressionImage(record, runs, clusterBytes);
}

std::vector<BYTE> BuildFakeNtfsImageWithStoredCompressionUnit()
{
  // Real runs covering the whole unit, no sparse pad: a stored
  // (incompressible) unit, whose bytes must come back untouched.
  const std::vector<FakeDataRun> runs{
      {kCompressedDataLcn, kCompressionUnitClusters}};

  const FakeRecord record = MakeNonResidentDataRecord(
      NtfsBrowser::Flag::StdInfoPermission::ARCHIVE |
          NtfsBrowser::Flag::StdInfoPermission::COMPRESSED,
      kCompressionUnitSizeShift, kCompressionUnitSize, runs);

  return BuildCompressionImage(record, runs,
                               CompressionFixturePattern(kCompressionUnitSize));
}

std::vector<BYTE> BuildFakeNtfsImageWithSparseCompressionUnit()
{
  const std::vector<FakeDataRun> runs{{{}, kCompressionUnitClusters}};

  const FakeRecord record = MakeNonResidentDataRecord(
      NtfsBrowser::Flag::StdInfoPermission::ARCHIVE |
          NtfsBrowser::Flag::StdInfoPermission::COMPRESSED |
          NtfsBrowser::Flag::StdInfoPermission::SPARSE,
      kCompressionUnitSizeShift, kCompressionUnitSize, runs);

  return BuildCompressionImage(record, runs, {});
}

std::vector<BYTE> BuildFakeNtfsImageWithFragmentedCompressedFile()
{
  // Two non-contiguous single-cluster real runs (so the compressed bytes
  // genuinely span two Data::RunEntrys) plus a 2-cluster sparse pad.
  const std::vector<FakeDataRun> runs{{kCompressedDataLcn, 1},
                                      {kFragmentedCompressedDataLcn, 1},
                                      {{}, kCompressionUnitClusters - 2}};

  const FakeRecord record = MakeNonResidentDataRecord(
      NtfsBrowser::Flag::StdInfoPermission::ARCHIVE |
          NtfsBrowser::Flag::StdInfoPermission::COMPRESSED,
      kCompressionUnitSizeShift, kFragmentedCompressedPayloadSize, runs);

  const std::vector<BYTE> payload =
      CompressionFixturePattern(kFragmentedCompressedPayloadSize);
  return BuildCompressionImage(record, runs,
                               MakeUncompressedLznt1Chunk(payload));
}

std::vector<BYTE> BuildFakeNtfsImageWithTrailingPartialCompressionUnit()
{
  // Five real clusters then one sparse: unit 0 (VCN 0..3) is entirely real
  // (stored), and unit 1 (VCN 4..5 only - the attribute stops there) sees
  // the tail of that same run plus one hole, so it is a compressed unit
  // whose real extent comes from a PARTIAL run rather than a run of its own.
  const std::vector<FakeDataRun> runs{
      {kCompressedDataLcn, kCompressionUnitClusters + 1}, {{}, 1}};

  const FakeRecord record = MakeNonResidentDataRecord(
      NtfsBrowser::Flag::StdInfoPermission::ARCHIVE |
          NtfsBrowser::Flag::StdInfoPermission::COMPRESSED,
      kCompressionUnitSizeShift,
      kTrailingPartialUnitStoredSize + kTrailingPartialUnitTailSize, runs);

  // Clusters 0..3 hold unit 0's stored bytes verbatim; cluster 4 holds unit
  // 1's LZNT1 stream (a single hand-encoded uncompressed chunk).
  std::vector<BYTE> clusterBytes =
      CompressionFixturePattern(kTrailingPartialUnitStoredSize);
  const std::vector<BYTE> tail = MakeUncompressedLznt1Chunk(
      CompressionFixturePattern(kTrailingPartialUnitTailSize));
  clusterBytes.insert(clusterBytes.end(), tail.begin(), tail.end());

  return BuildCompressionImage(record, runs, clusterBytes);
}

std::vector<BYTE> BuildFakeNtfsImageWithCorruptCompressedUnit()
{
  const std::vector<FakeDataRun> runs{{kCompressedDataLcn, 1},
                                      {{}, kCompressionUnitClusters - 1}};

  const FakeRecord record = MakeNonResidentDataRecord(
      NtfsBrowser::Flag::StdInfoPermission::ARCHIVE |
          NtfsBrowser::Flag::StdInfoPermission::COMPRESSED,
      kCompressionUnitSizeShift, kXcaLznt1ExampleDecompressedSize, runs);

  return BuildCompressionImage(record, runs, MakeCorruptLznt1Chunk());
}

std::vector<BYTE> BuildFakeNtfsImageWithUnmappedCompressionUnit()
{
  // Only unit 0's clusters are actually mapped; last_vcn claims two units'
  // worth.
  const std::vector<FakeDataRun> runs{
      {kCompressedDataLcn, kCompressionUnitClusters}};

  const FakeRecord record = MakeNonResidentDataRecord(
      NtfsBrowser::Flag::StdInfoPermission::ARCHIVE |
          NtfsBrowser::Flag::StdInfoPermission::COMPRESSED,
      kCompressionUnitSizeShift, 2ULL * kCompressionUnitSize, runs,
      {.last_vcn = 2ULL * kCompressionUnitClusters - 1});

  return BuildCompressionImage(record, runs,
                               CompressionFixturePattern(kCompressionUnitSize));
}

std::vector<BYTE> BuildFakeNtfsImageWithRealClustersAfterHole()
{
  const std::vector<FakeDataRun> runs{
      {kCompressedDataLcn, 1}, {{}, 1}, {kFragmentedCompressedDataLcn, 2}};

  const FakeRecord record = MakeNonResidentDataRecord(
      NtfsBrowser::Flag::StdInfoPermission::ARCHIVE |
          NtfsBrowser::Flag::StdInfoPermission::COMPRESSED,
      kCompressionUnitSizeShift, kCompressionUnitSize, runs);

  // Content is irrelevant - the layout is rejected before anything is
  // decompressed - but a real LZNT1 stream keeps the fixture honest about
  // being otherwise plausible.
  const std::vector<BYTE> clusterBytes(kXcaLznt1ExampleCompressed.begin(),
                                       kXcaLznt1ExampleCompressed.end());
  return BuildCompressionImage(record, runs, clusterBytes);
}

std::vector<BYTE> BuildFakeNtfsImageWithShortDecompressedUnit()
{
  // One real cluster then seven sparse: unit 0 (VCN 0..3) is compressed,
  // unit 1 (VCN 4..7) a hole. real_size covers both, so unit 0 is interior
  // and must yield a whole kCompressionUnitSize bytes.
  const std::vector<FakeDataRun> runs{
      {kCompressedDataLcn, 1}, {{}, 2ULL * kCompressionUnitClusters - 1}};

  const FakeRecord record = MakeNonResidentDataRecord(
      NtfsBrowser::Flag::StdInfoPermission::ARCHIVE |
          NtfsBrowser::Flag::StdInfoPermission::COMPRESSED,
      kCompressionUnitSizeShift, 2ULL * kCompressionUnitSize, runs);

  return BuildCompressionImage(
      record, runs,
      MakeUncompressedLznt1Chunk(
          CompressionFixturePattern(kShortDecompressedUnitSize)));
}

std::vector<BYTE> BuildFakeNtfsImageWithCompressedAttrMissingCompressedSize()
{
  const std::vector<FakeDataRun> runs{{kCompressedDataLcn, 1},
                                      {{}, kCompressionUnitClusters - 1}};

  const FakeRecord record = MakeNonResidentDataRecord(
      NtfsBrowser::Flag::StdInfoPermission::ARCHIVE |
          NtfsBrowser::Flag::StdInfoPermission::COMPRESSED,
      kCompressionUnitSizeShift, kXcaLznt1ExampleDecompressedSize, runs,
      {.total_size = kCompressedAttrTruncatedTotalSize});

  const std::vector<BYTE> clusterBytes(kXcaLznt1ExampleCompressed.begin(),
                                       kXcaLznt1ExampleCompressed.end());
  return BuildCompressionImage(record, runs, clusterBytes);
}

std::vector<BYTE> BuildFakeNtfsImageWithCompUnitSizeOutOfRange()
{
  const std::vector<FakeDataRun> runs{{kCompressedDataLcn, 1},
                                      {{}, kCompressionUnitClusters - 1}};

  const FakeRecord record = MakeNonResidentDataRecord(
      NtfsBrowser::Flag::StdInfoPermission::ARCHIVE |
          NtfsBrowser::Flag::StdInfoPermission::COMPRESSED,
      kCompUnitSizeOutOfRangeShift, kXcaLznt1ExampleDecompressedSize, runs);

  const std::vector<BYTE> clusterBytes(kXcaLznt1ExampleCompressed.begin(),
                                       kXcaLznt1ExampleCompressed.end());
  return BuildCompressionImage(record, runs, clusterBytes);
}

std::vector<BYTE> BuildFakeNtfsImageWithOversizedCompressionUnit()
{
  const std::vector<FakeDataRun> runs{{kCompressedDataLcn, 1},
                                      {{}, kCompressionUnitClusters - 1}};

  const FakeRecord record = MakeNonResidentDataRecord(
      NtfsBrowser::Flag::StdInfoPermission::ARCHIVE |
          NtfsBrowser::Flag::StdInfoPermission::COMPRESSED,
      kOversizedCompUnitSizeShift, kXcaLznt1ExampleDecompressedSize, runs);

  const std::vector<BYTE> clusterBytes(kXcaLznt1ExampleCompressed.begin(),
                                       kXcaLznt1ExampleCompressed.end());
  return BuildCompressionImage(record, runs, clusterBytes);
}

std::vector<BYTE> BuildFakeNtfsImageWithMisalignedCompressedStartVcn()
{
  const std::vector<FakeDataRun> runs{{kCompressedDataLcn, 1},
                                      {{}, kCompressionUnitClusters - 1}};

  const FakeRecord record = MakeNonResidentDataRecord(
      NtfsBrowser::Flag::StdInfoPermission::ARCHIVE |
          NtfsBrowser::Flag::StdInfoPermission::COMPRESSED,
      kCompressionUnitSizeShift, kXcaLznt1ExampleDecompressedSize, runs,
      {.start_vcn = kMisalignedCompressedStartVcn});

  const std::vector<BYTE> clusterBytes(kXcaLznt1ExampleCompressed.begin(),
                                       kXcaLznt1ExampleCompressed.end());
  return BuildCompressionImage(record, runs, clusterBytes);
}

// Writes a resident $EFS attribute holding "body" and returns its total_size.
DWORD WriteResidentEfsAttr(FakeRecord& record, DWORD offset,
                           std::span<const BYTE> body)
{
  constexpr std::wstring_view kName = L"$EFS";
  auto& attr =
      *reinterpret_cast<NtfsBrowser::Attr::HeaderResident*>(&record[offset]);
  attr.header.type = AttrType::LOGGED_UTILITY_STREAM;
  attr.header.non_resident = 0;
  attr.header.flags = 0;
  attr.header.id = 0;
  attr.header.name_length = static_cast<BYTE>(kName.size());
  attr.header.name_offset = static_cast<WORD>(sizeof(attr));
  attr.attr_size = static_cast<DWORD>(body.size());
  attr.attr_offset =
      static_cast<WORD>(sizeof(attr) + (kName.size() * sizeof(wchar_t)));
  attr.header.total_size =
      static_cast<DWORD>(attr.attr_offset) + attr.attr_size;

  std::memcpy(&record[offset + attr.header.name_offset], kName.data(),
              kName.size() * sizeof(wchar_t));
  std::memcpy(&record[offset + attr.attr_offset], body.data(), body.size());
  return attr.header.total_size;
}

std::vector<BYTE>
    BuildFakeNtfsImageWithEncryptedFile(const FakeEncryptedFile& file)
{
  FakeRecord record =
      MakeRecordHeader(kAttrOffset, NtfsBrowser::Flag::FileRecord::INUSE);

  DWORD offset = kAttrOffset;
  offset += WriteStandardInformationAttr(
      record, offset,
      NtfsBrowser::Flag::StdInfoPermission::ARCHIVE |
          NtfsBrowser::Flag::StdInfoPermission::ENCRYPTED);

  for (const FakeEncryptedStream& stream : file.streams)
  {
    WORD flags = stream.flagged_encrypted ? NtfsBrowser::Efs::kAttrFlagEncrypted
                                          : static_cast<WORD>(0);
    if (stream.flagged_compressed)
    {
      flags |= kAttrFlagCompressed;
    }
    offset += WriteNonResidentAttr(record, offset, AttrType::DATA, 0,
                                   stream.real_size, stream.runs,
                                   {.name = stream.name, .flags = flags});
  }

  std::vector<FakeDataRun> efsRuns;
  if (!file.efs_stream.empty())
  {
    if (file.efs_resident)
    {
      offset += WriteResidentEfsAttr(record, offset, file.efs_stream);
    }
    else
    {
      efsRuns.push_back(
          {kFakeEfsStreamLcn,
           static_cast<DWORD>((file.efs_stream.size() + kClusterSize - 1) /
                              kClusterSize)});
      offset += WriteNonResidentAttr(
          record, offset, AttrType::LOGGED_UTILITY_STREAM, 0,
          file.efs_stream.size(), efsRuns, {.name = L"$EFS"});
    }
  }
  WriteEndOfAttributesMarker(record, offset);

  std::vector<BYTE> image = BuildFakeNtfsImage();
  const DWORD mftAddr = static_cast<DWORD>(kMftLcn) * kClusterSize;
  const size_t rootOffset = mftAddr + static_cast<size_t>(kFakeFileRecordSize) *
                                          static_cast<size_t>(MftIdx::ROOT);
  std::memcpy(image.data() + rootOffset, record.data(), record.size());

  for (const FakeEncryptedStream& stream : file.streams)
  {
    LayRunBytes(image, stream.runs, stream.cluster_bytes);
  }
  LayRunBytes(image, efsRuns, file.efs_stream);

  return image;
}

std::vector<BYTE> BuildFakeNtfsImageWithEncryptedDirectory()
{
  const std::array<FakeIndexName, 1> rootNames{
      {{kEncryptedDirectoryNames[0], kEncryptedDirectoryMftRefs[0], false}}};
  const std::array<FakeIndexName, 1> blockNames{
      {{kEncryptedDirectoryNames[1], kEncryptedDirectoryMftRefs[1], true}}};

  const std::vector<FakeDataRun> runs{{kCompressedDataLcn, 1}};

  const FakeRecord record = MakeIndexAllocationDirRecord(
      NtfsBrowser::Flag::StdInfoPermission::ARCHIVE |
          NtfsBrowser::Flag::StdInfoPermission::ENCRYPTED,
      0, kFakeFileRecordSize, runs, {}, rootNames);

  return BuildCompressionImage(record, runs, MakeIndexBlockContent(blockNames));
}

std::vector<BYTE> BuildFakeNtfsImageWithCompressedEncryptedDirectory()
{
  const std::array<FakeIndexName, 1> rootNames{
      {{kEncryptedDirectoryNames[0], kEncryptedDirectoryMftRefs[0], false}}};
  const std::array<FakeIndexName, 1> blockNames{
      {{kEncryptedDirectoryNames[1], kEncryptedDirectoryMftRefs[1], true}}};

  const std::vector<FakeDataRun> runs{{kCompressedDataLcn, 2},
                                      {{}, kCompressionUnitClusters - 2}};

  const FakeRecord record = MakeIndexAllocationDirRecord(
      NtfsBrowser::Flag::StdInfoPermission::ARCHIVE |
          NtfsBrowser::Flag::StdInfoPermission::COMPRESSED |
          NtfsBrowser::Flag::StdInfoPermission::ENCRYPTED,
      kCompressionUnitSizeShift, kFakeFileRecordSize, runs, {}, rootNames);

  return BuildCompressionImage(
      record, runs,
      MakeUncompressedLznt1Chunk(MakeIndexBlockContent(blockNames)));
}

std::vector<BYTE> BuildFakeNtfsImageWithMinimalNonResidentData()
{
  std::vector<BYTE> image = BuildFakeNtfsImage();

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
  attr.comp_unit_size = 0;
  attr.real_size = 0;
  attr.alloc_size = 0;
  attr.ini_size = 0;
  // data_run_offset == total_size: the attribute is exactly its own 64-byte
  // base header, with no run-list bytes at all. Legal, and the smallest
  // total_size FileRecord<S>::ParseAttrs() may accept for a non-resident
  // attribute - the whole point of this fixture.
  attr.data_run_offset =
      static_cast<WORD>(NtfsBrowser::Attr::kHeaderNonResidentBaseSize);
  attr.header.total_size = NtfsBrowser::Attr::kHeaderNonResidentBaseSize;

  WriteEndOfAttributesMarker(record, kAttrOffset + attr.header.total_size);

  const DWORD mftAddr = static_cast<DWORD>(kMftLcn) * kClusterSize;
  const size_t rootOffset = mftAddr + static_cast<size_t>(kFakeFileRecordSize) *
                                          static_cast<size_t>(MftIdx::ROOT);
  std::memcpy(image.data() + rootOffset, record.data(), record.size());

  return image;
}

std::vector<BYTE> BuildFakeNtfsImageWithCompressedIndexAllocation()
{
  // Two real clusters (a 1026-byte uncompressed LZNT1 chunk wrapping one
  // whole index block) plus a 2-cluster sparse pad: fewer real clusters than
  // the unit holds, so the unit is compressed.
  const std::vector<FakeDataRun> runs{{kCompressedDataLcn, 2},
                                      {{}, kCompressionUnitClusters - 2}};

  const FakeRecord record = MakeIndexAllocationDirRecord(
      NtfsBrowser::Flag::StdInfoPermission::ARCHIVE |
          NtfsBrowser::Flag::StdInfoPermission::COMPRESSED,
      kCompressionUnitSizeShift, kFakeFileRecordSize, runs);

  return BuildCompressionImage(
      record, runs,
      MakeUncompressedLznt1Chunk(MakeCompressedIndexBlockContent()));
}

std::vector<BYTE> BuildFakeNtfsImageWithCorruptCompressedIndexAllocation()
{
  const std::vector<FakeDataRun> runs{{kCompressedDataLcn, 2},
                                      {{}, kCompressionUnitClusters - 2}};

  const FakeRecord record = MakeIndexAllocationDirRecord(
      NtfsBrowser::Flag::StdInfoPermission::ARCHIVE |
          NtfsBrowser::Flag::StdInfoPermission::COMPRESSED,
      kCompressionUnitSizeShift, kFakeFileRecordSize, runs);

  return BuildCompressionImage(record, runs, MakeCorruptLznt1Chunk());
}

std::vector<BYTE> BuildFakeNtfsImageWithSurrogatePairNames()
{
  const std::array<FakeIndexName, 2> rootNames{
      {{kSurrogateNames[0], kSurrogateNameMftRefs[0],
        kSurrogateNameIsDirectory[0]},
       {kSurrogateNames[1], kSurrogateNameMftRefs[1],
        kSurrogateNameIsDirectory[1]}}};
  const std::array<FakeIndexName, 2> blockNames{
      {{kSurrogateNames[2], kSurrogateNameMftRefs[2],
        kSurrogateNameIsDirectory[2]},
       {kSurrogateNames[3], kSurrogateNameMftRefs[3],
        kSurrogateNameIsDirectory[3]}}};

  const std::vector<FakeDataRun> runs{{kCompressedDataLcn, 2},
                                      {{}, kCompressionUnitClusters - 2}};

  const FakeRecord record = MakeIndexAllocationDirRecord(
      NtfsBrowser::Flag::StdInfoPermission::ARCHIVE |
          NtfsBrowser::Flag::StdInfoPermission::COMPRESSED,
      kCompressionUnitSizeShift, kFakeFileRecordSize, runs, {}, rootNames);

  return BuildCompressionImage(
      record, runs,
      MakeUncompressedLznt1Chunk(MakeIndexBlockContent(blockNames)));
}

////////////////////////////////////////////////////////////////////////////
// Fuzz-corpus-only compressed $INDEX_ALLOCATION fixtures - stay on
// $INDEX_ALLOCATION, never $DATA, since that is all FuzzOnce() ReadData()s.
////////////////////////////////////////////////////////////////////////////

std::vector<BYTE> BuildFakeNtfsImageWithCompUnitSizeOutOfRangeIndexAllocation()
{
  const std::vector<FakeDataRun> runs{{kCompressedDataLcn, 2},
                                      {{}, kCompressionUnitClusters - 2}};

  const FakeRecord record = MakeIndexAllocationDirRecord(
      NtfsBrowser::Flag::StdInfoPermission::ARCHIVE |
          NtfsBrowser::Flag::StdInfoPermission::COMPRESSED,
      kCompUnitSizeOutOfRangeShift, kFakeFileRecordSize, runs);

  return BuildCompressionImage(
      record, runs,
      MakeUncompressedLznt1Chunk(MakeCompressedIndexBlockContent()));
}

std::vector<BYTE>
    BuildFakeNtfsImageWithOversizedCompressionUnitIndexAllocation()
{
  const std::vector<FakeDataRun> runs{{kCompressedDataLcn, 2},
                                      {{}, kCompressionUnitClusters - 2}};

  const FakeRecord record = MakeIndexAllocationDirRecord(
      NtfsBrowser::Flag::StdInfoPermission::ARCHIVE |
          NtfsBrowser::Flag::StdInfoPermission::COMPRESSED,
      kOversizedCompUnitSizeShift, kFakeFileRecordSize, runs);

  return BuildCompressionImage(
      record, runs,
      MakeUncompressedLznt1Chunk(MakeCompressedIndexBlockContent()));
}

std::vector<BYTE> BuildFakeNtfsImageWithMisalignedStartVcnIndexAllocation()
{
  const std::vector<FakeDataRun> runs{{kCompressedDataLcn, 2},
                                      {{}, kCompressionUnitClusters - 2}};

  const FakeRecord record = MakeIndexAllocationDirRecord(
      NtfsBrowser::Flag::StdInfoPermission::ARCHIVE |
          NtfsBrowser::Flag::StdInfoPermission::COMPRESSED,
      kCompressionUnitSizeShift, kFakeFileRecordSize, runs,
      {.start_vcn = kMisalignedCompressedStartVcn});

  return BuildCompressionImage(
      record, runs,
      MakeUncompressedLznt1Chunk(MakeCompressedIndexBlockContent()));
}

std::vector<BYTE> BuildFakeNtfsImageWithMissingCompressedSizeIndexAllocation()
{
  const std::vector<FakeDataRun> runs{{kCompressedDataLcn, 2},
                                      {{}, kCompressionUnitClusters - 2}};

  const FakeRecord record = MakeIndexAllocationDirRecord(
      NtfsBrowser::Flag::StdInfoPermission::ARCHIVE |
          NtfsBrowser::Flag::StdInfoPermission::COMPRESSED,
      kCompressionUnitSizeShift, kFakeFileRecordSize, runs,
      {.total_size = kCompressedAttrTruncatedTotalSize});

  return BuildCompressionImage(
      record, runs,
      MakeUncompressedLznt1Chunk(MakeCompressedIndexBlockContent()));
}

std::vector<BYTE> BuildFakeNtfsImageWithUnmappedCompressionUnitIndexAllocation()
{
  // Only 2 of unit 0's 4 clusters are actually mapped by the run list, but
  // last_vcn (forced to 2 via the override) claims a 3-cluster attribute -
  // LeadingRealClusters() walks the one real run, reaches vcn 2, then runs
  // out of runs with vcn(2) != unitEnd(3).
  const std::vector<FakeDataRun> runs{{kCompressedDataLcn, 2}};

  const FakeRecord record = MakeIndexAllocationDirRecord(
      NtfsBrowser::Flag::StdInfoPermission::ARCHIVE |
          NtfsBrowser::Flag::StdInfoPermission::COMPRESSED,
      kCompressionUnitSizeShift, kFakeFileRecordSize, runs, {.last_vcn = 2});

  return BuildCompressionImage(record, runs,
                               CompressionFixturePattern(kCompressionUnitSize));
}

std::vector<BYTE> BuildFakeNtfsImageWithRealClustersAfterHoleIndexAllocation()
{
  // real, hole, real - within a single compression unit, no encoding this
  // library understands produces real clusters after a hole, so
  // LeadingRealClusters() must reject it before ParseIndexBlock() ever tries
  // to decompress anything.
  const std::vector<FakeDataRun> runs{
      {kCompressedDataLcn, 1}, {{}, 1}, {kFragmentedCompressedDataLcn, 2}};

  const FakeRecord record = MakeIndexAllocationDirRecord(
      NtfsBrowser::Flag::StdInfoPermission::ARCHIVE |
          NtfsBrowser::Flag::StdInfoPermission::COMPRESSED,
      kCompressionUnitSizeShift, kCompressionUnitSize, runs);

  // Content is irrelevant - the layout is rejected before anything is
  // decompressed - but real LZNT1-looking bytes keep the fixture honest
  // about being otherwise plausible.
  const std::vector<BYTE> clusterBytes(kXcaLznt1ExampleCompressed.begin(),
                                       kXcaLznt1ExampleCompressed.end());
  return BuildCompressionImage(record, runs, clusterBytes);
}

std::vector<BYTE> BuildFakeNtfsImageWithSparseCompressionUnitIndexAllocation()
{
  // A pure hole: LeadingRealClusters() returns 0, so GetCompressionUnit()
  // takes its "sparse" branch - the fixture's own point, independent of
  // ParseIndexBlock()'s later, separate magic-check failure on the result.
  const std::vector<FakeDataRun> runs{{{}, kCompressionUnitClusters}};

  const FakeRecord record = MakeIndexAllocationDirRecord(
      NtfsBrowser::Flag::StdInfoPermission::ARCHIVE |
          NtfsBrowser::Flag::StdInfoPermission::COMPRESSED |
          NtfsBrowser::Flag::StdInfoPermission::SPARSE,
      kCompressionUnitSizeShift, kCompressionUnitSize, runs);

  return BuildCompressionImage(record, runs, {});
}

std::vector<BYTE> BuildFakeNtfsImageWithShortDecompressedUnitIndexAllocation()
{
  const std::vector<FakeDataRun> runs{{kCompressedDataLcn, 1},
                                      {{}, kCompressionUnitClusters - 1}};

  const FakeRecord record = MakeIndexAllocationDirRecord(
      NtfsBrowser::Flag::StdInfoPermission::ARCHIVE |
          NtfsBrowser::Flag::StdInfoPermission::COMPRESSED,
      kCompressionUnitSizeShift, kCompressionUnitSize, runs);

  return BuildCompressionImage(
      record, runs,
      MakeUncompressedLznt1Chunk(
          CompressionFixturePattern(kShortDecompressedUnitSize)));
}

// LCN whose product with this fixture's cluster size overflows a signed
// LONGLONG inside ReadClusters()'s gsl::narrow<LONGLONG>() call - same
// magnitude as kHugeMftLcn, applied to a data run's LCN instead.
constexpr ULONGLONG kOverflowingLcn = 1ULL << 53;

// Directory record shaped like MakeIndexAllocationDirRecord(), but with a
// single hand-encoded run (8-byte LCN offset) at kOverflowingLcn, so it
// reaches ReadClusters()'s narrowing failure from GetCompressionUnit().
FakeRecord MakeIndexAllocationDirRecordWithOverflowingLcn(DWORD realRunClusters)
{
  FakeRecord record =
      MakeRecordHeader(kAttrOffset, NtfsBrowser::Flag::FileRecord::INUSE |
                                        NtfsBrowser::Flag::FileRecord::DIR);

  DWORD offset = kAttrOffset;
  offset += WriteStandardInformationAttr(
      record, offset,
      NtfsBrowser::Flag::StdInfoPermission::ARCHIVE |
          NtfsBrowser::Flag::StdInfoPermission::COMPRESSED);

  // $INDEX_ROOT - identical nameless SUBNODE-only entry as
  // MakeIndexAllocationDirRecord().
  auto& rootAttr =
      *reinterpret_cast<NtfsBrowser::Attr::HeaderResident*>(&record[offset]);
  rootAttr.header.type = AttrType::INDEX_ROOT;
  rootAttr.header.non_resident = 0;
  rootAttr.header.name_length = 0;
  rootAttr.header.flags = 0;
  rootAttr.header.id = 0;
  rootAttr.attr_offset = static_cast<WORD>(sizeof(rootAttr));

  BYTE* body = &record[offset + rootAttr.attr_offset];
  auto& root = *reinterpret_cast<NtfsBrowser::Attr::IndexRoot*>(body);
  root.attr_type = AttrType::FILE_NAME;
  root.coll_rule = 0;
  root.ib_size = kFakeFileRecordSize;
  root.clusters_per_ib = 1;
  root.entry_offset =
      static_cast<DWORD>((body + sizeof(NtfsBrowser::Attr::IndexRoot)) -
                         reinterpret_cast<BYTE*>(&root.entry_offset));

  auto& e1 = *reinterpret_cast<NtfsBrowser::Data::IndexEntry*>(
      body + sizeof(NtfsBrowser::Attr::IndexRoot));
  e1.mft_index = 0;
  e1.mft_sn = 0;
  e1.stream_size = 0;
  e1.flags = NtfsBrowser::Flag::IndexEntry::SUBNODE |
             NtfsBrowser::Flag::IndexEntry::LAST;
  e1.size = static_cast<WORD>(offsetof(NtfsBrowser::Data::IndexEntry, stream) +
                              sizeof(ULONGLONG));
  auto& subNodeVcn = *reinterpret_cast<ULONGLONG*>(
      reinterpret_cast<BYTE*>(&e1) + e1.size - sizeof(ULONGLONG));
  subNodeVcn = 0;

  root.total_entry_size = e1.size;
  root.alloc_entry_size = e1.size;
  root.flags = 0;

  rootAttr.attr_size =
      static_cast<DWORD>(sizeof(NtfsBrowser::Attr::IndexRoot)) + e1.size;
  rootAttr.header.total_size =
      static_cast<DWORD>(sizeof(rootAttr)) + rootAttr.attr_size;

  offset += rootAttr.header.total_size;

  // $INDEX_ALLOCATION: compressed, comp_unit_size == kCompressionUnitSizeShift.
  auto& allocAttr =
      *reinterpret_cast<NtfsBrowser::Attr::HeaderNonResident*>(&record[offset]);
  allocAttr.header.type = AttrType::INDEX_ALLOCATION;
  allocAttr.header.non_resident = 1;
  allocAttr.header.name_length = 0;
  allocAttr.header.flags = kAttrFlagCompressed;
  allocAttr.header.id = 0;
  allocAttr.start_vcn = 0;
  allocAttr.last_vcn = kCompressionUnitClusters - 1;
  allocAttr.comp_unit_size = kCompressionUnitSizeShift;
  allocAttr.alloc_size = kCompressionUnitClusters * kClusterSize;
  allocAttr.real_size = kFakeFileRecordSize;
  allocAttr.ini_size = allocAttr.real_size;

  const auto headerSize = static_cast<WORD>(
      sizeof(allocAttr) + NtfsBrowser::Attr::kCompressedSizeFieldSize);
  allocAttr.data_run_offset = headerSize;

  const ULONGLONG compressedSize =
      static_cast<ULONGLONG>(realRunClusters) * kClusterSize;
  std::memcpy(&record[offset + sizeof(allocAttr)], &compressedSize,
              sizeof(compressedSize));

  BYTE* dataRun = &record[offset + headerSize];
  DWORD runLen = 0;
  // Real run: header 0x81 (8-byte LCN-offset field), an 8-byte LE delta of
  // kOverflowingLcn, covering realRunClusters clusters.
  dataRun[runLen++] = 0x81;
  dataRun[runLen++] = static_cast<BYTE>(realRunClusters);
  {
    const auto delta = static_cast<LONGLONG>(kOverflowingLcn);
    std::memcpy(&dataRun[runLen], &delta, sizeof(delta));
    runLen += sizeof(delta);
  }
  if (realRunClusters < kCompressionUnitClusters)
  {
    // Sparse run padding the unit out to a whole compression unit (header
    // byte 0x01: 1-byte length field, 0-byte offset field).
    dataRun[runLen++] = 0x01;
    dataRun[runLen++] =
        static_cast<BYTE>(kCompressionUnitClusters - realRunClusters);
  }
  dataRun[runLen++] = 0x00;  // terminate the run list

  allocAttr.header.total_size = static_cast<DWORD>(headerSize) + runLen;

  offset += allocAttr.header.total_size;

  WriteEndOfAttributesMarker(record, offset);
  return record;
}

std::vector<BYTE> BuildFakeNtfsImageWithStoredCompressionUnitBadLcn()
{
  // All 4 clusters real (no sparse) - the "stored" branch.
  const FakeRecord record =
      MakeIndexAllocationDirRecordWithOverflowingLcn(kCompressionUnitClusters);

  std::vector<BYTE> image = BuildFakeNtfsImage();
  const DWORD mftAddr = static_cast<DWORD>(kMftLcn) * kClusterSize;
  const size_t rootOffset = mftAddr + static_cast<size_t>(kFakeFileRecordSize) *
                                          static_cast<size_t>(MftIdx::ROOT);
  std::memcpy(image.data() + rootOffset, record.data(), record.size());
  return image;
}

std::vector<BYTE> BuildFakeNtfsImageWithCompressedCompressionUnitBadLcn()
{
  // 1 real cluster (at the overflowing LCN) + 3 sparse - the "compressed"
  // branch (realClusters < unitClusters).
  const FakeRecord record = MakeIndexAllocationDirRecordWithOverflowingLcn(1);

  std::vector<BYTE> image = BuildFakeNtfsImage();
  const DWORD mftAddr = static_cast<DWORD>(kMftLcn) * kClusterSize;
  const size_t rootOffset = mftAddr + static_cast<size_t>(kFakeFileRecordSize) *
                                          static_cast<size_t>(MftIdx::ROOT);
  std::memcpy(image.data() + rootOffset, record.data(), record.size());
  return image;
}

////////////////////////////////////////////////////////////////////////////
// LZNT1 decompressor rejection-path fixtures (src/lznt1/decompress.cpp):
// each a single compression unit whose real cluster(s) hold a hand-crafted,
// malformed LZNT1 byte stream.
////////////////////////////////////////////////////////////////////////////

std::vector<BYTE> BuildFakeNtfsImageWithLznt1InvalidSignatureIndexAllocation()
{
  const std::vector<FakeDataRun> runs{{kCompressedDataLcn, 1},
                                      {{}, kCompressionUnitClusters - 1}};

  const FakeRecord record = MakeIndexAllocationDirRecord(
      NtfsBrowser::Flag::StdInfoPermission::ARCHIVE |
          NtfsBrowser::Flag::StdInfoPermission::COMPRESSED,
      kCompressionUnitSizeShift, kFakeFileRecordSize, runs);

  // Chunk header 0x1002: bit 15 clear (not the end-of-buffer 0x0000 marker),
  // bits 14-12 == 1 != the mandatory signature 3.
  const std::vector<BYTE> clusterBytes{0x02, 0x10};
  return BuildCompressionImage(record, runs, clusterBytes);
}

std::vector<BYTE>
    BuildFakeNtfsImageWithLznt1ChunkExceedsSrcBoundsIndexAllocation()
{
  const std::vector<FakeDataRun> runs{{kCompressedDataLcn, 1},
                                      {{}, kCompressionUnitClusters - 1}};

  const FakeRecord record = MakeIndexAllocationDirRecord(
      NtfsBrowser::Flag::StdInfoPermission::ARCHIVE |
          NtfsBrowser::Flag::StdInfoPermission::COMPRESSED,
      kCompressionUnitSizeShift, kFakeFileRecordSize, runs);

  // Chunk header 0xBFFF: valid, declares a 4096-byte payload - far more than
  // the ~1022 bytes actually available in the one real cluster.
  const std::vector<BYTE> clusterBytes{0xFF, 0xBF};
  return BuildCompressionImage(record, runs, clusterBytes);
}

std::vector<BYTE>
    BuildFakeNtfsImageWithLznt1UncompressedChunkExceedsDestIndexAllocation()
{
  const std::vector<FakeDataRun> runs{{kCompressedDataLcn, 1},
                                      {{}, kCompressionUnitClusters - 1}};

  const FakeRecord record = MakeIndexAllocationDirRecord(
      NtfsBrowser::Flag::StdInfoPermission::ARCHIVE |
          NtfsBrowser::Flag::StdInfoPermission::COMPRESSED,
      kCompressionUnitSizeShift, kFakeFileRecordSize, runs);

  // Chunk 1 decompresses to just short of the 4096-byte unit; chunk 2
  // (uncompressed, declared payload 10) has too little dest buffer left.
  const std::vector<BYTE> clusterBytes{0x03, 0xB0, 0x02, 0xAA,
                                       0xF4, 0x0F, 0x09, 0x30};
  return BuildCompressionImage(record, runs, clusterBytes);
}

std::vector<BYTE> BuildFakeNtfsImageWithLznt1ChunkOver4096IndexAllocation()
{
  const std::vector<FakeDataRun> runs{{kCompressedDataLcn, 1},
                                      {{}, kCompressionUnitClusters - 1}};

  const FakeRecord record = MakeIndexAllocationDirRecord(
      NtfsBrowser::Flag::StdInfoPermission::ARCHIVE |
          NtfsBrowser::Flag::StdInfoPermission::COMPRESSED,
      kCompressionUnitSizeShift, kFakeFileRecordSize, runs);

  // Output reaches exactly 4096 bytes, then one more trailing byte forces a
  // 3rd data element whose own bounds check must reject it.
  const std::vector<BYTE> clusterBytes{0x04, 0xB0, 0x02, 0xAA,
                                       0xFC, 0x0F, 0x00};
  return BuildCompressionImage(record, runs, clusterBytes);
}

std::vector<BYTE> BuildFakeNtfsImageWithLznt1LiteralExceedsDestIndexAllocation()
{
  const std::vector<FakeDataRun> runs{{kCompressedDataLcn, 1},
                                      {{}, kCompressionUnitClusters - 1}};

  const FakeRecord record = MakeIndexAllocationDirRecord(
      NtfsBrowser::Flag::StdInfoPermission::ARCHIVE |
          NtfsBrowser::Flag::StdInfoPermission::COMPRESSED,
      kCompressionUnitSizeShift, kFakeFileRecordSize, runs);

  // Chunk 1 fills the dest buffer to exactly 4096 bytes; chunk 2's first
  // element is a literal, rejected for writing past a buffer already full.
  const std::vector<BYTE> clusterBytes{0x03, 0xB0, 0x02, 0xAA, 0xFC,
                                       0x0F, 0x01, 0xB0, 0x00, 0x00};
  return BuildCompressionImage(record, runs, clusterBytes);
}

std::vector<BYTE> BuildFakeNtfsImageWithLznt1TruncatedWordIndexAllocation()
{
  const std::vector<FakeDataRun> runs{{kCompressedDataLcn, 1},
                                      {{}, kCompressionUnitClusters - 1}};

  const FakeRecord record = MakeIndexAllocationDirRecord(
      NtfsBrowser::Flag::StdInfoPermission::ARCHIVE |
          NtfsBrowser::Flag::StdInfoPermission::COMPRESSED,
      kCompressionUnitSizeShift, kFakeFileRecordSize, runs);

  // Chunk header 0xB001 (payload 2): a flag byte (0x01 - a compressed word)
  // followed by only ONE more byte, not the two a compressed word needs.
  const std::vector<BYTE> clusterBytes{0x01, 0xB0, 0x01, 0x00};
  return BuildCompressionImage(record, runs, clusterBytes);
}

std::vector<BYTE>
    BuildFakeNtfsImageWithLznt1BackreferenceExceedsDestIndexAllocation()
{
  // comp_unit_size == 1 (2048-byte unit), deliberately smaller than
  // kChunkSize, so a legal-looking back-reference can still be rejected
  // purely for exceeding this smaller unit's own dest buffer.
  const std::vector<FakeDataRun> runs{{kCompressedDataLcn, 1}, {{}, 1}};

  const FakeRecord record = MakeIndexAllocationDirRecord(
      NtfsBrowser::Flag::StdInfoPermission::ARCHIVE |
          NtfsBrowser::Flag::StdInfoPermission::COMPRESSED,
      1, kFakeFileRecordSize, runs);

  // Back-reference (length 2048) fits the per-chunk cap but exceeds this
  // unit's own 2048-byte dest buffer.
  const std::vector<BYTE> clusterBytes{0x03, 0xB0, 0x02, 0xAA, 0xFD, 0x07};
  return BuildCompressionImage(record, runs, clusterBytes);
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
