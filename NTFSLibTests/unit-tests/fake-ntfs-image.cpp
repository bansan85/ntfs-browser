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
#include "data/index-block.h"
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

// Directory whose $ATTRIBUTE_LIST has two entries naming the same
// extension record, once for $INDEX_ROOT and once for $INDEX_ALLOCATION.
FakeRecord MakeAttributeListTwoTypesDirRecord()
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
      static_cast<DWORD>(sizeof(NtfsBrowser::Attr::AttributeList)) * 2;
  attr.attr_offset = static_cast<WORD>(sizeof(attr));
  attr.header.total_size = static_cast<DWORD>(sizeof(attr)) + attr.attr_size;

  auto* alEntries = reinterpret_cast<NtfsBrowser::Attr::AttributeList*>(
      &record[kAttrOffset + attr.attr_offset]);

  alEntries[0].attr_type = AttrType::INDEX_ROOT;
  alEntries[0].record_size =
      static_cast<WORD>(sizeof(NtfsBrowser::Attr::AttributeList));
  alEntries[0].name_length = 0;
  alEntries[0].name_offset = 0;
  alEntries[0].start_vcn = 0;
  alEntries[0].base_ref.segment_number = kMultiTypeExtensionIdx;
  alEntries[0].base_ref.sequence_number = 0;
  alEntries[0].attr_id = 0;

  alEntries[1].attr_type = AttrType::INDEX_ALLOCATION;
  alEntries[1].record_size =
      static_cast<WORD>(sizeof(NtfsBrowser::Attr::AttributeList));
  alEntries[1].name_length = 0;
  alEntries[1].name_offset = 0;
  alEntries[1].start_vcn = 0;
  alEntries[1].base_ref.segment_number = kMultiTypeExtensionIdx;
  alEntries[1].base_ref.sequence_number = 0;
  alEntries[1].attr_id = 0;

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

  auto& attr = *reinterpret_cast<NtfsBrowser::Attr::HeaderResident*>(
      &record[kAttrOffset]);
  attr.header.type = AttrType::ATTRIBUTE_LIST;
  attr.header.non_resident = 0;
  attr.header.name_length = 0;
  attr.header.flags = 0;
  attr.header.id = 0;
  attr.attr_size =
      static_cast<DWORD>(sizeof(NtfsBrowser::Attr::AttributeList)) * 4;
  attr.attr_offset = static_cast<WORD>(sizeof(attr));
  attr.header.total_size = static_cast<DWORD>(sizeof(attr)) + attr.attr_size;

  const std::array<ULONGLONG, 4> extensionIdxs{
      kUafExtensionIdx0, kUafExtensionIdx1, kUafExtensionIdx2,
      kUafExtensionIdx3};

  auto* alEntries = reinterpret_cast<NtfsBrowser::Attr::AttributeList*>(
      &record[kAttrOffset + attr.attr_offset]);
  for (size_t i = 0; i < extensionIdxs.size(); i++)
  {
    alEntries[i].attr_type = AttrType::INDEX_ALLOCATION;
    alEntries[i].record_size =
        static_cast<WORD>(sizeof(NtfsBrowser::Attr::AttributeList));
    alEntries[i].name_length = 0;
    alEntries[i].name_offset = 0;
    alEntries[i].start_vcn = 0;
    alEntries[i].base_ref.segment_number = extensionIdxs[i];
    alEntries[i].base_ref.sequence_number = 0;
    alEntries[i].attr_id = 0;
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
      static_cast<DWORD>(sizeof(NtfsBrowser::Attr::AttributeList)) + 10;
  static_assert(kBodySize % sizeof(NtfsBrowser::Attr::AttributeList) != 0,
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
  e1.record_size = static_cast<WORD>(sizeof(NtfsBrowser::Attr::AttributeList));
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
  attr.attr_size = sizeof(NtfsBrowser::Attr::AttributeList);
  attr.attr_offset = static_cast<WORD>(sizeof(attr));
  attr.header.total_size = static_cast<DWORD>(sizeof(attr)) + attr.attr_size;

  auto& e1 = *reinterpret_cast<NtfsBrowser::Attr::AttributeList*>(
      &record[kAttrOffset + attr.attr_offset]);
  e1.attr_type = AttrType::ATTRIBUTE_LIST;
  e1.record_size = static_cast<WORD>(sizeof(NtfsBrowser::Attr::AttributeList));
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
