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

// Directory (#kAttrListMultiTypeDirIdx): a resident $ATTRIBUTE_LIST with TWO
// records, both naming the SAME extension record (#kMultiTypeExtensionIdx)
// but for different attribute types ($INDEX_ROOT then $INDEX_ALLOCATION) -
// regression fixture for F17's primary defect: AttrList<S>::AttrList()
// deduplicates fr.attr_list_chain_ on record_ref alone, so the second entry
// - naming the same record but a different, not-yet-retrieved attr_type -
// is wrongly skipped.
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

// Extension record (#kMultiTypeExtensionIdx): holds BOTH a resident
// $INDEX_ROOT (the same single "Foo" entry, file reference 20, as
// MakeIndexRootExtensionRecord()) and a minimal non-resident
// $INDEX_ALLOCATION right after it (empty data run, real_size 0 - the same
// "no real cluster data needed" shape MakeMftRecord() uses for $MFT's DATA
// attribute). Enough for FileRecord::ParseAttr() to construct an
// AttrIndexAlloc, which is all this fixture needs.
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

  // Second attribute: a minimal non-resident $INDEX_ALLOCATION.
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

// LCN (in kClusterSize units) where the forged index block for
// kIndexAllocDirIdx's $INDEX_ALLOCATION is written. Chosen well past every
// file record BuildFakeNtfsImage() and friends place (the highest MFT index
// used anywhere in this file is kUndersizedAttrRecordIdx = 8, i.e. LCN 9),
// so the record area and the index block data never overlap.
constexpr DWORD kForgedIndexBlockLcn = 20;

// Directory (#kIndexAllocDirIdx): $INDEX_ROOT holds a single nameless entry
// (stream_size = 0) whose only role is to carry the SUBNODE flag and a
// subnode VCN of 0, so TraverseSubEntries() follows it straight into
// TraverseSubNode() -> AttrIndexAlloc<S>::ParseIndexBlock() without needing
// any real B+-tree comparison logic. $INDEX_ALLOCATION is non-resident,
// a single data run pointing at kForgedIndexBlockLcn, where
// BuildFakeNtfsImageWithForgedIndexBlock() writes the actual "INDX" bytes.
FakeRecord MakeIndexAllocDirRecord()
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
  // Header (up to and including "stream") plus the 8-byte subnode VCN that
  // replaces it when stream_size == 0 (Data::IndexEntry, src/data/index-entry.h).
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

  // $INDEX_ALLOCATION
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
  // Data run header byte: high nibble = LCN offset field size (4 bytes),
  // low nibble = length field size (1 byte) - standard NTFS run encoding
  // (AttrNonResident::PickData, src/attr-non-resident.cpp).
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

// Extension record (#kUafExtensionIdx0..3): a single minimal non-resident
// $INDEX_ALLOCATION (empty data run - nothing in this fixture reads through
// it) whose real_size is the given sentinel. Regression fixture for N3: only
// GetDataSize() (which reads attr_header_nr_.real_size fresh on every call)
// is exercised, so the sentinel written here doubles as a marker for
// "am I still reading this specific record's own memory".
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

// Directory (#kUafAttrListDirIdx): a resident $ATTRIBUTE_LIST with FOUR
// records, each naming a DIFFERENT extension record (kUafExtensionIdx0..3)
// for the same attribute type ($INDEX_ALLOCATION) - a real, non-forged NTFS
// shape (a heavily fragmented directory's $INDEX_ALLOCATION relocated
// across several extension records, one $ATTRIBUTE_LIST entry per VCN
// range/record). Regression fixture for N3: resolving entry N+1 grows
// AttrList<S>::file_record_list_ (a std::vector<FileRecord<S>>), which in
// Strategy::FULL_CACHE can relocate every FileRecord already constructed
// for entries 0..N - and the attributes already merged out of them.
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

// Record (#kAttrNameExceedsTotalSizeRecordIdx): a single resident $DATA
// attribute whose name_offset/name_length (kAttrNameBoundsNameOffset/
// kAttrNameBoundsNameLength) point past its own declared total_size, while
// still landing on known, deterministic bytes inside this same 1024-byte
// record buffer (kAttrNameBoundsSentinel, written at that exact offset by
// this function) - regression fixture for F2: AttrBase<S>::GetAttrName()
// (src/attr-base.cpp) builds a std::wstring_view straight from
// name_offset/name_length without ever checking name_offset +
// 2*name_length against total_size (or the record's own end). A forged
// name_length/name_offset pair can in general read up to ~64KiB + 510 bytes
// past a 1024-byte record; here the sentinel sits well inside the buffer so
// the wrong (pre-fix) result is deterministic rather than depending on
// adjacent heap contents. See F2 in docs/bug-reports/2026-09-03-full-repo.md.
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
                "name must exceed total_size to reproduce F2");
  static_assert(sizeof(kAttrNameBoundsSentinel) - sizeof(wchar_t) ==
                    static_cast<size_t>(kAttrNameBoundsNameLength) *
                        sizeof(wchar_t),
                "sentinel length must match name_length exactly");

  attr.header.name_length = kAttrNameBoundsNameLength;
  attr.header.name_offset = kAttrNameBoundsNameOffset;

  // Deterministic bytes GetAttrName() must never actually be read from
  // post-fix: written past total_size (28), but well inside the 1024-byte
  // record buffer.
  std::memcpy(&record[kAttrOffset + kAttrNameBoundsNameOffset],
              kAttrNameBoundsSentinel,
              static_cast<size_t>(kAttrNameBoundsNameLength) * sizeof(wchar_t));

  WriteEndOfAttributesMarker(record, kAttrOffset + attr.header.total_size);
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
  // Never validated against sectors + 1 before the F4 fix, and not
  // consulted at all by the loop it should bound - see F4's writeup. Its
  // value here is therefore irrelevant to the bug; kept plausible only for
  // readability.
  block.size_of_us =
      static_cast<WORD>(kForgedIndexBlockSize / kBytesPerSector + 1);
  block.vcn = 0;
  block.not_leaf = 0;

  return image;
}

std::vector<BYTE> BuildFakeNtfsImageWithTinyIndexBlock()
{
  std::vector<BYTE> image = BuildFakeNtfsImage();

  // Patch the shared BPB in place, same technique as
  // BuildFakeNtfsImageWithForgedIndexBlock(): clusters_per_index_block is a
  // DWORD field, but ntfs-volume.cpp::ParseBootSector() only ever consults
  // its low byte, truncated to a signed char.
  auto& bpb = *reinterpret_cast<NtfsBrowser::Data::NtfsBpb*>(image.data());
  bpb.clusters_per_index_block = kTinyClustersPerIndexBlock;

  return image;
}

std::vector<BYTE> BuildFakeNtfsImageWithOversizedIndexBlock()
{
  std::vector<BYTE> image = BuildFakeNtfsImage();

  // Patch the shared BPB in place, same technique as
  // BuildFakeNtfsImageWithTinyIndexBlock(): clusters_per_index_block is a
  // DWORD field, but ntfs-volume.cpp::ParseBootSector() only ever consults
  // its low byte, truncated to a signed char.
  auto& bpb = *reinterpret_cast<NtfsBrowser::Data::NtfsBpb*>(image.data());
  bpb.clusters_per_index_block = kOversizedClustersPerIndexBlock;

  return image;
}

std::vector<BYTE> BuildFakeNtfsImageWithOversizedFileRecord()
{
  std::vector<BYTE> image = BuildFakeNtfsImage();

  // Patch the shared BPB in place, same technique as
  // BuildFakeNtfsImageWithOversizedIndexBlock() just above.
  auto& bpb = *reinterpret_cast<NtfsBrowser::Data::NtfsBpb*>(image.data());
  bpb.clusters_per_file_record = kOversizedClustersPerFileRecord;

  return image;
}

std::vector<BYTE> BuildFakeNtfsImageWithFileRecordSizeTooBig()
{
  std::vector<BYTE> image = BuildFakeNtfsImage();

  // Patch the shared BPB in place, same technique as
  // BuildFakeNtfsImageWithOversizedFileRecord() just above.
  auto& bpb = *reinterpret_cast<NtfsBrowser::Data::NtfsBpb*>(image.data());
  bpb.clusters_per_file_record = kFileRecordSizeTooBigClustersPerFileRecord;

  return image;
}

std::vector<BYTE> BuildFakeNtfsImageWithHugeMftLcn()
{
  std::vector<BYTE> image = BuildFakeNtfsImage();

  // Patch the shared BPB in place, same technique as
  // BuildFakeNtfsImageWithTinyIndexBlock(): lcn_mft is otherwise only ever
  // set once, in BuildFakeNtfsImage() itself.
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

  // Zero out the $MFT record BuildFakeNtfsImage() just wrote: magic no
  // longer matches kFileRecordMagic, so ParseFileRecord() fails on it, while
  // $Volume/root (written at different offsets) are untouched.
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

  // Patch the root directory's (#5) own offset_of_attr in place - unlike
  // BuildFakeNtfsImageWithOversizedFileRecord() and friends, which patch the
  // shared BPB, offset_of_attr is a per-record field (FileRecordHeader::Data),
  // so this reaches straight into the record BuildFakeNtfsImage() already
  // wrote for MftIdx::ROOT.
  const DWORD mftAddr = static_cast<DWORD>(kMftLcn) * kClusterSize;
  const size_t rootOffset = mftAddr + static_cast<size_t>(kFakeFileRecordSize) *
                                          static_cast<size_t>(MftIdx::ROOT);
  auto& header = *reinterpret_cast<NtfsBrowser::FileRecordHeader::Data*>(
      &image[rootOffset]);
  header.offset_of_attr = kAttrOffsetOutOfBounds;

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
