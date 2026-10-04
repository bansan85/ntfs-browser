#include "fake-ntfs-image.h"

#include <ntfs-browser/win-types.h>

#include <algorithm>
#include <array>
#include <cassert>
#include <cstring>
#include <fstream>
#include <limits>
#include <optional>
#include <random>
#include <span>
#include <type_traits>
#include <utility>
#include <vector>

#include <gsl/narrow>

#include <ntfs-browser/data/attr-header-common.h>
#include <ntfs-browser/data/attr-type.h>
#include <ntfs-browser/mft-idx.h>

#include "attr/attribute-list.h"
#include "attr/filename.h"
#include "attr/header-non-resident.h"
#include "attr/header-resident.h"
#include "attr/index-root.h"
#include "attr/standard-information.h"
#include "attr/volume-information.h"
#include "data/file-record-header.h"
#include "data/index-block.h"
#include "data/index-entry.h"
#include "data/ntfs-bpb.h"
#include "efs/efs-context.h"
#include "file-record-header-edit.h"
#include "flag/file-record.h"
#include "flag/filename-namespace.h"
#include "flag/filename.h"
#include "flag/index-entry.h"
#include "flag/std-info-permission.h"
#include "lznt1/decompress.h"
#include "mft-file-reference.h"

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

// Size of the fixup slot a block ends with: the USN, then one word per sector.
constexpr WORD kUsSlotSize = 6;

// Points the fixup slot at the record's own last bytes, so PatchUS()
// succeeds without a real fixup array.
constexpr WORD kOffsetOfUs = kFakeFileRecordSize - kUsSlotSize;

// Room an attribute reserves after its header for a short data run list.
constexpr size_t kRunListRoom = 8;

// Data run headers: the high nibble is the LCN offset field size, the low
// nibble the length field size. A 4-byte offset and a 1-byte length, or an
// 8-byte offset and a 1-byte length.
constexpr BYTE kRunHeader4LcnBytes = 0x41;
constexpr BYTE kRunHeader8LcnBytes = 0x81;

// Alignment the readers need to bind structs onto a record.
constexpr size_t kRecordAlignment = 8;

// $STANDARD_INFORMATION timestamps whose bytes are all distinct, so a
// misplaced field cannot match by accident.
constexpr ULONGLONG kStdInfoCreateTime = 0x0102030405060708ULL;
constexpr ULONGLONG kStdInfoAlterTime = 0x1112131415161718ULL;
constexpr ULONGLONG kStdInfoMftTime = 0x2122232425262728ULL;
constexpr ULONGLONG kStdInfoReadTime = 0x3132333435363738ULL;

// The file reference of the first fake index entry.
constexpr DWORD kFirstEntryRecord = 20;

// A byte no fixture otherwise holds, to tell a forged one from the original.
constexpr BYTE kForgedByte = 0xFF;

// A bitmap byte with every bit set, and the mask and shift that split a
// WORD into bytes.
constexpr BYTE kAllBitsSet = 0xFF;
constexpr unsigned kByteMask = 0xFFU;
constexpr unsigned kBitsPerByte = 8U;

// The index block size field's encoding of a size below one cluster:
// 0xF7 is -9, so 2^9 bytes.
constexpr BYTE kSubClusterIndexBlockEncoding = 0xF7;

// First file record of the index leaf blocks the sub-cluster fixture and the
// split-extent fixture write.
constexpr DWORD kSubClusterBlockRecordBase = 110;
constexpr DWORD kSplitBlockRecordBase = 120;

// A forged $FILE_NAME length that claims more characters than fit.
constexpr BYTE kOverlongNameLength = 200;

// The boot sector's last two bytes: 0xAA55, little-endian.
constexpr BYTE kBootSignatureLow = 0xAA;
constexpr BYTE kBootSignatureHigh = 0x55;

// The compressed bytes of the corrupt LZNT1 chunk: a valid chunk header
// (0xB002, little-endian), then a flag byte, and a compressed word whose
// displacement (1) reaches before anything was decompressed.
constexpr BYTE kCorruptChunkHeaderLow = 0x02;
constexpr BYTE kCorruptChunkHeaderHigh = 0xB0;

// How pattern bytes derive from an index: (i * mul + add) % mod. The modulus
// is prime and not a power of two, so no block size or cluster size repeats
// the pattern.
constexpr unsigned kPatternMul = 31U;
constexpr unsigned kPatternAdd = 7U;
constexpr unsigned kPatternMod = 251U;

static_assert(kAttrOffset + sizeof(NtfsBrowser::Attr::HeaderNonResident) +
                      kRunListRoom <
                  kOffsetOfUs,
              "attribute data must not reach into the fixup slot");

// The readers bind structs (8-byte aligned at most) onto offsets inside a
// record, so the record itself must start on that boundary: a plain
// std::array<BYTE> may sit at any stack address.
struct alignas(kRecordAlignment) FakeRecord
    : std::array<BYTE, kFakeFileRecordSize>
{
};

// Packs an on-disk file reference: record number low, sequence number high.
constexpr ULONGLONG MakeFileReference(ULONGLONG record, WORD sequence)
{
  return (static_cast<ULONGLONG>(sequence) << NtfsBrowser::kMftSequenceShift) |
         record;
}

// Sequence number a real volume's root directory record carries; non-zero,
// so a reader that forgets to mask it off a parent_ref sees a wrong number.
constexpr WORD kRootSequenceNumber = 5;

// NTFS starts every attribute of a record, and every entry inside one, on an
// 8-byte boundary; the readers bind structs onto these offsets, so an odd one
// is a misaligned access.
constexpr DWORD kAttrAlignment = 8;

// Returns the sub-node VCN slot: the last 8 bytes of the index entry `e`.
ULONGLONG& SubNodeVcnSlot(NtfsBrowser::Data::IndexEntry& index_entry)
{
  const std::span<BYTE> raw(reinterpret_cast<BYTE*>(&index_entry),
                            index_entry.size);
  return *reinterpret_cast<ULONGLONG*>(
      &gsl::at(raw, gsl::narrow<gsl::index>(raw.size() - sizeof(ULONGLONG))));
}

// Rounds a size up to kAttrAlignment.
constexpr DWORD AlignAttrSize(size_t size)
{
  return gsl::narrow<DWORD>((size + kAttrAlignment - 1) &
                            ~(kAttrAlignment - 1));
}

// Builds a bare file-record header with the given attribute offset and
// flags.
FakeRecord MakeRecordHeader(WORD offsetOfAttr,
                            NtfsBrowser::Flag::FileRecord flags)
{
  FakeRecord record{};

  EditFileRecordHeader(record,
                       [&](FileRecordHeader::Data& header)
                       {
                         header.magic = kFileRecordMagic;
                         header.offset_of_us = kOffsetOfUs;
                         header.size_of_us = 3;
                         header.offset_of_attr = offsetOfAttr;
                         header.flags = flags;
                       });

  return record;
}

// Makes record an extension record: its own sequence number, and the file
// reference of the base record it belongs to.
void SetRecordLink(FakeRecord& record, WORD sequence, ULONGLONG baseRef)
{
  EditFileRecordHeader(record,
                       [&](FileRecordHeader::Data& header)
                       {
                         header.seq_no = sequence;
                         header.ref_to_base = baseRef;
                       });
}

// Writes the AttrType::ALL end-of-attributes marker at offset.
void WriteEndOfAttributesMarker(FakeRecord& record, DWORD offset)
{
  const auto marker = static_cast<DWORD>(AttrType::ALL);
  std::memcpy(&record.at(offset), &marker, sizeof(marker));
}

// Encodes text as on-disk UTF-16LE code units: WORD, not wchar_t, which is
// only 16 bits on some platforms (eg. Windows) and 32 on others (eg. Linux).
// A code point above the BMP is split into a surrogate pair; an element
// already in the surrogate range (eg. one half of a pair a 16-bit wchar_t
// already split) passes through as one unit, so this is correct whether
// text arrived pre-split or not.
std::vector<WORD> ToUtf16(std::wstring_view text)
{
  constexpr char32_t kMaxBmp = 0xFFFF;
  constexpr char32_t kSurrogateBase = 0x10000;
  constexpr unsigned kSurrogateShift = 10;
  constexpr char32_t kSurrogateMask = 0x3FF;
  constexpr WORD kHighSurrogateFirst = 0xD800;
  constexpr WORD kLowSurrogateFirst = 0xDC00;

  std::vector<WORD> units;
  units.reserve(text.size());
  for (const wchar_t character : text)
  {
    const auto codePoint = static_cast<char32_t>(
        gsl::narrow<std::make_unsigned_t<wchar_t>>(character));
    if (codePoint <= kMaxBmp)
    {
      units.push_back(static_cast<WORD>(codePoint));
      continue;
    }
    const char32_t offset = codePoint - kSurrogateBase;
    units.push_back(
        gsl::narrow<WORD>(kHighSurrogateFirst + (offset >> kSurrogateShift)));
    units.push_back(
        gsl::narrow<WORD>(kLowSurrogateFirst + (offset & kSurrogateMask)));
  }
  return units;
}

// Writes record at byteOffset, growing image to the next 64 KiB boundary -
// FULL_CACHE always reads a whole 64 KiB block, so a short image short-reads.
void PutRecordAt(std::vector<BYTE>& image, size_t byteOffset,
                 const FakeRecord& record)
{
  constexpr size_t kFullCacheReadBlockSize = size_t{64} * 1024;
  const size_t neededEnd = byteOffset + record.size();
  const size_t alignedEnd =
      ((neededEnd + kFullCacheReadBlockSize - 1) / kFullCacheReadBlockSize) *
      kFullCacheReadBlockSize;
  if (image.size() < alignedEnd)
  {
    image.resize(alignedEnd, 0);
  }
  std::memcpy(&image.at(byteOffset), record.data(), record.size());
}

// Writes record at $MFT index idx - a plain contiguous slot, mftAddr plus
// idx file records - growing image first if needed.
void PutMftRecord(std::vector<BYTE>& image, DWORD mftAddr, ULONGLONG idx,
                  const FakeRecord& record)
{
  PutRecordAt(image, mftAddr + static_cast<size_t>(kFakeFileRecordSize) * idx,
              record);
}

// Builds a fake $MFT record: one non-resident DATA attribute whose
// real_size reports kSentinelRecordCount fake records, with an empty
// data run (GetRecordsCount() only reads real_size).
FakeRecord MakeMftRecord()
{
  FakeRecord record =
      MakeRecordHeader(kAttrOffset, NtfsBrowser::Flag::FileRecord::INUSE);

  auto& attr = *reinterpret_cast<NtfsBrowser::Attr::HeaderNonResident*>(
      &record.at(kAttrOffset));
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
  attr.header.total_size = AlignAttrSize(sizeof(attr) + kRunListRoom);

  record.at(kAttrOffset + sizeof(attr)) = 0x00;

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
      &record.at(kAttrOffset));
  attr.header.type = AttrType::DATA;
  attr.header.non_resident = 1;
  attr.header.name_length = 0;
  attr.header.flags = 0;
  attr.header.id = 0;
  attr.start_vcn = 0;
  attr.last_vcn = clusters - 1;
  attr.data_run_offset = static_cast<WORD>(sizeof(attr));
  attr.comp_unit_size = 0;
  attr.real_size = ULONGLONG{clusters} * kClusterSize;
  attr.alloc_size = attr.real_size;
  attr.ini_size = attr.real_size;

  const std::span<BYTE> dataRun =
      std::span<BYTE>(record).subspan(kAttrOffset + attr.data_run_offset);
  DWORD runLen = 0;
  // High nibble = LCN offset field size, low nibble = length field size.
  gsl::at(dataRun, runLen++) = kRunHeader4LcnBytes;
  gsl::at(dataRun, runLen++) = gsl::narrow<BYTE>(clusters);
  std::memcpy(&gsl::at(dataRun, runLen), &lcn, sizeof(lcn));
  runLen += sizeof(lcn);
  gsl::at(dataRun, runLen++) = 0x00;  // terminate the run list

  attr.header.total_size = AlignAttrSize(sizeof(attr) + runLen);

  WriteEndOfAttributesMarker(record, kAttrOffset + attr.header.total_size);
  return record;
}

// Builds $MFT's base record: a resident $ATTRIBUTE_LIST with one entry per
// (extension record index, start VCN) pair, then $MFT's own DATA attribute,
// whose last VCN is baseLastVcn. Every entry carries entrySequence as the
// sequence number of the extension record it names.
FakeRecord MakeMftRecordWithDataContinuations(
    std::span<const std::pair<ULONGLONG, ULONGLONG>> continuations,
    ULONGLONG baseLastVcn = 0, WORD entrySequence = 0)
{
  FakeRecord record =
      MakeRecordHeader(kAttrOffset, NtfsBrowser::Flag::FileRecord::INUSE);

  DWORD offset = kAttrOffset;

  const auto entrySize =
      AlignAttrSize(NtfsBrowser::Attr::kAttributeListEntryHeaderSize);

  auto& listAttr =
      *reinterpret_cast<NtfsBrowser::Attr::HeaderResident*>(&record.at(offset));
  listAttr.header.type = AttrType::ATTRIBUTE_LIST;
  listAttr.header.non_resident = 0;
  listAttr.header.name_length = 0;
  listAttr.header.flags = 0;
  listAttr.header.id = 0;
  listAttr.attr_size = entrySize * gsl::narrow<DWORD>(continuations.size());
  listAttr.attr_offset = static_cast<WORD>(sizeof(listAttr));
  listAttr.header.total_size =
      AlignAttrSize(sizeof(listAttr) + listAttr.attr_size);

  for (size_t i = 0; i < continuations.size(); i++)
  {
    auto& alEntry = *reinterpret_cast<NtfsBrowser::Attr::AttributeList*>(
        &record.at(offset + listAttr.attr_offset + i * entrySize));
    alEntry.attr_type = AttrType::DATA;
    alEntry.record_size = gsl::narrow<WORD>(entrySize);
    alEntry.name_length = 0;
    alEntry.name_offset = 0;
    alEntry.start_vcn =
        gsl::at(continuations, gsl::narrow<gsl::index>(i)).second;
    alEntry.base_ref.segment_number =
        gsl::at(continuations, gsl::narrow<gsl::index>(i)).first;
    alEntry.base_ref.sequence_number = entrySequence;
    alEntry.attr_id = 0;
  }

  offset += listAttr.header.total_size;

  auto& dataAttr = *reinterpret_cast<NtfsBrowser::Attr::HeaderNonResident*>(
      &record.at(offset));
  dataAttr.header.type = AttrType::DATA;
  dataAttr.header.non_resident = 1;
  dataAttr.header.name_length = 0;
  dataAttr.header.flags = 0;
  dataAttr.header.id = 0;
  dataAttr.start_vcn = 0;
  dataAttr.last_vcn = baseLastVcn;
  dataAttr.data_run_offset = static_cast<WORD>(sizeof(dataAttr));
  dataAttr.comp_unit_size = 0;
  dataAttr.real_size = kFakeFileRecordSize;
  dataAttr.alloc_size = dataAttr.real_size;
  dataAttr.ini_size = dataAttr.real_size;
  dataAttr.header.total_size = AlignAttrSize(sizeof(dataAttr) + kRunListRoom);

  record.at(offset + sizeof(dataAttr)) = 0x00;

  WriteEndOfAttributesMarker(record, offset + dataAttr.header.total_size);
  return record;
}

// Extension record holding the continuation instance of $MFT's own DATA
// attribute: a single non-resident run of clusters clusters at LCN lcn,
// starting at VCN startVcn - unlike MakeMftRecordWithRealDataRun()'s, which
// always starts at VCN 0. It belongs to $MFT (base record 0) and carries
// sequence number sequence.
FakeRecord MakeMftDataContinuationExtensionRecord(ULONGLONG startVcn, DWORD lcn,
                                                  DWORD clusters,
                                                  WORD sequence = 0)
{
  FakeRecord record =
      MakeRecordHeader(kAttrOffset, NtfsBrowser::Flag::FileRecord::INUSE);
  SetRecordLink(record, sequence, static_cast<ULONGLONG>(MftIdx::MFT));

  auto& attr = *reinterpret_cast<NtfsBrowser::Attr::HeaderNonResident*>(
      &record.at(kAttrOffset));
  attr.header.type = AttrType::DATA;
  attr.header.non_resident = 1;
  attr.header.name_length = 0;
  attr.header.flags = 0;
  attr.header.id = 0;
  attr.start_vcn = startVcn;
  attr.last_vcn = startVcn + clusters - 1;
  attr.data_run_offset = static_cast<WORD>(sizeof(attr));
  attr.comp_unit_size = 0;
  attr.real_size = (startVcn + clusters) * kClusterSize;
  attr.alloc_size = attr.real_size;
  attr.ini_size = attr.real_size;

  const std::span<BYTE> dataRun =
      std::span<BYTE>(record).subspan(kAttrOffset + attr.data_run_offset);
  DWORD runLen = 0;
  // High nibble = LCN offset field size, low nibble = length field size.
  gsl::at(dataRun, runLen++) = kRunHeader4LcnBytes;
  gsl::at(dataRun, runLen++) = gsl::narrow<BYTE>(clusters);
  std::memcpy(&gsl::at(dataRun, runLen), &lcn, sizeof(lcn));
  runLen += sizeof(lcn);
  gsl::at(dataRun, runLen++) = 0x00;  // terminate the run list

  attr.header.total_size = AlignAttrSize(sizeof(attr) + runLen);

  WriteEndOfAttributesMarker(record, kAttrOffset + attr.header.total_size);
  return record;
}

// Builds a record with valid magic but offset_of_us == kFakeFileRecordSize,
// which FileRecordHeader's ctor rejects outright.
FakeRecord MakeInvalidOffsetOfUsRecord()
{
  FakeRecord record{};

  EditFileRecordHeader(record,
                       [](FileRecordHeader::Data& header)
                       {
                         header.magic = kFileRecordMagic;
                         header.offset_of_us = kFakeFileRecordSize;
                         header.size_of_us = 3;
                       });

  return record;
}

// Builds a fake $Volume record whose VOLUME_INFORMATION attribute declares
// attrSize bytes, reporting NTFS 3.1.
FakeRecord MakeVolumeRecordSized(WORD attrSize)
{
  FakeRecord record =
      MakeRecordHeader(kAttrOffset, NtfsBrowser::Flag::FileRecord::INUSE);

  auto& attr = *reinterpret_cast<NtfsBrowser::Attr::HeaderResident*>(
      &record.at(kAttrOffset));
  attr.header.type = AttrType::VOLUME_INFORMATION;
  attr.header.non_resident = 0;
  attr.header.name_length = 0;
  attr.header.flags = 0;
  attr.header.id = 0;
  attr.attr_size = attrSize;
  attr.attr_offset = static_cast<WORD>(sizeof(attr));
  attr.header.total_size = AlignAttrSize(sizeof(attr) + attr.attr_size);

  auto& volInfo = *reinterpret_cast<NtfsBrowser::Attr::VolumeInformation*>(
      &record.at(kAttrOffset + attr.attr_offset));
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

  auto const& volInfo = *reinterpret_cast<NtfsBrowser::Attr::HeaderResident*>(
      &record.at(kAttrOffset));
  const DWORD nameOffset = kAttrOffset + volInfo.header.total_size;

  auto& attr = *reinterpret_cast<NtfsBrowser::Attr::HeaderResident*>(
      &record.at(nameOffset));
  attr.header.type = AttrType::VOLUME_NAME;
  attr.header.non_resident = 0;
  attr.header.name_length = 0;
  attr.header.flags = 0;
  attr.header.id = 1;
  const std::vector<WORD> encodedName = ToUtf16(name);
  attr.attr_size = gsl::narrow<DWORD>(encodedName.size() * sizeof(WORD));
  attr.attr_offset = static_cast<WORD>(sizeof(attr));
  attr.header.total_size = AlignAttrSize(sizeof(attr) + attr.attr_size);

  std::memcpy(&record.at(nameOffset + attr.attr_offset), encodedName.data(),
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
      &record.at(kAttrOffset));
  attr.header.type = AttrType::STANDARD_INFORMATION;
  attr.header.non_resident = 0;
  attr.header.name_length = 0;
  attr.header.flags = 0;
  attr.header.id = 0;
  attr.attr_size = attrSize;
  attr.attr_offset = static_cast<WORD>(sizeof(attr));
  attr.header.total_size = AlignAttrSize(sizeof(attr) + attr.attr_size);

  auto& stdInfo = *reinterpret_cast<NtfsBrowser::Attr::StandardInformation*>(
      &record.at(kAttrOffset + attr.attr_offset));
  stdInfo.create_time = kStdInfoCreateTime;
  stdInfo.alter_time = kStdInfoAlterTime;
  stdInfo.mft_time = kStdInfoMftTime;
  stdInfo.read_time = kStdInfoReadTime;
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
      &record.at(kAttrOffset));
  attr.header.type = AttrType::ATTRIBUTE_LIST;
  attr.header.non_resident = 0;
  attr.header.name_length = 0;
  attr.header.flags = 0;
  attr.header.id = 0;
  attr.attr_size =
      static_cast<DWORD>(NtfsBrowser::Attr::kAttributeListEntryHeaderSize);
  attr.attr_offset = static_cast<WORD>(sizeof(attr));
  attr.header.total_size = AlignAttrSize(sizeof(attr) + attr.attr_size);

  auto& alEntry = *reinterpret_cast<NtfsBrowser::Attr::AttributeList*>(
      &record.at(kAttrOffset + attr.attr_offset));
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
// (file reference 20), plus the terminating nameless entry. baseIdx is the
// record it extends; 0 leaves it a base record.
FakeRecord MakeIndexRootExtensionRecord(ULONGLONG baseIdx = 0)
{
  FakeRecord record =
      MakeRecordHeader(kAttrOffset, NtfsBrowser::Flag::FileRecord::INUSE);
  SetRecordLink(record, 0, baseIdx);

  auto& attr = *reinterpret_cast<NtfsBrowser::Attr::HeaderResident*>(
      &record.at(kAttrOffset));
  attr.header.type = AttrType::INDEX_ROOT;
  attr.header.non_resident = 0;
  attr.header.name_length = 0;
  attr.header.flags = 0;
  attr.header.id = 0;
  attr.attr_offset = static_cast<WORD>(sizeof(attr));

  const std::span<BYTE> body =
      std::span<BYTE>(record).subspan(kAttrOffset + attr.attr_offset);
  auto& root = *reinterpret_cast<NtfsBrowser::Attr::IndexRoot*>(body.data());
  root.attr_type = AttrType::FILE_NAME;
  root.coll_rule = 0;
  root.ib_size = kFakeFileRecordSize;
  root.clusters_per_ib = 1;
  root.entry_offset = gsl::narrow<DWORD>(
      (&gsl::at(body, sizeof(NtfsBrowser::Attr::IndexRoot))) -
      reinterpret_cast<BYTE*>(&root.entry_offset));

  // Entry 1: "Foo", a regular (non-directory) file, reference 20.
  auto& first_entry = *reinterpret_cast<NtfsBrowser::Data::IndexEntry*>(
      &gsl::at(body, sizeof(NtfsBrowser::Attr::IndexRoot)));
  first_entry.mft_index = kFirstEntryRecord;
  first_entry.mft_sn = 1;

  auto& filename =
      *reinterpret_cast<NtfsBrowser::Attr::Filename*>(&first_entry.stream);
  filename.parent_ref = kAttributeListDirIdx;
  filename.flags = NtfsBrowser::Flag::Filename::NONE;
  filename.name_length = 3;
  filename.name_space = NtfsBrowser::Flag::FilenameNamespace::WIN_32;
  constexpr std::wstring_view kFooName = L"Foo";
  for (BYTE i = 0; i < filename.name_length; i++)
  {
    filename.name[i] = gsl::narrow<WORD>(kFooName[i]);
  }

  first_entry.stream_size =
      gsl::narrow<WORD>(reinterpret_cast<BYTE*>(&filename.name[3]) -
                        reinterpret_cast<BYTE*>(&filename));
  first_entry.size = gsl::narrow<WORD>(AlignAttrSize(
      &first_entry.stream - reinterpret_cast<BYTE*>(&first_entry) +
      first_entry.stream_size));

  // Entry 2: the terminating entry - no name, no sub-node.
  auto& second_entry =
      *reinterpret_cast<NtfsBrowser::Data::IndexEntry*>(&gsl::at(
          body, gsl::narrow<gsl::index>(sizeof(NtfsBrowser::Attr::IndexRoot) +
                                        first_entry.size)));
  second_entry.flags = NtfsBrowser::Flag::IndexEntry::LAST;
  second_entry.stream_size = 0;
  second_entry.size = gsl::narrow<WORD>(AlignAttrSize(
      &second_entry.stream - reinterpret_cast<BYTE*>(&second_entry)));

  root.total_entry_size =
      static_cast<DWORD>(first_entry.size) + second_entry.size;
  root.alloc_entry_size = root.total_entry_size;
  root.flags = 0;

  attr.attr_size = static_cast<DWORD>(sizeof(NtfsBrowser::Attr::IndexRoot)) +
                   first_entry.size + second_entry.size;
  attr.header.total_size = AlignAttrSize(sizeof(attr) + attr.attr_size);

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

  constexpr WORD kEntrySize = static_cast<WORD>(
      AlignAttrSize(NtfsBrowser::Attr::kAttributeListEntryHeaderSize));

  auto& attr = *reinterpret_cast<NtfsBrowser::Attr::HeaderResident*>(
      &record.at(kAttrOffset));
  attr.header.type = AttrType::ATTRIBUTE_LIST;
  attr.header.non_resident = 0;
  attr.header.name_length = 0;
  attr.header.flags = 0;
  attr.header.id = 0;
  attr.attr_size = static_cast<DWORD>(kEntrySize) * 2;
  attr.attr_offset = static_cast<WORD>(sizeof(attr));
  attr.header.total_size = AlignAttrSize(sizeof(attr) + attr.attr_size);

  const std::span<BYTE> body =
      std::span<BYTE>(record).subspan(kAttrOffset + attr.attr_offset);

  auto& first_entry =
      *reinterpret_cast<NtfsBrowser::Attr::AttributeList*>(body.data());
  first_entry.attr_type = AttrType::INDEX_ROOT;
  first_entry.record_size = kEntrySize;
  first_entry.name_length = 0;
  first_entry.name_offset = 0;
  first_entry.start_vcn = 0;
  first_entry.base_ref.segment_number = kMultiTypeExtensionIdx;
  first_entry.base_ref.sequence_number = 0;
  first_entry.attr_id = 0;

  auto& second_entry = *reinterpret_cast<NtfsBrowser::Attr::AttributeList*>(
      &gsl::at(body, kEntrySize));
  second_entry.attr_type = AttrType::INDEX_ALLOCATION;
  second_entry.record_size = kEntrySize;
  second_entry.name_length = 0;
  second_entry.name_offset = 0;
  second_entry.start_vcn = 0;
  second_entry.base_ref.segment_number = kMultiTypeExtensionIdx;
  second_entry.base_ref.sequence_number = 0;
  second_entry.attr_id = 0;

  WriteEndOfAttributesMarker(record, kAttrOffset + attr.header.total_size);
  return record;
}

// Extension record holding both a resident $INDEX_ROOT (the same single
// "Foo" entry as MakeIndexRootExtensionRecord()) and a minimal
// non-resident $INDEX_ALLOCATION right after it. It extends record baseIdx.
FakeRecord MakeIndexRootAndAllocExtensionRecord(ULONGLONG baseIdx)
{
  FakeRecord record =
      MakeRecordHeader(kAttrOffset, NtfsBrowser::Flag::FileRecord::INUSE);
  SetRecordLink(record, 0, baseIdx);

  auto& rootAttr = *reinterpret_cast<NtfsBrowser::Attr::HeaderResident*>(
      &record.at(kAttrOffset));
  rootAttr.header.type = AttrType::INDEX_ROOT;
  rootAttr.header.non_resident = 0;
  rootAttr.header.name_length = 0;
  rootAttr.header.flags = 0;
  rootAttr.header.id = 0;
  rootAttr.attr_offset = static_cast<WORD>(sizeof(rootAttr));

  const std::span<BYTE> body =
      std::span<BYTE>(record).subspan(kAttrOffset + rootAttr.attr_offset);
  auto& root = *reinterpret_cast<NtfsBrowser::Attr::IndexRoot*>(body.data());
  root.attr_type = AttrType::FILE_NAME;
  root.coll_rule = 0;
  root.ib_size = kFakeFileRecordSize;
  root.clusters_per_ib = 1;
  root.entry_offset = gsl::narrow<DWORD>(
      (&gsl::at(body, sizeof(NtfsBrowser::Attr::IndexRoot))) -
      reinterpret_cast<BYTE*>(&root.entry_offset));

  // Entry 1: "Foo", a regular (non-directory) file, reference 20.
  auto& first_entry = *reinterpret_cast<NtfsBrowser::Data::IndexEntry*>(
      &gsl::at(body, sizeof(NtfsBrowser::Attr::IndexRoot)));
  first_entry.mft_index = kFirstEntryRecord;
  first_entry.mft_sn = 1;

  auto& filename =
      *reinterpret_cast<NtfsBrowser::Attr::Filename*>(&first_entry.stream);
  filename.parent_ref = kAttrListMultiTypeDirIdx;
  filename.flags = NtfsBrowser::Flag::Filename::NONE;
  filename.name_length = 3;
  filename.name_space = NtfsBrowser::Flag::FilenameNamespace::WIN_32;
  constexpr std::wstring_view kFooName = L"Foo";
  for (BYTE i = 0; i < filename.name_length; i++)
  {
    filename.name[i] = gsl::narrow<WORD>(kFooName[i]);
  }

  first_entry.stream_size =
      gsl::narrow<WORD>(reinterpret_cast<BYTE*>(&filename.name[3]) -
                        reinterpret_cast<BYTE*>(&filename));
  first_entry.size = gsl::narrow<WORD>(AlignAttrSize(
      &first_entry.stream - reinterpret_cast<BYTE*>(&first_entry) +
      first_entry.stream_size));

  // Entry 2: the terminating entry - no name, no sub-node.
  auto& second_entry =
      *reinterpret_cast<NtfsBrowser::Data::IndexEntry*>(&gsl::at(
          body, gsl::narrow<gsl::index>(sizeof(NtfsBrowser::Attr::IndexRoot) +
                                        first_entry.size)));
  second_entry.flags = NtfsBrowser::Flag::IndexEntry::LAST;
  second_entry.stream_size = 0;
  second_entry.size = gsl::narrow<WORD>(AlignAttrSize(
      &second_entry.stream - reinterpret_cast<BYTE*>(&second_entry)));

  root.total_entry_size =
      static_cast<DWORD>(first_entry.size) + second_entry.size;
  root.alloc_entry_size = root.total_entry_size;
  root.flags = 0;

  rootAttr.attr_size =
      static_cast<DWORD>(sizeof(NtfsBrowser::Attr::IndexRoot)) +
      first_entry.size + second_entry.size;
  rootAttr.header.total_size =
      AlignAttrSize(sizeof(rootAttr) + rootAttr.attr_size);

  const DWORD allocAttrOffset = kAttrOffset + rootAttr.header.total_size;
  auto& allocAttr = *reinterpret_cast<NtfsBrowser::Attr::HeaderNonResident*>(
      &record.at(allocAttrOffset));
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
  allocAttr.header.total_size = AlignAttrSize(sizeof(allocAttr) + kRunListRoom);

  // Data run: a single 0x00 byte terminates the run list immediately -
  // nothing reads through it in this fixture.
  record.at(allocAttrOffset + sizeof(allocAttr)) = 0x00;

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
      &record.at(kAttrOffset));
  attr.header.type = AttrType::REPARSE_POINT;
  attr.header.non_resident = 0;
  attr.header.name_length = 0;
  attr.header.flags = 0;
  attr.header.id = 0;
  attr.header.total_size = kUndersizedTotalSize;

  record.at(kAttrOffset + sizeof(NtfsBrowser::Attr::HeaderResident)) =
      kForgedByte;

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
      *reinterpret_cast<NtfsBrowser::Attr::HeaderResident*>(&record.at(offset));
  rootAttr.header.type = AttrType::INDEX_ROOT;
  rootAttr.header.non_resident = 0;
  rootAttr.header.name_length = 0;
  rootAttr.header.flags = 0;
  rootAttr.header.id = 0;
  rootAttr.attr_offset = static_cast<WORD>(sizeof(rootAttr));

  const std::span<BYTE> body =
      std::span<BYTE>(record).subspan(offset + rootAttr.attr_offset);
  auto& root = *reinterpret_cast<NtfsBrowser::Attr::IndexRoot*>(body.data());
  root.attr_type = AttrType::FILE_NAME;
  root.coll_rule = 0;
  root.ib_size = kForgedIndexBlockSize;
  root.clusters_per_ib =
      static_cast<BYTE>(kForgedIndexBlockSize / kClusterSize);
  root.entry_offset = gsl::narrow<DWORD>(
      (&gsl::at(body, sizeof(NtfsBrowser::Attr::IndexRoot))) -
      reinterpret_cast<BYTE*>(&root.entry_offset));

  auto& first_entry = *reinterpret_cast<NtfsBrowser::Data::IndexEntry*>(
      &gsl::at(body, sizeof(NtfsBrowser::Attr::IndexRoot)));
  first_entry.mft_index = 0;
  first_entry.mft_sn = 0;
  first_entry.stream_size = 0;
  first_entry.flags = NtfsBrowser::Flag::IndexEntry::SUBNODE |
                      NtfsBrowser::Flag::IndexEntry::LAST;
  // Size covers the header up to stream, plus the 8-byte subnode VCN.
  first_entry.size = static_cast<WORD>(AlignAttrSize(
      offsetof(NtfsBrowser::Data::IndexEntry, stream) + sizeof(ULONGLONG)));
  auto& subNodeVcn = SubNodeVcnSlot(first_entry);
  subNodeVcn = 0;

  root.total_entry_size = first_entry.size;
  root.alloc_entry_size = first_entry.size;
  root.flags = 0;

  rootAttr.attr_size =
      static_cast<DWORD>(sizeof(NtfsBrowser::Attr::IndexRoot)) +
      first_entry.size;
  rootAttr.header.total_size =
      AlignAttrSize(sizeof(rootAttr) + rootAttr.attr_size);

  offset += rootAttr.header.total_size;

  auto& allocAttr = *reinterpret_cast<NtfsBrowser::Attr::HeaderNonResident*>(
      &record.at(offset));
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

  const std::span<BYTE> dataRun =
      std::span<BYTE>(record).subspan(offset + allocAttr.data_run_offset);
  DWORD runLen = 0;
  // High nibble = 4-byte LCN offset field; low nibble = 1-byte run length.
  gsl::at(dataRun, runLen++) = kRunHeader4LcnBytes;
  gsl::at(dataRun, runLen++) =
      static_cast<BYTE>(kForgedIndexBlockSize / kClusterSize);
  {
    const DWORD lcn = kForgedIndexBlockLcn;
    std::memcpy(&gsl::at(dataRun, runLen), &lcn, sizeof(lcn));
    runLen += sizeof(lcn);
  }
  gsl::at(dataRun, runLen++) = 0x00;  // terminate the run list

  allocAttr.header.total_size = AlignAttrSize(sizeof(allocAttr) + runLen);

  offset += allocAttr.header.total_size;

  WriteEndOfAttributesMarker(record, offset);
  return record;
}

// Extension record: a minimal non-resident $INDEX_ALLOCATION whose
// real_size is the given sentinel. It extends record baseIdx.
FakeRecord MakeIndexAllocationOnlyExtensionRecord(DWORD realSize,
                                                  ULONGLONG baseIdx)
{
  FakeRecord record =
      MakeRecordHeader(kAttrOffset, NtfsBrowser::Flag::FileRecord::INUSE);
  SetRecordLink(record, 0, baseIdx);

  auto& allocAttr = *reinterpret_cast<NtfsBrowser::Attr::HeaderNonResident*>(
      &record.at(kAttrOffset));
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
  allocAttr.header.total_size = AlignAttrSize(sizeof(allocAttr) + kRunListRoom);

  // Data run: a single 0x00 byte terminates the run list immediately.
  record.at(kAttrOffset + sizeof(allocAttr)) = 0x00;

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

  constexpr WORD kEntrySize = static_cast<WORD>(
      AlignAttrSize(NtfsBrowser::Attr::kAttributeListEntryHeaderSize));

  auto& attr = *reinterpret_cast<NtfsBrowser::Attr::HeaderResident*>(
      &record.at(kAttrOffset));
  attr.header.type = AttrType::ATTRIBUTE_LIST;
  attr.header.non_resident = 0;
  attr.header.name_length = 0;
  attr.header.flags = 0;
  attr.header.id = 0;
  attr.attr_size = static_cast<DWORD>(kEntrySize) * 4;
  attr.attr_offset = static_cast<WORD>(sizeof(attr));
  attr.header.total_size = AlignAttrSize(sizeof(attr) + attr.attr_size);

  const std::array<ULONGLONG, 4> extensionIdxs{
      kUafExtensionIdx0, kUafExtensionIdx1, kUafExtensionIdx2,
      kUafExtensionIdx3};

  const std::span<BYTE> body =
      std::span<BYTE>(record).subspan(kAttrOffset + attr.attr_offset);
  for (size_t i = 0; i < extensionIdxs.size(); i++)
  {
    auto& entry = *reinterpret_cast<NtfsBrowser::Attr::AttributeList*>(
        &gsl::at(body, gsl::narrow<gsl::index>(i * kEntrySize)));
    entry.attr_type = AttrType::INDEX_ALLOCATION;
    entry.record_size = kEntrySize;
    entry.name_length = 0;
    entry.name_offset = 0;
    entry.start_vcn = 0;
    entry.base_ref.segment_number = extensionIdxs.at(i);
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
      &record.at(kAttrOffset));
  attr.header.type = AttrType::DATA;
  attr.header.non_resident = 0;
  attr.header.flags = 0;
  attr.header.id = 0;
  attr.attr_size = kBodySize;
  attr.attr_offset = static_cast<WORD>(sizeof(attr));
  attr.header.total_size = AlignAttrSize(sizeof(attr) + kBodySize);

  static_assert(static_cast<DWORD>(kAttrNameBoundsNameOffset) +
                        2 * static_cast<DWORD>(kAttrNameBoundsNameLength) >
                    sizeof(NtfsBrowser::Attr::HeaderResident) + kBodySize,
                "name must exceed total_size");
  static_assert(kAttrNameBoundsSentinel.size() ==
                    static_cast<size_t>(kAttrNameBoundsNameLength),
                "sentinel length must match name_length exactly");

  attr.header.name_length = kAttrNameBoundsNameLength;
  attr.header.name_offset = kAttrNameBoundsNameOffset;

  // Past total_size (28), but still inside the 1024-byte record buffer.
  const std::vector<WORD> encodedSentinel = ToUtf16(kAttrNameBoundsSentinel);
  std::memcpy(&record.at(kAttrOffset + kAttrNameBoundsNameOffset),
              encodedSentinel.data(),
              static_cast<size_t>(kAttrNameBoundsNameLength) * sizeof(WORD));

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
      &record.at(kAttrOffset));
  attr.header.type = AttrType::DATA;
  attr.header.non_resident = 0;
  attr.header.name_length = 0;
  attr.header.flags = 0;
  attr.header.id = 0;
  attr.attr_size = gsl::narrow<DWORD>(kSmallResidentDataContent.size());
  attr.attr_offset = static_cast<WORD>(sizeof(attr));
  attr.header.total_size = AlignAttrSize(sizeof(attr) + attr.attr_size);

  std::memcpy(&record.at(kAttrOffset + attr.attr_offset),
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
      &record.at(kAttrOffset));
  attr.header.type = AttrType::ATTRIBUTE_LIST;
  attr.header.non_resident = 0;
  attr.header.name_length = 0;
  attr.header.flags = 0;
  attr.header.id = 0;
  attr.attr_size = kBodySize;
  attr.attr_offset = static_cast<WORD>(sizeof(attr));
  attr.header.total_size = AlignAttrSize(sizeof(attr) + attr.attr_size);

  auto& first_entry = *reinterpret_cast<NtfsBrowser::Attr::AttributeList*>(
      &record.at(kAttrOffset + attr.attr_offset));
  first_entry.attr_type = AttrType::DATA;
  first_entry.record_size =
      static_cast<WORD>(NtfsBrowser::Attr::kAttributeListEntryHeaderSize);
  first_entry.name_length = 0;
  first_entry.name_offset = 0;
  first_entry.start_vcn = 0;
  first_entry.base_ref.segment_number = static_cast<ULONGLONG>(MftIdx::ROOT);
  first_entry.base_ref.sequence_number = 0;
  first_entry.attr_id = 0;

  WriteEndOfAttributesMarker(record, kAttrOffset + attr.header.total_size);
  return record;
}

// Builds a directory record whose resident $ATTRIBUTE_LIST holds a real
// entry (relocating $INDEX_ROOT to kIndexExtensionIdx), followed by one
// whose own record_size is nonzero but smaller than the entry header
// itself. VG4(a): the mid-loop record_size bounds check, never a short
// ReadData().
FakeRecord MakeAttributeListRecordSizeTooSmallDirRecord()
{
  FakeRecord record =
      MakeRecordHeader(kAttrOffset, NtfsBrowser::Flag::FileRecord::INUSE |
                                        NtfsBrowser::Flag::FileRecord::DIR);

  constexpr WORD kEntrySize = static_cast<WORD>(
      AlignAttrSize(NtfsBrowser::Attr::kAttributeListEntryHeaderSize));
  // Nonzero, but smaller than kEntrySize - the exact condition under test.
  constexpr WORD kTooSmallRecordSize = 5;

  auto& attr = *reinterpret_cast<NtfsBrowser::Attr::HeaderResident*>(
      &record.at(kAttrOffset));
  attr.header.type = AttrType::ATTRIBUTE_LIST;
  attr.header.non_resident = 0;
  attr.header.name_length = 0;
  attr.header.flags = 0;
  attr.header.id = 0;
  attr.attr_size = static_cast<DWORD>(kEntrySize) * 2;
  attr.attr_offset = static_cast<WORD>(sizeof(attr));
  attr.header.total_size = AlignAttrSize(sizeof(attr) + attr.attr_size);

  const std::span<BYTE> body =
      std::span<BYTE>(record).subspan(kAttrOffset + attr.attr_offset);

  auto& first_entry =
      *reinterpret_cast<NtfsBrowser::Attr::AttributeList*>(body.data());
  first_entry.attr_type = AttrType::INDEX_ROOT;
  first_entry.record_size = kEntrySize;
  first_entry.name_length = 0;
  first_entry.name_offset = 0;
  first_entry.start_vcn = 0;
  first_entry.base_ref.segment_number = kIndexExtensionIdx;
  first_entry.base_ref.sequence_number = 0;
  first_entry.attr_id = 0;

  // Relocates nowhere - base_ref names this same directory record, so
  // AttrList skips it - only its record_size matters here.
  auto& second_entry = *reinterpret_cast<NtfsBrowser::Attr::AttributeList*>(
      &gsl::at(body, kEntrySize));
  second_entry.attr_type = AttrType::DATA;
  second_entry.record_size = kTooSmallRecordSize;
  second_entry.name_length = 0;
  second_entry.name_offset = 0;
  second_entry.start_vcn = 0;
  second_entry.base_ref.segment_number = kAttributeListDirIdx;
  second_entry.base_ref.sequence_number = 0;
  second_entry.attr_id = 0;

  WriteEndOfAttributesMarker(record, kAttrOffset + attr.header.total_size);
  return record;
}

// Builds a directory record whose resident $ATTRIBUTE_LIST holds a single
// real entry (relocating $INDEX_ROOT to kIndexExtensionIdx) whose own
// record_size overshoots the attribute's declared size. VG4(b): the
// post-loop offset-vs-size check, reached through AttrList's normal
// (nullopt) end, not a short ReadData().
FakeRecord MakeAttributeListOffsetMismatchDirRecord()
{
  FakeRecord record =
      MakeRecordHeader(kAttrOffset, NtfsBrowser::Flag::FileRecord::INUSE |
                                        NtfsBrowser::Flag::FileRecord::DIR);

  constexpr WORD kEntrySize = static_cast<WORD>(
      AlignAttrSize(NtfsBrowser::Attr::kAttributeListEntryHeaderSize));
  // Past kEntrySize, so the single entry's declared span overshoots the
  // attribute's own declared size below.
  constexpr WORD kOvershootRecordSize = static_cast<WORD>(kEntrySize + 4);

  auto& attr = *reinterpret_cast<NtfsBrowser::Attr::HeaderResident*>(
      &record.at(kAttrOffset));
  attr.header.type = AttrType::ATTRIBUTE_LIST;
  attr.header.non_resident = 0;
  attr.header.name_length = 0;
  attr.header.flags = 0;
  attr.header.id = 0;
  attr.attr_size = static_cast<DWORD>(kEntrySize);
  attr.attr_offset = static_cast<WORD>(sizeof(attr));
  attr.header.total_size = AlignAttrSize(sizeof(attr) + attr.attr_size);

  auto& first_entry = *reinterpret_cast<NtfsBrowser::Attr::AttributeList*>(
      &record.at(kAttrOffset + attr.attr_offset));
  first_entry.attr_type = AttrType::INDEX_ROOT;
  first_entry.record_size = kOvershootRecordSize;
  first_entry.name_length = 0;
  first_entry.name_offset = 0;
  first_entry.start_vcn = 0;
  first_entry.base_ref.segment_number = kIndexExtensionIdx;
  first_entry.base_ref.sequence_number = 0;
  first_entry.attr_id = 0;

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
      &record.at(kAttrOffset));
  attr.header.type = AttrType::ATTRIBUTE_LIST;
  attr.header.non_resident = 0;
  attr.header.name_length = 0;
  attr.header.flags = 0;
  attr.header.id = 0;
  attr.attr_size =
      static_cast<DWORD>(NtfsBrowser::Attr::kAttributeListEntryHeaderSize);
  attr.attr_offset = static_cast<WORD>(sizeof(attr));
  attr.header.total_size = AlignAttrSize(sizeof(attr) + attr.attr_size);

  auto& first_entry = *reinterpret_cast<NtfsBrowser::Attr::AttributeList*>(
      &record.at(kAttrOffset + attr.attr_offset));
  first_entry.attr_type = AttrType::ATTRIBUTE_LIST;
  first_entry.record_size =
      static_cast<WORD>(NtfsBrowser::Attr::kAttributeListEntryHeaderSize);
  first_entry.name_length = 0;
  first_entry.name_offset = 0;
  first_entry.start_vcn = 0;
  first_entry.base_ref.segment_number = targetIdx;
  first_entry.base_ref.sequence_number = 0;
  first_entry.attr_id = 0;

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
      &record.at(kAttrOffset));
  attr.header.type = AttrType::ATTRIBUTE_LIST;
  attr.header.non_resident = 0;
  attr.header.name_length = 0;
  attr.header.flags = 0;
  attr.header.id = 0;
  attr.attr_size = static_cast<DWORD>(kAttributeListRealEntrySize) * 2;
  attr.attr_offset = static_cast<WORD>(sizeof(attr));
  attr.header.total_size = AlignAttrSize(sizeof(attr) + attr.attr_size);

  const std::span<BYTE> body =
      std::span<BYTE>(record).subspan(kAttrOffset + attr.attr_offset);

  // The second entry sits at an odd stride on purpose, so it cannot be bound
  // to a struct reference: fill an aligned copy and copy the on-disk bytes.
  const auto writeEntry =
      [](BYTE* dest, AttrType type, ULONGLONG recordIdx, WORD attrId)
  {
    NtfsBrowser::Attr::AttributeList entry{};
    entry.attr_type = type;
    entry.record_size = kAttributeListRealEntrySize;
    entry.name_length = 0;
    entry.name_offset = 0;
    entry.start_vcn = 0;
    entry.base_ref.segment_number = recordIdx;
    entry.base_ref.sequence_number = 0;
    entry.attr_id = attrId;
    std::memcpy(dest, &entry, kAttributeListRealEntrySize);
  };
  writeEntry(body.data(), AttrType::INDEX_ROOT, kAttrListTightPackExtIdxA, 0);
  writeEntry(&gsl::at(body, kAttributeListRealEntrySize),
             AttrType::INDEX_ALLOCATION, kAttrListTightPackExtIdxB, 1);

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
      &record.at(kAttrOffset));
  attr.header.type = AttrType::DATA;
  attr.header.non_resident = 0;
  attr.header.flags = 0;
  attr.header.id = 0;
  const std::vector<WORD> encodedName = ToUtf16(kNamedDataStreamName);
  attr.header.name_length = kNamedDataStreamNameLength;
  attr.header.name_offset = static_cast<WORD>(sizeof(attr));
  attr.attr_size = gsl::narrow<DWORD>(kNamedDataStreamContent.size());
  attr.attr_offset =
      gsl::narrow<WORD>(sizeof(attr) + encodedName.size() * sizeof(WORD));
  attr.header.total_size = AlignAttrSize(attr.attr_offset + attr.attr_size);

  std::memcpy(&record.at(kAttrOffset + attr.header.name_offset),
              encodedName.data(), encodedName.size() * sizeof(WORD));
  std::memcpy(&record.at(kAttrOffset + attr.attr_offset),
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
      &record.at(kAttrOffset));
  attr.header.type = AttrType::INDEX_ROOT;
  attr.header.non_resident = 0;
  attr.header.name_length = 0;
  attr.header.flags = 0;
  attr.header.id = 0;
  attr.attr_offset = static_cast<WORD>(sizeof(attr));

  const std::span<BYTE> body =
      std::span<BYTE>(record).subspan(kAttrOffset + attr.attr_offset);
  auto& root = *reinterpret_cast<NtfsBrowser::Attr::IndexRoot*>(body.data());
  root.attr_type = AttrType::FILE_NAME;
  root.coll_rule = 0;
  root.ib_size = kFakeFileRecordSize;
  root.clusters_per_ib = 1;
  root.entry_offset = gsl::narrow<DWORD>(
      (&gsl::at(body, sizeof(NtfsBrowser::Attr::IndexRoot))) -
      reinterpret_cast<BYTE*>(&root.entry_offset));

  // Entry 1: the single real FILE_NAME entry this variant declares.
  auto& first_entry = *reinterpret_cast<NtfsBrowser::Data::IndexEntry*>(
      &gsl::at(body, sizeof(NtfsBrowser::Attr::IndexRoot)));
  first_entry.mft_index = mftIndex;
  first_entry.mft_sn = 1;

  auto& filename =
      *reinterpret_cast<NtfsBrowser::Attr::Filename*>(&first_entry.stream);
  filename.parent_ref = parentRef;
  filename.flags = NtfsBrowser::Flag::Filename::NONE;
  filename.name_length = gsl::narrow<BYTE>(name.size());
  filename.name_space = NtfsBrowser::Flag::FilenameNamespace::WIN_32;
  for (BYTE i = 0; i < filename.name_length; i++)
  {
    // name_length is name.size(), so i < name.size() by the loop condition.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    filename.name[i] = gsl::narrow<WORD>(name[i]);
  }

  first_entry.stream_size = gsl::narrow<WORD>(
      reinterpret_cast<BYTE*>(&filename.name[filename.name_length]) -
      reinterpret_cast<BYTE*>(&filename));
  first_entry.size = gsl::narrow<WORD>(AlignAttrSize(
      &first_entry.stream - reinterpret_cast<BYTE*>(&first_entry) +
      first_entry.stream_size));

  // Entry 2: the terminating entry - no name, no sub-node.
  auto& second_entry =
      *reinterpret_cast<NtfsBrowser::Data::IndexEntry*>(&gsl::at(
          body, gsl::narrow<gsl::index>(sizeof(NtfsBrowser::Attr::IndexRoot) +
                                        first_entry.size)));
  second_entry.flags = NtfsBrowser::Flag::IndexEntry::LAST;
  second_entry.stream_size = 0;
  second_entry.size = gsl::narrow<WORD>(AlignAttrSize(
      &second_entry.stream - reinterpret_cast<BYTE*>(&second_entry)));

  root.total_entry_size =
      static_cast<DWORD>(first_entry.size) + second_entry.size;
  root.alloc_entry_size = root.total_entry_size;
  root.flags = 0;

  attr.attr_size = static_cast<DWORD>(sizeof(NtfsBrowser::Attr::IndexRoot)) +
                   first_entry.size + second_entry.size;
  attr.header.total_size = AlignAttrSize(sizeof(attr) + attr.attr_size);

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
      *reinterpret_cast<NtfsBrowser::Attr::HeaderResident*>(&record.at(offset));
  rootAttr.header.type = AttrType::INDEX_ROOT;
  rootAttr.header.non_resident = 0;
  rootAttr.header.name_length = 0;
  rootAttr.header.flags = 0;
  rootAttr.header.id = 0;
  rootAttr.attr_offset = static_cast<WORD>(sizeof(rootAttr));

  const std::span<BYTE> body =
      std::span<BYTE>(record).subspan(offset + rootAttr.attr_offset);
  auto& root = *reinterpret_cast<NtfsBrowser::Attr::IndexRoot*>(body.data());
  root.attr_type = AttrType::FILE_NAME;
  root.coll_rule = 0;
  root.ib_size = kFakeFileRecordSize;
  root.clusters_per_ib = 1;
  root.entry_offset = gsl::narrow<DWORD>(
      (&gsl::at(body, sizeof(NtfsBrowser::Attr::IndexRoot))) -
      reinterpret_cast<BYTE*>(&root.entry_offset));

  // Entry 1 ("A_"): non-terminal, so it carries both a name and a sub-node
  // VCN right after it.
  auto& first_entry = *reinterpret_cast<NtfsBrowser::Data::IndexEntry*>(
      &gsl::at(body, sizeof(NtfsBrowser::Attr::IndexRoot)));
  first_entry.mft_index = kGapCollationNonTerminalMftRef;
  first_entry.mft_sn = 1;

  auto& fn1 =
      *reinterpret_cast<NtfsBrowser::Attr::Filename*>(&first_entry.stream);
  fn1.parent_ref = static_cast<ULONGLONG>(MftIdx::ROOT);
  fn1.flags = NtfsBrowser::Flag::Filename::DIRECTORY;
  constexpr std::wstring_view kNonTerminalName = L"A_";
  fn1.name_length = 2;
  fn1.name_space = NtfsBrowser::Flag::FilenameNamespace::WIN_32;
  for (BYTE i = 0; i < fn1.name_length; i++)
  {
    fn1.name[i] = gsl::narrow<WORD>(kNonTerminalName[i]);
  }

  first_entry.stream_size =
      gsl::narrow<WORD>(reinterpret_cast<BYTE*>(&fn1.name[fn1.name_length]) -
                        reinterpret_cast<BYTE*>(&fn1));
  first_entry.flags = NtfsBrowser::Flag::IndexEntry::SUBNODE;
  first_entry.size = gsl::narrow<WORD>(AlignAttrSize(
      &first_entry.stream - reinterpret_cast<BYTE*>(&first_entry) +
      first_entry.stream_size + sizeof(ULONGLONG)));
  auto& subNodeVcn = SubNodeVcnSlot(first_entry);
  subNodeVcn = 0;

  // Entry 2: the terminating entry - no name, no sub-node.
  auto& second_entry =
      *reinterpret_cast<NtfsBrowser::Data::IndexEntry*>(&gsl::at(
          body, gsl::narrow<gsl::index>(sizeof(NtfsBrowser::Attr::IndexRoot) +
                                        first_entry.size)));
  second_entry.flags = NtfsBrowser::Flag::IndexEntry::LAST;
  second_entry.stream_size = 0;
  second_entry.size = gsl::narrow<WORD>(AlignAttrSize(
      &second_entry.stream - reinterpret_cast<BYTE*>(&second_entry)));

  root.total_entry_size =
      static_cast<DWORD>(first_entry.size) + second_entry.size;
  root.alloc_entry_size = root.total_entry_size;
  root.flags = 0;

  rootAttr.attr_size =
      static_cast<DWORD>(sizeof(NtfsBrowser::Attr::IndexRoot)) +
      first_entry.size + second_entry.size;
  rootAttr.header.total_size =
      AlignAttrSize(sizeof(rootAttr) + rootAttr.attr_size);

  offset += rootAttr.header.total_size;

  // $INDEX_ALLOCATION
  auto& allocAttr = *reinterpret_cast<NtfsBrowser::Attr::HeaderNonResident*>(
      &record.at(offset));
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

  const std::span<BYTE> dataRun =
      std::span<BYTE>(record).subspan(offset + allocAttr.data_run_offset);
  DWORD runLen = 0;
  // High nibble = LCN offset field size, low nibble = length field size.
  gsl::at(dataRun, runLen++) = kRunHeader4LcnBytes;
  gsl::at(dataRun, runLen++) = 1;  // 1 cluster
  {
    const DWORD lcn = kGapCollationIndexBlockLcn;
    std::memcpy(&gsl::at(dataRun, runLen), &lcn, sizeof(lcn));
    runLen += sizeof(lcn);
  }
  gsl::at(dataRun, runLen++) = 0x00;  // terminate the run list

  allocAttr.header.total_size = AlignAttrSize(sizeof(allocAttr) + runLen);

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
      *reinterpret_cast<NtfsBrowser::Attr::HeaderResident*>(&record.at(offset));
  rootAttr.header.type = AttrType::INDEX_ROOT;
  rootAttr.header.non_resident = 0;
  rootAttr.header.name_length = 0;
  rootAttr.header.flags = 0;
  rootAttr.header.id = 0;
  rootAttr.attr_offset = static_cast<WORD>(sizeof(rootAttr));

  const std::span<BYTE> body =
      std::span<BYTE>(record).subspan(offset + rootAttr.attr_offset);
  auto& root = *reinterpret_cast<NtfsBrowser::Attr::IndexRoot*>(body.data());
  root.attr_type = AttrType::FILE_NAME;
  root.coll_rule = 0;
  root.ib_size = kClusterSize;
  root.clusters_per_ib = 1;
  root.entry_offset = gsl::narrow<DWORD>(
      (&gsl::at(body, sizeof(NtfsBrowser::Attr::IndexRoot))) -
      reinterpret_cast<BYTE*>(&root.entry_offset));

  auto& first_entry = *reinterpret_cast<NtfsBrowser::Data::IndexEntry*>(
      &gsl::at(body, sizeof(NtfsBrowser::Attr::IndexRoot)));
  first_entry.mft_index = 0;
  first_entry.mft_sn = 0;
  first_entry.stream_size = 0;
  first_entry.flags = NtfsBrowser::Flag::IndexEntry::SUBNODE |
                      NtfsBrowser::Flag::IndexEntry::LAST;
  // Header plus the 8-byte subnode VCN that replaces "stream" when empty.
  first_entry.size = static_cast<WORD>(AlignAttrSize(
      offsetof(NtfsBrowser::Data::IndexEntry, stream) + sizeof(ULONGLONG)));
  auto& subNodeVcn = SubNodeVcnSlot(first_entry);
  subNodeVcn = 0;

  root.total_entry_size = first_entry.size;
  root.alloc_entry_size = first_entry.size;
  root.flags = 0;

  rootAttr.attr_size =
      static_cast<DWORD>(sizeof(NtfsBrowser::Attr::IndexRoot)) +
      first_entry.size;
  rootAttr.header.total_size =
      AlignAttrSize(sizeof(rootAttr) + rootAttr.attr_size);

  offset += rootAttr.header.total_size;

  // $INDEX_ALLOCATION: one data run, kIndexBlockChainLength clusters starting
  // at kIndexBlockChainLcn.
  auto& allocAttr = *reinterpret_cast<NtfsBrowser::Attr::HeaderNonResident*>(
      &record.at(offset));
  allocAttr.header.type = AttrType::INDEX_ALLOCATION;
  allocAttr.header.non_resident = 1;
  allocAttr.header.name_length = 0;
  allocAttr.header.flags = 0;
  allocAttr.header.id = 0;
  allocAttr.start_vcn = 0;
  allocAttr.last_vcn = kIndexBlockChainLength - 1;
  allocAttr.data_run_offset = static_cast<WORD>(sizeof(allocAttr));
  allocAttr.comp_unit_size = 0;
  allocAttr.real_size = ULONGLONG{kIndexBlockChainLength} * kClusterSize;
  allocAttr.alloc_size = allocAttr.real_size;
  allocAttr.ini_size = allocAttr.real_size;

  const std::span<BYTE> dataRun =
      std::span<BYTE>(record).subspan(offset + allocAttr.data_run_offset);
  DWORD runLen = 0;
  // Data run header byte: high nibble = LCN offset field size (4 bytes),
  // low nibble = length field size (1 byte) - standard NTFS run encoding
  // (AttrNonResident::PickData, src/attr-non-resident.cpp).
  gsl::at(dataRun, runLen++) = kRunHeader4LcnBytes;
  gsl::at(dataRun, runLen++) = static_cast<BYTE>(kIndexBlockChainLength);
  {
    const DWORD lcn = kIndexBlockChainLcn;
    std::memcpy(&gsl::at(dataRun, runLen), &lcn, sizeof(lcn));
    runLen += sizeof(lcn);
  }
  gsl::at(dataRun, runLen++) = 0x00;  // terminate the run list

  allocAttr.header.total_size = AlignAttrSize(sizeof(allocAttr) + runLen);

  offset += allocAttr.header.total_size;

  WriteEndOfAttributesMarker(record, offset);
  return record;
}

// LCN where BuildFakeNtfsImageWithOrphanedIndexBlocks() writes its three
// index blocks, kept clear of every other fixture's placement in this file.
constexpr DWORD kOrphanedBlocksLcn = 200;

// Number of index blocks (VCN 0-2) the fixture's $INDEX_ALLOCATION covers.
constexpr DWORD kOrphanedBlocksCount = 3;

// Builds a root-directory replacement whose $INDEX_ROOT points only at
// VCN 0, while its $INDEX_ALLOCATION stream is sized for
// kOrphanedBlocksCount blocks - VCN 1 and 2 exist on "disk" but no pointer
// in the tree reaches them. declaredBlockCount, when different from
// kOrphanedBlocksCount, forges the attribute's own real_size (hence
// GetIndexBlockCount()) without changing the data run: blocks beyond
// kOrphanedBlocksCount are then declared but never actually backed.
FakeRecord MakeOrphanedIndexBlocksRootRecord(
    ULONGLONG declaredBlockCount = kOrphanedBlocksCount)
{
  FakeRecord record =
      MakeRecordHeader(kAttrOffset, NtfsBrowser::Flag::FileRecord::INUSE |
                                        NtfsBrowser::Flag::FileRecord::DIR);
  // The sequence its entries' parent references carry.
  EditFileRecordHeader(record, [](FileRecordHeader::Data& header)
                       { header.seq_no = kRootSequenceNumber; });

  DWORD offset = kAttrOffset;

  // $INDEX_ROOT
  auto& rootAttr =
      *reinterpret_cast<NtfsBrowser::Attr::HeaderResident*>(&record.at(offset));
  rootAttr.header.type = AttrType::INDEX_ROOT;
  rootAttr.header.non_resident = 0;
  rootAttr.header.name_length = 0;
  rootAttr.header.flags = 0;
  rootAttr.header.id = 0;
  rootAttr.attr_offset = static_cast<WORD>(sizeof(rootAttr));

  const std::span<BYTE> body =
      std::span<BYTE>(record).subspan(offset + rootAttr.attr_offset);
  auto& root = *reinterpret_cast<NtfsBrowser::Attr::IndexRoot*>(body.data());
  root.attr_type = AttrType::FILE_NAME;
  root.coll_rule = 0;
  root.ib_size = kClusterSize;
  root.clusters_per_ib = 1;
  root.entry_offset = gsl::narrow<DWORD>(
      (&gsl::at(body, sizeof(NtfsBrowser::Attr::IndexRoot))) -
      reinterpret_cast<BYTE*>(&root.entry_offset));

  // Sole entry: nameless, terminal, pointing at VCN 0 - the only block a
  // normal B+ tree walk reaches in this fixture.
  auto& first_entry = *reinterpret_cast<NtfsBrowser::Data::IndexEntry*>(
      &gsl::at(body, sizeof(NtfsBrowser::Attr::IndexRoot)));
  first_entry.mft_index = 0;
  first_entry.mft_sn = 0;
  first_entry.stream_size = 0;
  first_entry.flags = NtfsBrowser::Flag::IndexEntry::SUBNODE |
                      NtfsBrowser::Flag::IndexEntry::LAST;
  first_entry.size = static_cast<WORD>(AlignAttrSize(
      offsetof(NtfsBrowser::Data::IndexEntry, stream) + sizeof(ULONGLONG)));
  auto& subNodeVcn = SubNodeVcnSlot(first_entry);
  subNodeVcn = 0;

  root.total_entry_size = first_entry.size;
  root.alloc_entry_size = first_entry.size;
  root.flags = 0;

  rootAttr.attr_size =
      static_cast<DWORD>(sizeof(NtfsBrowser::Attr::IndexRoot)) +
      first_entry.size;
  rootAttr.header.total_size =
      AlignAttrSize(sizeof(rootAttr) + rootAttr.attr_size);

  offset += rootAttr.header.total_size;

  // $INDEX_ALLOCATION: kOrphanedBlocksCount contiguous blocks at
  // kOrphanedBlocksLcn, only the first ever pointed at from $INDEX_ROOT.
  auto& allocAttr = *reinterpret_cast<NtfsBrowser::Attr::HeaderNonResident*>(
      &record.at(offset));
  allocAttr.header.type = AttrType::INDEX_ALLOCATION;
  allocAttr.header.non_resident = 1;
  allocAttr.header.name_length = 0;
  allocAttr.header.flags = 0;
  allocAttr.header.id = 0;
  allocAttr.start_vcn = 0;
  allocAttr.last_vcn = kOrphanedBlocksCount - 1;
  allocAttr.data_run_offset = static_cast<WORD>(sizeof(allocAttr));
  allocAttr.comp_unit_size = 0;
  allocAttr.real_size = declaredBlockCount * kClusterSize;
  allocAttr.alloc_size = allocAttr.real_size;
  allocAttr.ini_size = allocAttr.real_size;

  const std::span<BYTE> dataRun =
      std::span<BYTE>(record).subspan(offset + allocAttr.data_run_offset);
  DWORD runLen = 0;
  // High nibble = LCN offset field size, low nibble = length field size.
  gsl::at(dataRun, runLen++) = kRunHeader4LcnBytes;
  gsl::at(dataRun, runLen++) = static_cast<BYTE>(kOrphanedBlocksCount);
  {
    const DWORD lcn = kOrphanedBlocksLcn;
    std::memcpy(&gsl::at(dataRun, runLen), &lcn, sizeof(lcn));
    runLen += sizeof(lcn);
  }
  gsl::at(dataRun, runLen++) = 0x00;  // terminate the run list

  allocAttr.header.total_size = AlignAttrSize(sizeof(allocAttr) + runLen);

  offset += allocAttr.header.total_size;

  WriteEndOfAttributesMarker(record, offset);
  return record;
}

// Writes a single-cluster index block at "vcn" (relative to kOrphanedBlocksLcn)
// holding one real leaf entry, into "image".
void WriteOrphanedIndexLeafBlock(std::vector<BYTE>& image, DWORD vcn,
                                 ULONGLONG mftRef, ULONGLONG parentRef,
                                 std::wstring_view name)
{
  const auto nameLength = gsl::narrow<BYTE>(name.size());
  const size_t blocksOffset =
      static_cast<size_t>(kOrphanedBlocksLcn) * kClusterSize;
  const std::span<BYTE> blockStart = std::span<BYTE>(image).subspan(
      blocksOffset + static_cast<size_t>(vcn) * kClusterSize);
  auto& block =
      *reinterpret_cast<NtfsBrowser::Data::IndexBlock*>(blockStart.data());
  std::memset(&block, 0, sizeof(block));
  block.magic = kIndexBlockMagic;
  // Points at the block's own last 6 bytes, so PatchUS() succeeds trivially
  // without a real fixup array.
  block.offset_of_us = static_cast<WORD>(kClusterSize - kUsSlotSize);
  block.size_of_us = 3;
  block.vcn = vcn;
  block.entry_offset = gsl::narrow<DWORD>(
      (&gsl::at(blockStart, sizeof(NtfsBrowser::Data::IndexBlock))) -
      reinterpret_cast<BYTE*>(&block.entry_offset));
  block.not_leaf = 0;

  const std::span<BYTE> body =
      blockStart.subspan(sizeof(NtfsBrowser::Data::IndexBlock));
  auto& first_entry =
      *reinterpret_cast<NtfsBrowser::Data::IndexEntry*>(body.data());
  first_entry.mft_index = mftRef;
  first_entry.mft_sn = 1;

  auto& fn1 =
      *reinterpret_cast<NtfsBrowser::Attr::Filename*>(&first_entry.stream);
  fn1.parent_ref = parentRef;
  fn1.flags = NtfsBrowser::Flag::Filename::NONE;
  fn1.name_length = nameLength;
  fn1.name_space = NtfsBrowser::Flag::FilenameNamespace::WIN_32;
  for (BYTE i = 0; i < nameLength; i++)
  {
    // nameLength is name.size(), so i < name.size() by the loop condition.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    fn1.name[i] = gsl::narrow<WORD>(name[i]);
  }

  first_entry.stream_size =
      gsl::narrow<WORD>(reinterpret_cast<BYTE*>(&fn1.name[nameLength]) -
                        reinterpret_cast<BYTE*>(&fn1));
  first_entry.flags = NtfsBrowser::Flag::IndexEntry::LAST;
  first_entry.size = gsl::narrow<WORD>(AlignAttrSize(
      &first_entry.stream - reinterpret_cast<BYTE*>(&first_entry) +
      first_entry.stream_size));

  block.total_entry_size = first_entry.size;
  block.alloc_entry_size = first_entry.size;
}

// LCN where BuildFakeNtfsImageWithMultiClusterOrphanedIndexBlock() writes
// its index blocks, kept clear of every other fixture's placement in this
// file.
constexpr DWORD kMultiClusterOrphanLcn = 210;

// Index block size in bytes for that fixture: more than one cluster, so the
// blockIndex-to-VCN scaling under test actually multiplies.
constexpr DWORD kMultiClusterOrphanIndexBlockSize =
    static_cast<DWORD>(kMultiClusterOrphanClustersPerBlock) * kClusterSize;

// Two real index blocks: block 0 (VCN 0, reachable) and block 1 (VCN
// kMultiClusterOrphanClustersPerBlock, orphaned).
constexpr DWORD kMultiClusterOrphanBlockCount = 2;

// Builds a root-directory replacement whose $INDEX_ROOT points only at
// block 0, while $INDEX_ALLOCATION covers kMultiClusterOrphanBlockCount
// blocks of kMultiClusterOrphanClustersPerBlock clusters each.
FakeRecord MakeMultiClusterOrphanedIndexBlocksRootRecord()
{
  FakeRecord record =
      MakeRecordHeader(kAttrOffset, NtfsBrowser::Flag::FileRecord::INUSE |
                                        NtfsBrowser::Flag::FileRecord::DIR);
  // The sequence its entries' parent references carry.
  EditFileRecordHeader(record, [](FileRecordHeader::Data& header)
                       { header.seq_no = kRootSequenceNumber; });

  DWORD offset = kAttrOffset;

  // $INDEX_ROOT: a single sub-node pointer at VCN 0 (block 0).
  auto& rootAttr =
      *reinterpret_cast<NtfsBrowser::Attr::HeaderResident*>(&record.at(offset));
  rootAttr.header.type = AttrType::INDEX_ROOT;
  rootAttr.header.non_resident = 0;
  rootAttr.header.name_length = 0;
  rootAttr.header.flags = 0;
  rootAttr.header.id = 0;
  rootAttr.attr_offset = static_cast<WORD>(sizeof(rootAttr));

  const std::span<BYTE> body =
      std::span<BYTE>(record).subspan(offset + rootAttr.attr_offset);
  auto& root = *reinterpret_cast<NtfsBrowser::Attr::IndexRoot*>(body.data());
  root.attr_type = AttrType::FILE_NAME;
  root.coll_rule = 0;
  root.ib_size = kMultiClusterOrphanIndexBlockSize;
  root.clusters_per_ib = kMultiClusterOrphanClustersPerBlock;
  root.entry_offset = gsl::narrow<DWORD>(
      (&gsl::at(body, sizeof(NtfsBrowser::Attr::IndexRoot))) -
      reinterpret_cast<BYTE*>(&root.entry_offset));

  auto& first_entry = *reinterpret_cast<NtfsBrowser::Data::IndexEntry*>(
      &gsl::at(body, sizeof(NtfsBrowser::Attr::IndexRoot)));
  first_entry.mft_index = 0;
  first_entry.mft_sn = 0;
  first_entry.stream_size = 0;
  first_entry.flags = NtfsBrowser::Flag::IndexEntry::SUBNODE |
                      NtfsBrowser::Flag::IndexEntry::LAST;
  first_entry.size = static_cast<WORD>(AlignAttrSize(
      offsetof(NtfsBrowser::Data::IndexEntry, stream) + sizeof(ULONGLONG)));
  auto& subNodeVcn = SubNodeVcnSlot(first_entry);
  subNodeVcn = 0;  // block 0

  root.total_entry_size = first_entry.size;
  root.alloc_entry_size = first_entry.size;
  root.flags = 0;

  rootAttr.attr_size =
      static_cast<DWORD>(sizeof(NtfsBrowser::Attr::IndexRoot)) +
      first_entry.size;
  rootAttr.header.total_size =
      AlignAttrSize(sizeof(rootAttr) + rootAttr.attr_size);

  offset += rootAttr.header.total_size;

  // $INDEX_ALLOCATION: kMultiClusterOrphanBlockCount contiguous blocks at
  // kMultiClusterOrphanLcn, only block 0 ever pointed at from $INDEX_ROOT.
  auto& allocAttr = *reinterpret_cast<NtfsBrowser::Attr::HeaderNonResident*>(
      &record.at(offset));
  allocAttr.header.type = AttrType::INDEX_ALLOCATION;
  allocAttr.header.non_resident = 1;
  allocAttr.header.name_length = 0;
  allocAttr.header.flags = 0;
  allocAttr.header.id = 0;
  allocAttr.start_vcn = 0;
  const DWORD totalClusters =
      static_cast<DWORD>(kMultiClusterOrphanBlockCount) *
      kMultiClusterOrphanClustersPerBlock;
  allocAttr.last_vcn = totalClusters - 1;
  allocAttr.data_run_offset = static_cast<WORD>(sizeof(allocAttr));
  allocAttr.comp_unit_size = 0;
  allocAttr.real_size = static_cast<ULONGLONG>(kMultiClusterOrphanBlockCount) *
                        kMultiClusterOrphanIndexBlockSize;
  allocAttr.alloc_size = allocAttr.real_size;
  allocAttr.ini_size = allocAttr.real_size;

  const std::span<BYTE> dataRun =
      std::span<BYTE>(record).subspan(offset + allocAttr.data_run_offset);
  DWORD runLen = 0;
  // High nibble = LCN offset field size, low nibble = length field size.
  gsl::at(dataRun, runLen++) = kRunHeader4LcnBytes;
  gsl::at(dataRun, runLen++) = gsl::narrow<BYTE>(totalClusters);
  {
    const DWORD lcn = kMultiClusterOrphanLcn;
    std::memcpy(&gsl::at(dataRun, runLen), &lcn, sizeof(lcn));
    runLen += sizeof(lcn);
  }
  gsl::at(dataRun, runLen++) = 0x00;  // terminate the run list

  allocAttr.header.total_size = AlignAttrSize(sizeof(allocAttr) + runLen);

  offset += allocAttr.header.total_size;

  WriteEndOfAttributesMarker(record, offset);
  return record;
}

// Writes a real index block at block index "blockIndex" (VCN blockIndex *
// kMultiClusterOrphanClustersPerBlock), holding one real leaf entry.
void WriteMultiClusterIndexLeafBlock(std::vector<BYTE>& image, DWORD blockIndex,
                                     ULONGLONG mftRef, ULONGLONG parentRef,
                                     std::wstring_view name)
{
  const auto nameLength = gsl::narrow<BYTE>(name.size());
  const ULONGLONG vcn =
      static_cast<ULONGLONG>(blockIndex) * kMultiClusterOrphanClustersPerBlock;
  const size_t blocksOffset =
      static_cast<size_t>(kMultiClusterOrphanLcn) * kClusterSize;
  const std::span<BYTE> blockStart = std::span<BYTE>(image).subspan(
      blocksOffset + gsl::narrow<size_t>(vcn) * kClusterSize);
  auto& block =
      *reinterpret_cast<NtfsBrowser::Data::IndexBlock*>(blockStart.data());
  std::memset(&block, 0, sizeof(block));
  block.magic = kIndexBlockMagic;
  // Points the USA at the block's own last (1 + sectors) words, so every
  // PatchUS() check compares a still-zero byte range to itself and the
  // whole array trivially self-patches, without a real fixup array - the
  // multi-sector generalization of the single-cluster fixtures' same trick.
  const DWORD sectors = kMultiClusterOrphanIndexBlockSize / kBytesPerSector;
  block.offset_of_us =
      gsl::narrow<WORD>(kMultiClusterOrphanIndexBlockSize - 2 * (1 + sectors));
  block.size_of_us = gsl::narrow<WORD>(1 + sectors);
  block.vcn = vcn;
  block.entry_offset = gsl::narrow<DWORD>(
      (&gsl::at(blockStart, sizeof(NtfsBrowser::Data::IndexBlock))) -
      reinterpret_cast<BYTE*>(&block.entry_offset));
  block.not_leaf = 0;

  const std::span<BYTE> body =
      blockStart.subspan(sizeof(NtfsBrowser::Data::IndexBlock));
  auto& first_entry =
      *reinterpret_cast<NtfsBrowser::Data::IndexEntry*>(body.data());
  first_entry.mft_index = mftRef;
  first_entry.mft_sn = 1;

  auto& fn1 =
      *reinterpret_cast<NtfsBrowser::Attr::Filename*>(&first_entry.stream);
  fn1.parent_ref = parentRef;
  fn1.flags = NtfsBrowser::Flag::Filename::NONE;
  fn1.name_length = nameLength;
  fn1.name_space = NtfsBrowser::Flag::FilenameNamespace::WIN_32;
  for (BYTE i = 0; i < nameLength; i++)
  {
    // nameLength is name.size(), so i < name.size() by the loop condition.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    fn1.name[i] = gsl::narrow<WORD>(name[i]);
  }

  first_entry.stream_size =
      gsl::narrow<WORD>(reinterpret_cast<BYTE*>(&fn1.name[nameLength]) -
                        reinterpret_cast<BYTE*>(&fn1));
  first_entry.flags = NtfsBrowser::Flag::IndexEntry::LAST;
  first_entry.size = gsl::narrow<WORD>(AlignAttrSize(
      &first_entry.stream - reinterpret_cast<BYTE*>(&first_entry) +
      first_entry.stream_size));

  block.total_entry_size = first_entry.size;
  block.alloc_entry_size = first_entry.size;
}

// One $INDEX_ALLOCATION instance built by MakeDirectoryWithIndexAllocation():
// "clusters" clusters at "lcn", from virtual cluster "start_vcn" on.
struct FakeIndexAllocExtent
{
  ULONGLONG start_vcn;
  DWORD clusters;
  DWORD lcn;
};

// Builds a root-directory replacement, sequence kRootSequenceNumber, whose
// $INDEX_ROOT points only at block 0. Its $INDEX_ALLOCATION has one instance
// per extent. The first declares declaredBlockCount blocks of ibSize bytes.
// The others declare no size, as a real continuation does.
FakeRecord MakeDirectoryWithIndexAllocation(
    DWORD ibSize, std::span<const FakeIndexAllocExtent> extents,
    ULONGLONG declaredBlockCount)
{
  FakeRecord record =
      MakeRecordHeader(kAttrOffset, NtfsBrowser::Flag::FileRecord::INUSE |
                                        NtfsBrowser::Flag::FileRecord::DIR);
  EditFileRecordHeader(record, [](FileRecordHeader::Data& header)
                       { header.seq_no = kRootSequenceNumber; });

  DWORD offset = kAttrOffset;

  auto& rootAttr =
      *reinterpret_cast<NtfsBrowser::Attr::HeaderResident*>(&record.at(offset));
  rootAttr.header.type = AttrType::INDEX_ROOT;
  rootAttr.header.non_resident = 0;
  rootAttr.header.name_length = 0;
  rootAttr.header.flags = 0;
  rootAttr.header.id = 0;
  rootAttr.attr_offset = static_cast<WORD>(sizeof(rootAttr));

  const std::span<BYTE> body =
      std::span<BYTE>(record).subspan(offset + rootAttr.attr_offset);
  auto& root = *reinterpret_cast<NtfsBrowser::Attr::IndexRoot*>(body.data());
  root.attr_type = AttrType::FILE_NAME;
  root.coll_rule = 0;
  root.ib_size = ibSize;
  root.clusters_per_ib =
      gsl::narrow<BYTE>(ibSize >= kClusterSize ? ibSize / kClusterSize : 1);
  root.entry_offset = gsl::narrow<DWORD>(
      (&gsl::at(body, sizeof(NtfsBrowser::Attr::IndexRoot))) -
      reinterpret_cast<BYTE*>(&root.entry_offset));

  auto& first_entry = *reinterpret_cast<NtfsBrowser::Data::IndexEntry*>(
      &gsl::at(body, sizeof(NtfsBrowser::Attr::IndexRoot)));
  first_entry.mft_index = 0;
  first_entry.mft_sn = 0;
  first_entry.stream_size = 0;
  first_entry.flags = NtfsBrowser::Flag::IndexEntry::SUBNODE |
                      NtfsBrowser::Flag::IndexEntry::LAST;
  first_entry.size = static_cast<WORD>(AlignAttrSize(
      offsetof(NtfsBrowser::Data::IndexEntry, stream) + sizeof(ULONGLONG)));
  auto& subNodeVcn = SubNodeVcnSlot(first_entry);
  subNodeVcn = 0;  // block 0

  root.total_entry_size = first_entry.size;
  root.alloc_entry_size = first_entry.size;
  root.flags = 0;

  rootAttr.attr_size =
      static_cast<DWORD>(sizeof(NtfsBrowser::Attr::IndexRoot)) +
      first_entry.size;
  rootAttr.header.total_size =
      AlignAttrSize(sizeof(rootAttr) + rootAttr.attr_size);

  offset += rootAttr.header.total_size;

  bool first = true;
  for (const FakeIndexAllocExtent& extent : extents)
  {
    auto& allocAttr = *reinterpret_cast<NtfsBrowser::Attr::HeaderNonResident*>(
        &record.at(offset));
    allocAttr.header.type = AttrType::INDEX_ALLOCATION;
    allocAttr.header.non_resident = 1;
    allocAttr.header.name_length = 0;
    allocAttr.header.flags = 0;
    allocAttr.header.id = 0;
    allocAttr.start_vcn = extent.start_vcn;
    allocAttr.last_vcn = extent.start_vcn + extent.clusters - 1;
    allocAttr.data_run_offset = static_cast<WORD>(sizeof(allocAttr));
    allocAttr.comp_unit_size = 0;
    allocAttr.real_size = first ? declaredBlockCount * ibSize : 0;
    allocAttr.alloc_size = allocAttr.real_size;
    allocAttr.ini_size = allocAttr.real_size;
    first = false;

    const std::span<BYTE> dataRun =
        std::span<BYTE>(record).subspan(offset + allocAttr.data_run_offset);
    DWORD runLen = 0;
    // High nibble = LCN offset field size, low nibble = length field size.
    gsl::at(dataRun, runLen++) = kRunHeader4LcnBytes;
    gsl::at(dataRun, runLen++) = gsl::narrow<BYTE>(extent.clusters);
    std::memcpy(&gsl::at(dataRun, runLen), &extent.lcn, sizeof(extent.lcn));
    runLen += sizeof(extent.lcn);
    gsl::at(dataRun, runLen++) = 0x00;  // terminate the run list

    allocAttr.header.total_size = AlignAttrSize(sizeof(allocAttr) + runLen);
    offset += allocAttr.header.total_size;
  }

  WriteEndOfAttributesMarker(record, offset);
  return record;
}

// Writes an index block of blockSize bytes at byte offset blockOffset, holding
// one real leaf entry named "name", filed under parentRef.
void WriteIndexLeafBlockAt(std::vector<BYTE>& image, size_t blockOffset,
                           DWORD blockSize, ULONGLONG vcn, ULONGLONG mftRef,
                           ULONGLONG parentRef, std::wstring_view name)
{
  const std::span<BYTE> blockStart =
      std::span<BYTE>(image).subspan(blockOffset);
  auto& block =
      *reinterpret_cast<NtfsBrowser::Data::IndexBlock*>(blockStart.data());
  std::memset(&block, 0, sizeof(block));
  block.magic = kIndexBlockMagic;
  // Points the USA at the block's own last (1 + sectors) words, so PatchUS()
  // compares a still-zero range to itself, without a real fixup array.
  const DWORD sectors = blockSize / kBytesPerSector;
  block.offset_of_us = gsl::narrow<WORD>(blockSize - 2 * (1 + sectors));
  block.size_of_us = gsl::narrow<WORD>(1 + sectors);
  block.vcn = vcn;
  block.entry_offset = gsl::narrow<DWORD>(
      (&gsl::at(blockStart, sizeof(NtfsBrowser::Data::IndexBlock))) -
      reinterpret_cast<BYTE*>(&block.entry_offset));
  block.not_leaf = 0;

  const std::span<BYTE> body =
      blockStart.subspan(sizeof(NtfsBrowser::Data::IndexBlock));
  auto& first_entry =
      *reinterpret_cast<NtfsBrowser::Data::IndexEntry*>(body.data());
  first_entry.mft_index = mftRef;
  first_entry.mft_sn = 1;

  auto& fn1 =
      *reinterpret_cast<NtfsBrowser::Attr::Filename*>(&first_entry.stream);
  fn1.parent_ref = parentRef;
  fn1.flags = NtfsBrowser::Flag::Filename::NONE;
  fn1.name_length = gsl::narrow<BYTE>(name.size());
  fn1.name_space = NtfsBrowser::Flag::FilenameNamespace::WIN_32;
  for (size_t i = 0; i < name.size(); i++)
  {
    // nameLength is name.size(), so i < name.size() by the loop condition.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    fn1.name[i] = gsl::narrow<WORD>(name[i]);
  }

  first_entry.stream_size =
      gsl::narrow<WORD>(reinterpret_cast<BYTE*>(&fn1.name[name.size()]) -
                        reinterpret_cast<BYTE*>(&fn1));
  first_entry.flags = NtfsBrowser::Flag::IndexEntry::LAST;
  first_entry.size = gsl::narrow<WORD>(AlignAttrSize(
      &first_entry.stream - reinterpret_cast<BYTE*>(&first_entry) +
      first_entry.stream_size));

  block.total_entry_size = first_entry.size;
  block.alloc_entry_size = first_entry.size;
}

// Clusters BuildFakeNtfsImageWithSubClusterOrphanedIndexBlocks()'s
// $INDEX_ALLOCATION maps: two 512-byte blocks per 1024-byte cluster, so four
// blocks in all.
constexpr DWORD kSubClusterAllocClusters = 2;

// Clusters each of BuildFakeNtfsImageWithSplitIndexAllocation()'s two
// $INDEX_ALLOCATION instances maps: one block per cluster, so four blocks in
// all.
constexpr DWORD kSplitExtentClusters = 2;

////////////////////////////////////////////////////////////////////////////
// NTFS compression fixtures (see fake-ntfs-image.h for what each builds)
////////////////////////////////////////////////////////////////////////////

// AttrHeaderCommon::flags bit 0 ("compressed"); unread by the library itself
// but set here since a real compressed attribute always sets it too.
constexpr WORD kAttrFlagCompressed = 0x0001;

// Encodes "runs" into NTFS' real, delta-LCN run-list format at "dataRun"
// (terminated by 0x00), and returns the byte count written.
DWORD EncodeDataRuns(std::span<BYTE> dataRun,
                     const std::vector<FakeDataRun>& runs)
{
  DWORD runLen = 0;
  DWORD previousLcn = 0;

  for (const FakeDataRun& run : runs)
  {
    if (run.lcn)
    {
      gsl::at(dataRun, runLen++) = kRunHeader4LcnBytes;
      gsl::at(dataRun, runLen++) = gsl::narrow<BYTE>(run.clusters);
      const LONG delta =
          gsl::narrow<LONG>(*run.lcn) - gsl::narrow<LONG>(previousLcn);
      std::memcpy(&gsl::at(dataRun, runLen), &delta, sizeof(delta));
      runLen += sizeof(delta);
      previousLcn = *run.lcn;
    }
    else
    {
      gsl::at(dataRun, runLen++) = 0x01;
      gsl::at(dataRun, runLen++) = gsl::narrow<BYTE>(run.clusters);
    }
  }

  gsl::at(dataRun, runLen++) = 0x00;  // terminate the run list
  return runLen;
}

// Writes one resident $STANDARD_INFORMATION attribute at record[offset]
// with the given DOS permission bits, and returns its total_size.
DWORD WriteStandardInformationAttr(
    FakeRecord& record, DWORD offset,
    NtfsBrowser::Flag::StdInfoPermission permission)
{
  auto& attr =
      *reinterpret_cast<NtfsBrowser::Attr::HeaderResident*>(&record.at(offset));
  attr.header.type = AttrType::STANDARD_INFORMATION;
  attr.header.non_resident = 0;
  attr.header.name_length = 0;
  attr.header.flags = 0;
  attr.header.id = 0;
  attr.attr_size =
      static_cast<DWORD>(sizeof(NtfsBrowser::Attr::StandardInformation));
  attr.attr_offset = static_cast<WORD>(sizeof(attr));
  attr.header.total_size = AlignAttrSize(sizeof(attr) + attr.attr_size);

  auto& stdInfo = *reinterpret_cast<NtfsBrowser::Attr::StandardInformation*>(
      &record.at(offset + attr.attr_offset));
  stdInfo.create_time = kStdInfoCreateTime;
  stdInfo.alter_time = kStdInfoAlterTime;
  stdInfo.mft_time = kStdInfoMftTime;
  stdInfo.read_time = kStdInfoReadTime;
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
  // Overrides the initialized size, which defaults to the real size.
  std::optional<ULONGLONG> ini_size;
  // Hand-encoded run list, terminator included, written instead of the
  // encoding of "runs": for LCNs EncodeDataRuns() cannot express.
  std::vector<BYTE> raw_runs;
};

// Writes one non-resident attribute at record[offset] and returns its
// declared total_size. compUnitSize == 0 is ordinary; non-zero adds the
// trailing 8-byte CompressedSize field.
DWORD WriteNonResidentAttr(FakeRecord& record, DWORD offset, AttrType type,
                           WORD compUnitSize, ULONGLONG realSize,
                           const std::vector<FakeDataRun>& runs,
                           const FakeNonResidentOverrides& overrides = {})
{
  auto& attr = *reinterpret_cast<NtfsBrowser::Attr::HeaderNonResident*>(
      &record.at(offset));
  attr.header.type = type;
  attr.header.non_resident = 1;
  const std::vector<WORD> encodedName = ToUtf16(overrides.name);
  assert(encodedName.size() <= 255 && "on-disk name_length is one byte");
  attr.header.name_length = gsl::narrow<BYTE>(encodedName.size());
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
  attr.ini_size = overrides.ini_size.value_or(realSize);

  const auto headerSize = gsl::narrow<WORD>(
      sizeof(attr) +
      ((compUnitSize != 0) ? NtfsBrowser::Attr::kCompressedSizeFieldSize : 0));
  const size_t nameBytes = encodedName.size() * sizeof(WORD);
  if (nameBytes != 0)
  {
    attr.header.name_offset = headerSize;
    std::memcpy(&record.at(offset + headerSize), encodedName.data(), nameBytes);
  }
  const auto runOffset = gsl::narrow<WORD>(headerSize + nameBytes);
  attr.data_run_offset = runOffset;

  if (compUnitSize != 0)
  {
    // CompressedSize: total allocated size of the attribute's compressed
    // clusters, i.e. everything actually on disk (the sparse padding of each
    // compressed unit excluded).
    const ULONGLONG compressedSize = realClusters * kClusterSize;
    std::memcpy(&record.at(offset + sizeof(attr)), &compressedSize,
                sizeof(compressedSize));
  }

  DWORD runLen = 0;
  if (overrides.raw_runs.empty())
  {
    runLen = EncodeDataRuns(std::span<BYTE>(record).subspan(offset + runOffset),
                            runs);
  }
  else
  {
    runLen = gsl::narrow<DWORD>(overrides.raw_runs.size());
    std::memcpy(&record.at(offset + runOffset), overrides.raw_runs.data(),
                runLen);
  }
  attr.header.total_size =
      overrides.total_size.value_or(AlignAttrSize(runOffset + runLen));
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

// Writes "name" as a leaf index entry at "dest" and returns its size in
// bytes. Entries are packed with no padding, like every other fixture here.
WORD WriteFilenameEntry(BYTE* dest, const FakeIndexName& name)
{
  auto& entry = *reinterpret_cast<NtfsBrowser::Data::IndexEntry*>(dest);
  entry.mft_index = name.mft_ref;
  entry.mft_sn = 1;

  auto& filename =
      *reinterpret_cast<NtfsBrowser::Attr::Filename*>(&entry.stream);
  filename.parent_ref = static_cast<ULONGLONG>(MftIdx::ROOT);
  filename.flags = name.directory ? NtfsBrowser::Flag::Filename::DIRECTORY
                                  : NtfsBrowser::Flag::Filename::NONE;
  const std::vector<WORD> encodedName = ToUtf16(name.name);
  filename.name_length = gsl::narrow<BYTE>(encodedName.size());
  filename.name_space = NtfsBrowser::Flag::FilenameNamespace::WIN_32;
  for (size_t i = 0; i < encodedName.size(); i++)
  {
    filename.name[i] = encodedName.at(i);
  }

  entry.stream_size = gsl::narrow<WORD>(
      reinterpret_cast<BYTE*>(&filename.name[filename.name_length]) -
      reinterpret_cast<BYTE*>(&filename));
  entry.size = gsl::narrow<WORD>(AlignAttrSize(
      &entry.stream - reinterpret_cast<BYTE*>(&entry) + entry.stream_size));
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
      *reinterpret_cast<NtfsBrowser::Attr::HeaderResident*>(&record.at(offset));
  rootAttr.header.type = AttrType::INDEX_ROOT;
  rootAttr.header.non_resident = 0;
  rootAttr.header.name_length = 0;
  rootAttr.header.flags = 0;
  rootAttr.header.id = 0;
  rootAttr.attr_offset = static_cast<WORD>(sizeof(rootAttr));

  const std::span<BYTE> body =
      std::span<BYTE>(record).subspan(offset + rootAttr.attr_offset);
  auto& root = *reinterpret_cast<NtfsBrowser::Attr::IndexRoot*>(body.data());
  root.attr_type = AttrType::FILE_NAME;
  root.coll_rule = 0;
  root.ib_size = kFakeFileRecordSize;
  root.clusters_per_ib = 1;
  root.entry_offset = gsl::narrow<DWORD>(
      (&gsl::at(body, sizeof(NtfsBrowser::Attr::IndexRoot))) -
      reinterpret_cast<BYTE*>(&root.entry_offset));

  const std::span<BYTE> entries =
      body.subspan(sizeof(NtfsBrowser::Attr::IndexRoot));
  DWORD leafBytes = 0;
  for (const FakeIndexName& name : rootNames)
  {
    leafBytes += WriteFilenameEntry(&gsl::at(entries, leafBytes), name);
  }

  auto& first_entry = *reinterpret_cast<NtfsBrowser::Data::IndexEntry*>(
      &gsl::at(entries, leafBytes));
  first_entry.mft_index = 0;
  first_entry.mft_sn = 0;
  first_entry.stream_size = 0;
  first_entry.flags = NtfsBrowser::Flag::IndexEntry::SUBNODE |
                      NtfsBrowser::Flag::IndexEntry::LAST;
  first_entry.size = static_cast<WORD>(AlignAttrSize(
      offsetof(NtfsBrowser::Data::IndexEntry, stream) + sizeof(ULONGLONG)));
  auto& subNodeVcn = SubNodeVcnSlot(first_entry);
  subNodeVcn = 0;

  root.total_entry_size = leafBytes + first_entry.size;
  root.alloc_entry_size = leafBytes + first_entry.size;
  root.flags = 0;

  rootAttr.attr_size =
      static_cast<DWORD>(sizeof(NtfsBrowser::Attr::IndexRoot)) + leafBytes +
      first_entry.size;
  rootAttr.header.total_size =
      AlignAttrSize(sizeof(rootAttr) + rootAttr.attr_size);

  offset += rootAttr.header.total_size;
  offset += WriteNonResidentAttr(record, offset, AttrType::INDEX_ALLOCATION,
                                 compUnitSize, realSize, runs, overrides);

  WriteEndOfAttributesMarker(record, offset);
  return record;
}

// Directory record (root, #5): a resident $INDEX_ROOT holding "names" as leaf
// entries, then the terminating entry.
FakeRecord MakeIndexRootDirRecordWithNames(std::span<const FakeIndexName> names)
{
  FakeRecord record =
      MakeRecordHeader(kAttrOffset, NtfsBrowser::Flag::FileRecord::INUSE |
                                        NtfsBrowser::Flag::FileRecord::DIR);

  auto& attr = *reinterpret_cast<NtfsBrowser::Attr::HeaderResident*>(
      &record.at(kAttrOffset));
  attr.header.type = AttrType::INDEX_ROOT;
  attr.header.non_resident = 0;
  attr.header.name_length = 0;
  attr.header.flags = 0;
  attr.header.id = 0;
  attr.attr_offset = static_cast<WORD>(sizeof(attr));

  const std::span<BYTE> body =
      std::span<BYTE>(record).subspan(kAttrOffset + attr.attr_offset);
  auto& root = *reinterpret_cast<NtfsBrowser::Attr::IndexRoot*>(body.data());
  root.attr_type = AttrType::FILE_NAME;
  root.coll_rule = 0;
  root.ib_size = kFakeFileRecordSize;
  root.clusters_per_ib = 1;
  root.entry_offset = gsl::narrow<DWORD>(
      (&gsl::at(body, sizeof(NtfsBrowser::Attr::IndexRoot))) -
      reinterpret_cast<BYTE*>(&root.entry_offset));

  const std::span<BYTE> entries =
      body.subspan(sizeof(NtfsBrowser::Attr::IndexRoot));
  DWORD leafBytes = 0;
  for (const FakeIndexName& name : names)
  {
    leafBytes += WriteFilenameEntry(&gsl::at(entries, leafBytes), name);
  }

  auto& last = *reinterpret_cast<NtfsBrowser::Data::IndexEntry*>(
      &gsl::at(entries, leafBytes));
  last.flags = NtfsBrowser::Flag::IndexEntry::LAST;
  last.stream_size = 0;
  last.size = gsl::narrow<WORD>(
      AlignAttrSize(&last.stream - reinterpret_cast<BYTE*>(&last)));

  root.total_entry_size = leafBytes + last.size;
  root.alloc_entry_size = root.total_entry_size;
  root.flags = 0;

  attr.attr_size = static_cast<DWORD>(sizeof(NtfsBrowser::Attr::IndexRoot)) +
                   leafBytes + last.size;
  attr.header.total_size = AlignAttrSize(sizeof(attr) + attr.attr_size);

  WriteEndOfAttributesMarker(record, kAttrOffset + attr.header.total_size);
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
  constexpr size_t kFullCacheReadBlockSize = size_t{64} * 1024;
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
    std::memcpy(&image.at(static_cast<size_t>(*run.lcn) *
                          static_cast<size_t>(kClusterSize)),
                &clusterBytes.at(written), chunk);
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
  std::memcpy(&image.at(rootOffset), record.data(), record.size());

  LayRunBytes(image, runs, clusterBytes);
  return image;
}

// The malformed LZNT1 bytes both corrupt-compression fixtures use: a
// well-formed compressed chunk header, then a compressed word whose
// displacement (1) reaches before anything has been decompressed yet.
std::vector<BYTE> MakeCorruptLznt1Chunk()
{
  return {kCorruptChunkHeaderLow, kCorruptChunkHeaderHigh, 0x01, 0x00, 0x00};
}

// The 1024-byte "INDX"-signed index block a compressed $INDEX_ALLOCATION
// decompresses to: "names" as leaf FILE_NAME entries plus the terminating
// entry. Built standalone since it is the *decompressed* content, wrapped
// into an LZNT1 chunk by the caller.
std::vector<BYTE> MakeIndexBlockContent(std::span<const FakeIndexName> names)
{
  std::vector<BYTE> content(kFakeFileRecordSize, 0);

  const std::span<BYTE> blockStart = content;
  auto& block =
      *reinterpret_cast<NtfsBrowser::Data::IndexBlock*>(blockStart.data());
  block.magic = kIndexBlockMagic;
  // Points offset_of_us at the fixup slot itself, so PatchUS() trivially
  // succeeds - same technique as BuildFakeNtfsImageWithGapCollationSubNode().
  block.offset_of_us = static_cast<WORD>(kFakeFileRecordSize - kUsSlotSize);
  block.size_of_us = 3;
  block.vcn = 0;
  block.entry_offset = gsl::narrow<DWORD>(
      (&gsl::at(blockStart, sizeof(NtfsBrowser::Data::IndexBlock))) -
      reinterpret_cast<BYTE*>(&block.entry_offset));
  block.not_leaf = 0;

  const std::span<BYTE> body =
      blockStart.subspan(sizeof(NtfsBrowser::Data::IndexBlock));

  DWORD leafBytes = 0;
  for (const FakeIndexName& name : names)
  {
    leafBytes += WriteFilenameEntry(&gsl::at(body, leafBytes), name);
  }

  auto& last = *reinterpret_cast<NtfsBrowser::Data::IndexEntry*>(
      &gsl::at(body, leafBytes));
  last.flags = NtfsBrowser::Flag::IndexEntry::LAST;
  last.stream_size = 0;
  last.size = gsl::narrow<WORD>(
      AlignAttrSize(&last.stream - reinterpret_cast<BYTE*>(&last)));

  block.total_entry_size = leafBytes + last.size;
  block.alloc_entry_size = block.total_entry_size;

  return content;
}

// The single "Comp" entry every compressed $INDEX_ALLOCATION fixture but
// the surrogate-pair one decompresses to.
std::vector<BYTE> MakeCompressedIndexBlockContent()
{
  const FakeIndexName comp{.name = kCompressedIndexEntryName,
                           .mft_ref = kCompressedIndexEntryMftRef,
                           .directory = false};
  return MakeIndexBlockContent(std::span(&comp, 1));
}

// One $FILE_NAME of a BuildFakeNtfsImageWithMftTree() record.
struct FakeFileName
{
  std::wstring_view name;
  ULONGLONG parent_ref;
  NtfsBrowser::Flag::FilenameNamespace name_space =
      NtfsBrowser::Flag::FilenameNamespace::WIN_32;
  // The size NTFS only refreshes in $FILE_NAME on a rename.
  ULONGLONG real_size = 0;
  // ORed onto the DIRECTORY/NONE flag WriteFileNameAttr() derives on its own,
  // eg. for a permission bit $FILE_NAME mirrors from $STANDARD_INFORMATION.
  NtfsBrowser::Flag::Filename extra_flags = NtfsBrowser::Flag::Filename::NONE;
};

// Writes one resident $FILE_NAME at record[offset] and returns its
// total_size.
DWORD WriteFileNameAttr(FakeRecord& record, DWORD offset,
                        const FakeFileName& name, bool directory)
{
  auto& attr =
      *reinterpret_cast<NtfsBrowser::Attr::HeaderResident*>(&record.at(offset));
  attr.header.type = AttrType::FILE_NAME;
  attr.header.non_resident = 0;
  attr.header.name_length = 0;
  attr.header.flags = 0;
  attr.header.id = 0;
  attr.attr_offset = static_cast<WORD>(sizeof(attr));
  attr.attr_size =
      gsl::narrow<DWORD>(offsetof(NtfsBrowser::Attr::Filename, name) +
                         name.name.size() * sizeof(WORD));
  attr.header.total_size = AlignAttrSize(sizeof(attr) + attr.attr_size);

  auto& filename = *reinterpret_cast<NtfsBrowser::Attr::Filename*>(
      &record.at(offset + attr.attr_offset));
  filename.parent_ref = name.parent_ref;
  filename.real_size = name.real_size;
  filename.alloc_size = name.real_size;
  filename.flags = (directory ? NtfsBrowser::Flag::Filename::DIRECTORY
                              : NtfsBrowser::Flag::Filename::NONE) |
                   name.extra_flags;
  filename.name_length = gsl::narrow<BYTE>(name.name.size());
  filename.name_space = name.name_space;
  for (size_t i = 0; i < name.name.size(); i++)
  {
    // i < name.name.size() by the loop condition.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    filename.name[i] = gsl::narrow<WORD>(name.name[i]);
  }

  return attr.header.total_size;
}

// Writes one resident, unnamed $DATA of size zero bytes at record[offset]
// and returns its total_size.
DWORD WriteResidentDataAttr(FakeRecord& record, DWORD offset, DWORD size)
{
  auto& attr =
      *reinterpret_cast<NtfsBrowser::Attr::HeaderResident*>(&record.at(offset));
  attr.header.type = AttrType::DATA;
  attr.header.non_resident = 0;
  attr.header.name_length = 0;
  attr.header.flags = 0;
  attr.header.id = 0;
  attr.attr_offset = static_cast<WORD>(sizeof(attr));
  attr.attr_size = size;
  attr.header.total_size = AlignAttrSize(sizeof(attr) + attr.attr_size);
  return attr.header.total_size;
}

// Builds one record of BuildFakeNtfsImageWithMftTree(): a header carrying
// sequence and baseRef, a $STANDARD_INFORMATION, names, and, when dataSize is
// set, a resident $DATA of that size.
FakeRecord MakeMftTreeRecord(NtfsBrowser::Flag::FileRecord flags, WORD sequence,
                             std::initializer_list<FakeFileName> names,
                             std::optional<DWORD> dataSize = {},
                             NtfsBrowser::Flag::StdInfoPermission permission =
                                 NtfsBrowser::Flag::StdInfoPermission::NORMAL,
                             ULONGLONG baseRef = 0)
{
  FakeRecord record = MakeRecordHeader(kAttrOffset, flags);
  EditFileRecordHeader(record,
                       [&](FileRecordHeader::Data& header)
                       {
                         header.seq_no = sequence;
                         header.ref_to_base = baseRef;
                       });

  const bool directory = (flags & NtfsBrowser::Flag::FileRecord::DIR) ==
                         NtfsBrowser::Flag::FileRecord::DIR;

  DWORD offset = kAttrOffset;
  offset += WriteStandardInformationAttr(record, offset, permission);
  for (const FakeFileName& name : names)
  {
    offset += WriteFileNameAttr(record, offset, name, directory);
  }
  if (dataSize)
  {
    offset += WriteResidentDataAttr(record, offset, *dataSize);
  }

  WriteEndOfAttributesMarker(record, offset);
  return record;
}

}  // namespace

std::vector<BYTE> BuildFakeNtfsImage()
{
  NtfsBrowser::Data::NtfsBpb bpb{};
  std::memcpy(bpb.signature, NtfsBrowser::Data::kNtfsSignature.data(),
              sizeof(bpb.signature));
  bpb.bytes_per_sector = kBytesPerSector;
  bpb.sectors_per_cluster = kSectorsPerCluster;
  bpb.lcn_mft = kMftLcn;
  bpb.clusters_per_file_record = 1;
  bpb.clusters_per_index_block = 1;
  bpb.x_aa = kBootSignatureLow;
  bpb.x_55 = kBootSignatureHigh;

  const DWORD mftAddr = static_cast<DWORD>(kMftLcn) * kClusterSize;
  const size_t recordsEnd =
      mftAddr + static_cast<size_t>(kFakeFileRecordSize) *
                    (static_cast<size_t>(MftIdx::ROOT) + 1);
  // FULL_CACHE always reads a 64 KiB block regardless of length requested.
  constexpr size_t kFullCacheReadBlockSize = size_t{64} * 1024;
  const size_t imageSize = std::max(recordsEnd, kFullCacheReadBlockSize);
  std::vector<BYTE> image(imageSize, 0);
  std::memcpy(image.data(), &bpb, sizeof(bpb));

  const auto putRecord = [&](MftIdx idx, const FakeRecord& record)
  {
    const size_t offset = mftAddr + static_cast<size_t>(kFakeFileRecordSize) *
                                        static_cast<size_t>(idx);
    std::memcpy(&image.at(offset), record.data(), record.size());
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
  std::memcpy(&image.at(offset), record.data(), record.size());

  return image;
}

std::vector<BYTE> BuildFakeNtfsImageWithEmptyVolumeInformation()
{
  std::vector<BYTE> image = BuildFakeNtfsImage();

  const DWORD mftAddr = static_cast<DWORD>(kMftLcn) * kClusterSize;
  const size_t offset = mftAddr + static_cast<size_t>(kFakeFileRecordSize) *
                                      static_cast<size_t>(MftIdx::VOLUME);
  const FakeRecord record = MakeVolumeRecordSized(0);
  std::memcpy(&image.at(offset), record.data(), record.size());

  return image;
}

std::vector<BYTE> BuildFakeNtfsImageWithVolumeName()
{
  std::vector<BYTE> image = BuildFakeNtfsImage();

  const DWORD mftAddr = static_cast<DWORD>(kMftLcn) * kClusterSize;
  const size_t offset = mftAddr + static_cast<size_t>(kFakeFileRecordSize) *
                                      static_cast<size_t>(MftIdx::VOLUME);
  const FakeRecord record = MakeVolumeRecordWithName(kFakeVolumeName);
  std::memcpy(&image.at(offset), record.data(), record.size());

  return image;
}

std::vector<BYTE> BuildFakeNtfsImageWithDeletedVolumeRecord()
{
  std::vector<BYTE> image = BuildFakeNtfsImage();

  const DWORD mftAddr = static_cast<DWORD>(kMftLcn) * kClusterSize;
  const size_t offset = mftAddr + static_cast<size_t>(kFakeFileRecordSize) *
                                      static_cast<size_t>(MftIdx::VOLUME);

  // Same $VOLUME_INFORMATION content as BuildFakeNtfsImage(), but with the
  // INUSE flag cleared - a freed record. bypass_deleted_gate_ must still let
  // NtfsVolume::Init() read it.
  FakeRecord record = MakeVolumeRecord();
  EditFileRecordHeader(record, [](FileRecordHeader::Data& header)
                       { header.flags = NtfsBrowser::Flag::FileRecord{}; });

  std::memcpy(&image.at(offset), record.data(), record.size());

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
  std::memcpy(&image.at(offset), record.data(), record.size());

  return image;
}

std::vector<BYTE> BuildFakeNtfsImageWithEmptyStandardInformation()
{
  std::vector<BYTE> image = BuildFakeNtfsImage();

  const DWORD mftAddr = static_cast<DWORD>(kMftLcn) * kClusterSize;
  const size_t offset =
      mftAddr + static_cast<size_t>(kFakeFileRecordSize) *
                    static_cast<size_t>(kLegacyStandardInformationRecordIdx);
  const FakeRecord record = MakeStandardInformationRecordSized(0);
  std::memcpy(&image.at(offset), record.data(), record.size());

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
  std::memcpy(&image.at(offset), record.data(), record.size());

  return image;
}

std::vector<BYTE> BuildFakeNtfsImageWithAttributeListDirectory()
{
  std::vector<BYTE> image = BuildFakeNtfsImage();

  const DWORD mftAddr = static_cast<DWORD>(kMftLcn) * kClusterSize;
  const auto putRecord = [&](ULONGLONG idx, const FakeRecord& record)
  {
    const size_t offset = mftAddr + static_cast<size_t>(kFakeFileRecordSize) *
                                        gsl::narrow<size_t>(idx);
    std::memcpy(&image.at(offset), record.data(), record.size());
  };

  putRecord(kAttributeListDirIdx, MakeAttributeListOnlyDirRecord());
  putRecord(kIndexExtensionIdx,
            MakeIndexRootExtensionRecord(kAttributeListDirIdx));

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
  std::memcpy(&image.at(offset), record.data(), record.size());

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
  std::memcpy(&image.at(dirOffset), dirRecord.data(), dirRecord.size());

  const size_t blockOffset =
      static_cast<size_t>(kForgedIndexBlockLcn) * kClusterSize;
  if (image.size() < blockOffset + kForgedIndexBlockSize)
  {
    image.resize(blockOffset + kForgedIndexBlockSize, 0);
  }

  auto& block =
      *reinterpret_cast<NtfsBrowser::Data::IndexBlock*>(&image.at(blockOffset));
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
                                        gsl::narrow<size_t>(idx);
    std::memcpy(&image.at(offset), record.data(), record.size());
  };

  putRecord(kAttrListMultiTypeDirIdx, MakeAttributeListTwoTypesDirRecord());
  putRecord(kMultiTypeExtensionIdx,
            MakeIndexRootAndAllocExtensionRecord(kAttrListMultiTypeDirIdx));

  return image;
}

std::vector<BYTE> BuildFakeNtfsImageWithFragmentedAttributeListDirectory()
{
  std::vector<BYTE> image = BuildFakeNtfsImage();

  const DWORD mftAddr = static_cast<DWORD>(kMftLcn) * kClusterSize;
  const auto putRecord = [&](ULONGLONG idx, const FakeRecord& record)
  {
    const size_t offset = mftAddr + static_cast<size_t>(kFakeFileRecordSize) *
                                        gsl::narrow<size_t>(idx);
    std::memcpy(&image.at(offset), record.data(), record.size());
  };

  putRecord(kUafAttrListDirIdx, MakeFragmentedAttributeListDirRecord());
  putRecord(kUafExtensionIdx0,
            MakeIndexAllocationOnlyExtensionRecord(kUafRealSizeSentinels.at(0),
                                                   kUafAttrListDirIdx));
  putRecord(kUafExtensionIdx1,
            MakeIndexAllocationOnlyExtensionRecord(kUafRealSizeSentinels.at(1),
                                                   kUafAttrListDirIdx));
  putRecord(kUafExtensionIdx2,
            MakeIndexAllocationOnlyExtensionRecord(kUafRealSizeSentinels.at(2),
                                                   kUafAttrListDirIdx));
  putRecord(kUafExtensionIdx3,
            MakeIndexAllocationOnlyExtensionRecord(kUafRealSizeSentinels.at(3),
                                                   kUafAttrListDirIdx));

  return image;
}

std::vector<BYTE> BuildFakeNtfsImageWithCorruptMftRecord()
{
  std::vector<BYTE> image = BuildFakeNtfsImage();

  // Zeroing the magic makes ParseFileRecord() fail on this record alone.
  const DWORD mftAddr = static_cast<DWORD>(kMftLcn) * kClusterSize;
  const size_t offset = mftAddr + static_cast<size_t>(kFakeFileRecordSize) *
                                      static_cast<size_t>(MftIdx::MFT);
  std::memset(&image.at(offset), 0, kFakeFileRecordSize);

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
  std::memcpy(&image.at(offset), record.data(), record.size());

  return image;
}

std::vector<BYTE> BuildFakeNtfsImageWithAttrOffsetOutOfBounds()
{
  std::vector<BYTE> image = BuildFakeNtfsImage();

  // offset_of_attr is per-record, unlike the BPB fields patched above.
  const DWORD mftAddr = static_cast<DWORD>(kMftLcn) * kClusterSize;
  const size_t rootOffset = mftAddr + static_cast<size_t>(kFakeFileRecordSize) *
                                          static_cast<size_t>(MftIdx::ROOT);
  EditFileRecordHeader(
      std::span(image).subspan(rootOffset, kFakeFileRecordSize),
      [](FileRecordHeader::Data& header)
      { header.offset_of_attr = kAttrOffsetOutOfBounds; });

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
  std::memcpy(&image.at(rootOffset), record.data(), record.size());

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
  std::memcpy(&image.at(rootOffset), record.data(), record.size());

  return image;
}

std::vector<BYTE> BuildFakeNtfsImageWithAttributeListRecordSizeTooSmall()
{
  std::vector<BYTE> image = BuildFakeNtfsImage();

  const DWORD mftAddr = static_cast<DWORD>(kMftLcn) * kClusterSize;
  const auto putRecord = [&](ULONGLONG idx, const FakeRecord& record)
  {
    const size_t offset = mftAddr + static_cast<size_t>(kFakeFileRecordSize) *
                                        gsl::narrow<size_t>(idx);
    std::memcpy(&image.at(offset), record.data(), record.size());
  };

  putRecord(kAttributeListDirIdx,
            MakeAttributeListRecordSizeTooSmallDirRecord());
  putRecord(kIndexExtensionIdx,
            MakeIndexRootExtensionRecord(kAttributeListDirIdx));

  return image;
}

std::vector<BYTE> BuildFakeNtfsImageWithAttributeListOffsetMismatch()
{
  std::vector<BYTE> image = BuildFakeNtfsImage();

  const DWORD mftAddr = static_cast<DWORD>(kMftLcn) * kClusterSize;
  const auto putRecord = [&](ULONGLONG idx, const FakeRecord& record)
  {
    const size_t offset = mftAddr + static_cast<size_t>(kFakeFileRecordSize) *
                                        gsl::narrow<size_t>(idx);
    std::memcpy(&image.at(offset), record.data(), record.size());
  };

  putRecord(kAttributeListDirIdx, MakeAttributeListOffsetMismatchDirRecord());
  putRecord(kIndexExtensionIdx,
            MakeIndexRootExtensionRecord(kAttributeListDirIdx));

  return image;
}

std::vector<BYTE> BuildFakeNtfsImageWithAttributeListCycle()
{
  std::vector<BYTE> image = BuildFakeNtfsImage();

  const DWORD mftAddr = static_cast<DWORD>(kMftLcn) * kClusterSize;
  const auto putRecord = [&](ULONGLONG idx, const FakeRecord& record)
  {
    const size_t offset = mftAddr + static_cast<size_t>(kFakeFileRecordSize) *
                                        gsl::narrow<size_t>(idx);
    std::memcpy(&image.at(offset), record.data(), record.size());
  };

  putRecord(static_cast<ULONGLONG>(MftIdx::ROOT),
            MakeAttributeListCycleRecord(kAttrListCycleExtIdx));
  FakeRecord ext =
      MakeAttributeListCycleRecord(static_cast<ULONGLONG>(MftIdx::ROOT));
  SetRecordLink(ext, 0, static_cast<ULONGLONG>(MftIdx::ROOT));
  putRecord(kAttrListCycleExtIdx, ext);

  return image;
}

std::vector<BYTE> BuildFakeNtfsImageWithTightlyPackedAttributeListDirectory()
{
  std::vector<BYTE> image = BuildFakeNtfsImage();

  const DWORD mftAddr = static_cast<DWORD>(kMftLcn) * kClusterSize;
  const auto putRecord = [&](ULONGLONG idx, const FakeRecord& record)
  {
    const size_t offset = mftAddr + static_cast<size_t>(kFakeFileRecordSize) *
                                        gsl::narrow<size_t>(idx);
    std::memcpy(&image.at(offset), record.data(), record.size());
  };

  putRecord(kAttrListTightPackDirIdx,
            MakeAttributeListTightlyPackedDirRecord());
  putRecord(kAttrListTightPackExtIdxA,
            MakeIndexRootExtensionRecord(kAttrListTightPackDirIdx));
  putRecord(kAttrListTightPackExtIdxB,
            MakeIndexAllocationOnlyExtensionRecord(kAttrListTightPackRealSize,
                                                   kAttrListTightPackDirIdx));

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
  std::memset(&image.at(rootOffset), 0, kFakeFileRecordSize);

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
  std::memcpy(&image.at(mftAddr), mftRecord.data(), mftRecord.size());

  // Written at the physical cluster the data run maps this record's VCN to.
  const size_t forgedOffset = (static_cast<size_t>(kFragmentedMftDataRunLcn) +
                               kFragmentedMftInvalidRecordIdx) *
                              kClusterSize;
  if (image.size() < forgedOffset + kFakeFileRecordSize)
  {
    image.resize(forgedOffset + kFakeFileRecordSize, 0);
  }
  const FakeRecord forgedRecord = MakeInvalidOffsetOfUsRecord();
  std::memcpy(&image.at(forgedOffset), forgedRecord.data(),
              forgedRecord.size());

  return image;
}

std::vector<BYTE> BuildFakeNtfsImageWithMftDataSplitAcrossAttributeList()
{
  std::vector<BYTE> image = BuildFakeNtfsImage();
  const DWORD mftAddr = static_cast<DWORD>(kMftLcn) * kClusterSize;

  // Overwrites $MFT's own record: a $ATTRIBUTE_LIST relocating its DATA
  // continuation (starting at kMftDataSplitTargetIdx) to the extension
  // record below.
  const std::array<std::pair<ULONGLONG, ULONGLONG>, 1> continuations{
      {{kMftDataSplitExtIdx, kMftDataSplitTargetIdx}}};
  PutMftRecord(image, mftAddr, static_cast<ULONGLONG>(MftIdx::MFT),
               MakeMftRecordWithDataContinuations(continuations));

  // Extension record: the continuation instance itself, one cluster
  // starting at VCN kMftDataSplitTargetIdx, mapped to kMftDataSplitLcn.
  PutMftRecord(image, mftAddr, kMftDataSplitExtIdx,
               MakeMftDataContinuationExtensionRecord(kMftDataSplitTargetIdx,
                                                      kMftDataSplitLcn, 1));

  // The target file record itself, at the physical LCN the continuation
  // instance's data run maps its VCN to - only reachable by consulting that
  // instance, since kMftDataSplitTargetIdx is past Enum::MftIdx::USER.
  FakeRecord targetRecord =
      MakeRecordHeader(kAttrOffset, NtfsBrowser::Flag::FileRecord::INUSE);
  WriteEndOfAttributesMarker(targetRecord, kAttrOffset);
  PutRecordAt(image, static_cast<size_t>(kMftDataSplitLcn) * kClusterSize,
              targetRecord);

  return image;
}

std::vector<BYTE> BuildFakeNtfsImageWithMftDataExtentChain()
{
  std::vector<BYTE> image = BuildFakeNtfsImage();
  const DWORD mftAddr = static_cast<DWORD>(kMftLcn) * kClusterSize;

  // Entry 1 names kMftChainExtA, reachable only through entry 2's extent.
  const std::array<std::pair<ULONGLONG, ULONGLONG>, 2> continuations{
      {{kMftChainExtA, kMftChainExtAStartVcn},
       {kMftChainExtB, kMftChainExtBStartVcn}}};
  PutMftRecord(image, mftAddr, static_cast<ULONGLONG>(MftIdx::MFT),
               MakeMftRecordWithDataContinuations(continuations));

  // Ext B: naively reachable (below Enum::MftIdx::USER). Its own extent
  // covers kMftChainExtA's real location.
  PutMftRecord(image, mftAddr, kMftChainExtB,
               MakeMftDataContinuationExtensionRecord(kMftChainExtBStartVcn,
                                                      kMftChainExtBLcn,
                                                      kMftChainExtBClusters));

  // Ext A's real bytes, mapped through ext B's extent; naive slot stays zero.
  const size_t extAOffset = (static_cast<size_t>(kMftChainExtBLcn) +
                             (kMftChainExtA - kMftChainExtBStartVcn)) *
                            kClusterSize;
  PutRecordAt(image, extAOffset,
              MakeMftDataContinuationExtensionRecord(kMftChainExtAStartVcn,
                                                     kMftChainTargetLcn, 1));

  // The target file record, only reachable once both hops resolve.
  FakeRecord targetRecord =
      MakeRecordHeader(kAttrOffset, NtfsBrowser::Flag::FileRecord::INUSE);
  WriteEndOfAttributesMarker(targetRecord, kAttrOffset);
  PutRecordAt(image, static_cast<size_t>(kMftChainTargetLcn) * kClusterSize,
              targetRecord);

  return image;
}

std::vector<BYTE> BuildFakeNtfsImageWithUnresolvableMftDataExtent()
{
  std::vector<BYTE> image = BuildFakeNtfsImage();
  const DWORD mftAddr = static_cast<DWORD>(kMftLcn) * kClusterSize;

  // Entry 1 is permanently unresolvable; entry 2 resolves fine.
  const std::array<std::pair<ULONGLONG, ULONGLONG>, 2> continuations{
      {{kMftUnresolvableExtIdx, kMftUnresolvableStartVcn},
       {kMftUnresolvableGoodExtIdx, kMftUnresolvableGoodStartVcn}}};
  PutMftRecord(image, mftAddr, static_cast<ULONGLONG>(MftIdx::MFT),
               MakeMftRecordWithDataContinuations(continuations));

  // The good entry: naively reachable, its extent covers the good record.
  PutMftRecord(image, mftAddr, kMftUnresolvableGoodExtIdx,
               MakeMftDataContinuationExtensionRecord(
                   kMftUnresolvableGoodStartVcn, kMftUnresolvableGoodLcn,
                   kMftUnresolvableGoodClusters));
  FakeRecord goodRecord =
      MakeRecordHeader(kAttrOffset, NtfsBrowser::Flag::FileRecord::INUSE);
  WriteEndOfAttributesMarker(goodRecord, kAttrOffset);
  PutRecordAt(image,
              (static_cast<size_t>(kMftUnresolvableGoodLcn) +
               (kMftUnresolvableGoodRecord - kMftUnresolvableGoodStartVcn)) *
                  kClusterSize,
              goodRecord);

  // kMftUnresolvableExtIdx's slot stays zero-filled and unmapped.

  return image;
}

std::vector<BYTE> BuildFakeNtfsImageWithMftDataLastVcnOverflow()
{
  std::vector<BYTE> image = BuildFakeNtfsImage();
  const DWORD mftAddr = static_cast<DWORD>(kMftLcn) * kClusterSize;

  const std::array<std::pair<ULONGLONG, ULONGLONG>, 1> continuations{
      {{kMftLastVcnOverflowTargetIdx, 0}}};
  PutMftRecord(image, mftAddr, static_cast<ULONGLONG>(MftIdx::MFT),
               MakeMftRecordWithDataContinuations(continuations,
                                                  kMftLastVcnOverflowLastVcn));

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
  std::memcpy(&image.at(rootOffset), record.data(), record.size());

  return image;
}

std::vector<BYTE> BuildFakeNtfsImageWithIndexRootVariants()
{
  std::vector<BYTE> image = BuildFakeNtfsImage();

  const DWORD mftAddr = static_cast<DWORD>(kMftLcn) * kClusterSize;
  const auto putRecord = [&](ULONGLONG idx, const FakeRecord& record)
  {
    const size_t offset = mftAddr + static_cast<size_t>(kFakeFileRecordSize) *
                                        gsl::narrow<size_t>(idx);
    std::memcpy(&image.at(offset), record.data(), record.size());
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
  std::memcpy(&image.at(rootOffset), record.data(), record.size());

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
  std::memcpy(&image.at(rootOffset), record.data(), record.size());

  // The sub-node itself: one real index block holding
  // kGapCollationSearchName as its only leaf entry.
  const size_t blockOffset =
      static_cast<size_t>(kGapCollationIndexBlockLcn) * kClusterSize;
  if (image.size() < blockOffset + kClusterSize)
  {
    image.resize(blockOffset + kClusterSize, 0);
  }

  const std::span<BYTE> blockStart =
      std::span<BYTE>(image).subspan(blockOffset);
  auto& block =
      *reinterpret_cast<NtfsBrowser::Data::IndexBlock*>(blockStart.data());
  std::memset(&block, 0, sizeof(block));
  block.magic = kIndexBlockMagic;
  // Points at the block's own last 6 bytes, so PatchUS() succeeds
  // trivially without a real fixup array.
  block.offset_of_us = static_cast<WORD>(kClusterSize - kUsSlotSize);
  block.size_of_us = 3;
  block.vcn = 0;
  block.entry_offset = gsl::narrow<DWORD>(
      (&gsl::at(blockStart, sizeof(NtfsBrowser::Data::IndexBlock))) -
      reinterpret_cast<BYTE*>(&block.entry_offset));
  block.not_leaf = 0;

  const std::span<BYTE> body =
      blockStart.subspan(sizeof(NtfsBrowser::Data::IndexBlock));

  // Entry 1: the real leaf entry, a plain leaf with no sub-node.
  auto& first_entry =
      *reinterpret_cast<NtfsBrowser::Data::IndexEntry*>(body.data());
  first_entry.mft_index = kGapCollationLeafMftRef;
  first_entry.mft_sn = 1;

  auto& fn1 =
      *reinterpret_cast<NtfsBrowser::Attr::Filename*>(&first_entry.stream);
  fn1.parent_ref = static_cast<ULONGLONG>(MftIdx::ROOT);
  fn1.flags = NtfsBrowser::Flag::Filename::NONE;
  fn1.name_length = kGapCollationSearchNameLength;
  fn1.name_space = NtfsBrowser::Flag::FilenameNamespace::WIN_32;
  for (BYTE i = 0; i < fn1.name_length; i++)
  {
    fn1.name[i] = gsl::narrow<WORD>(kGapCollationSearchName[i]);
  }

  first_entry.stream_size =
      gsl::narrow<WORD>(reinterpret_cast<BYTE*>(&fn1.name[fn1.name_length]) -
                        reinterpret_cast<BYTE*>(&fn1));
  first_entry.size = gsl::narrow<WORD>(AlignAttrSize(
      &first_entry.stream - reinterpret_cast<BYTE*>(&first_entry) +
      first_entry.stream_size));

  // Entry 2: the terminating entry - no name, no sub-node.
  auto& second_entry = *reinterpret_cast<NtfsBrowser::Data::IndexEntry*>(
      &gsl::at(body, first_entry.size));
  second_entry.flags = NtfsBrowser::Flag::IndexEntry::LAST;
  second_entry.stream_size = 0;
  second_entry.size = gsl::narrow<WORD>(AlignAttrSize(
      &second_entry.stream - reinterpret_cast<BYTE*>(&second_entry)));

  block.total_entry_size =
      static_cast<DWORD>(first_entry.size) + second_entry.size;
  block.alloc_entry_size = block.total_entry_size;

  return image;
}

namespace
{

// UTF-16 units in $UpCase: one entry per code unit of the BMP.
constexpr size_t kUpCaseUnitCount = 65536;

// LCN of the $INDEX_ALLOCATION block the non-ASCII fixture files its names in.
constexpr DWORD kNonAsciiIndexBlockLcn = 20;

// LCN of $UpCase's data in the non-ASCII fixture, past every record above.
constexpr DWORD kNonAsciiUpCaseLcn = 64;

// The 128 KiB $UpCase image the non-ASCII fixture stores: the identity map,
// but for a-z, the Latin-1 letters and y-diaeresis. The dotless i stays
// unmapped, as it does in a Windows table. Independent of the library.
std::vector<BYTE> MakeNonAsciiUpCaseBytes()
{
  constexpr WORD kLatin1LowerFirst = 0x00E0;
  constexpr WORD kLatin1LowerLast = 0x00FE;
  constexpr WORD kDivisionSign = 0x00F7;
  constexpr WORD kYDiaeresis = 0x00FF;
  constexpr WORD kCapitalYDiaeresis = 0x0178;
  constexpr WORD kCaseDistance = 0x20;

  std::vector<BYTE> bytes(kUpCaseUnitCount * sizeof(WORD));
  for (size_t unit = 0; unit < kUpCaseUnitCount; unit++)
  {
    auto upper = gsl::narrow<WORD>(unit);
    if (unit >= L'a' && unit <= L'z')
    {
      upper = gsl::narrow<WORD>(unit - kCaseDistance);
    }
    else if (unit >= kLatin1LowerFirst && unit <= kLatin1LowerLast &&
             unit != kDivisionSign)
    {
      upper = gsl::narrow<WORD>(unit - kCaseDistance);
    }
    else if (unit == kYDiaeresis)
    {
      upper = kCapitalYDiaeresis;
    }
    bytes.at(unit * 2) = static_cast<BYTE>(upper & kByteMask);
    bytes.at(unit * 2 + 1) = static_cast<BYTE>(upper >> kBitsPerByte);
  }
  return bytes;
}

}  // namespace

std::vector<BYTE> BuildFakeNtfsImageWithNonAsciiNames(NonAsciiNameLayout layout,
                                                      bool withUpCase)
{
  // Sorted by uppercase: E-acute (C9), O-diaeresis (D6), dotless i (131).
  const std::array<FakeIndexName, 3> names{
      {{kNonAsciiAcuteName, kNonAsciiAcuteMftRef, false},
       {kNonAsciiDiaeresisName, kNonAsciiDiaeresisMftRef, false},
       {kNonAsciiDotlessName, kNonAsciiDotlessMftRef, false}}};

  std::vector<BYTE> image = BuildFakeNtfsImage();

  const DWORD mftAddr = static_cast<DWORD>(kMftLcn) * kClusterSize;
  const auto putRecord = [&](MftIdx idx, const FakeRecord& record)
  {
    const size_t offset = mftAddr + static_cast<size_t>(kFakeFileRecordSize) *
                                        static_cast<size_t>(idx);
    std::memcpy(&image.at(offset), record.data(), record.size());
  };

  if (layout == NonAsciiNameLayout::kIndexRoot)
  {
    putRecord(MftIdx::ROOT, MakeIndexRootDirRecordWithNames(names));
  }
  else
  {
    const std::vector<FakeDataRun> runs{{kNonAsciiIndexBlockLcn, 1}};
    putRecord(MftIdx::ROOT, MakeIndexAllocationDirRecord(
                                NtfsBrowser::Flag::StdInfoPermission::ARCHIVE,
                                0, kFakeFileRecordSize, runs));
    LayRunBytes(image, runs, MakeIndexBlockContent(names));
  }

  if (withUpCase)
  {
    // $MFT now maps up to record 10, so $UpCase can be read through it.
    putRecord(MftIdx::MFT, MakeMftRecordWithRealDataRun(
                               static_cast<DWORD>(kMftLcn),
                               static_cast<DWORD>(MftIdx::UPCASE) + 1));

    const std::vector<BYTE> table = MakeNonAsciiUpCaseBytes();
    const std::vector<FakeDataRun> runs{
        {kNonAsciiUpCaseLcn,
         gsl::narrow<DWORD>(table.size() / kFakeClusterSize)}};

    FakeRecord upcase =
        MakeRecordHeader(kAttrOffset, NtfsBrowser::Flag::FileRecord::INUSE);
    const DWORD attrSize = WriteNonResidentAttr(
        upcase, kAttrOffset, AttrType::DATA, 0, table.size(), runs);
    WriteEndOfAttributesMarker(upcase, kAttrOffset + attrSize);
    putRecord(MftIdx::UPCASE, upcase);

    LayRunBytes(image, runs, table);
  }

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
  std::memcpy(&image.at(rootOffset), record.data(), record.size());

  // The chain itself: kIndexBlockChainLength contiguous blocks starting at
  // kIndexBlockChainLcn, one cluster each.
  const size_t chainOffset =
      static_cast<size_t>(kIndexBlockChainLcn) * kClusterSize;
  const size_t chainBytes =
      static_cast<size_t>(kIndexBlockChainLength) * kClusterSize;
  // FULL_CACHE always reads a whole 64KiB-aligned block, so the image must
  // extend past the chain's real end or its last read fails outright.
  constexpr size_t kFullCacheReadBlockSize = size_t{64} * 1024;
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
    const std::span<BYTE> blockStart = std::span<BYTE>(image).subspan(
        chainOffset + static_cast<size_t>(vcn) * kClusterSize);
    auto& block =
        *reinterpret_cast<NtfsBrowser::Data::IndexBlock*>(blockStart.data());
    std::memset(&block, 0, sizeof(block));
    block.magic = kIndexBlockMagic;
    // Points offset_of_us at the fixup slot itself, valid for every block.
    block.offset_of_us = static_cast<WORD>(kClusterSize - kUsSlotSize);
    block.size_of_us = 3;
    block.vcn = vcn;
    block.entry_offset = gsl::narrow<DWORD>(
        (&gsl::at(blockStart, sizeof(NtfsBrowser::Data::IndexBlock))) -
        reinterpret_cast<BYTE*>(&block.entry_offset));

    const std::span<BYTE> body =
        blockStart.subspan(sizeof(NtfsBrowser::Data::IndexBlock));
    auto& first_entry =
        *reinterpret_cast<NtfsBrowser::Data::IndexEntry*>(body.data());

    const bool isLeaf = (vcn == kIndexBlockChainLength - 1);
    if (!isLeaf)
    {
      // Intermediate block: a lone, nameless entry pointing at the next VCN.
      block.not_leaf = 1;
      first_entry.mft_index = 0;
      first_entry.mft_sn = 0;
      first_entry.stream_size = 0;
      first_entry.flags = NtfsBrowser::Flag::IndexEntry::SUBNODE |
                          NtfsBrowser::Flag::IndexEntry::LAST;
      first_entry.size = static_cast<WORD>(AlignAttrSize(
          offsetof(NtfsBrowser::Data::IndexEntry, stream) + sizeof(ULONGLONG)));
      auto& subNodeVcn = SubNodeVcnSlot(first_entry);
      subNodeVcn = vcn + 1;
    }
    else
    {
      // Deepest block: the real, named leaf entry, reached by depth alone.
      block.not_leaf = 0;
      first_entry.mft_index = kIndexBlockChainLeafMftRef;
      first_entry.mft_sn = 1;

      auto& fn1 =
          *reinterpret_cast<NtfsBrowser::Attr::Filename*>(&first_entry.stream);
      fn1.parent_ref = static_cast<ULONGLONG>(MftIdx::ROOT);
      fn1.flags = NtfsBrowser::Flag::Filename::NONE;
      fn1.name_length = kIndexBlockChainLeafNameLength;
      fn1.name_space = NtfsBrowser::Flag::FilenameNamespace::WIN_32;
      for (BYTE i = 0; i < fn1.name_length; i++)
      {
        fn1.name[i] = gsl::narrow<WORD>(kIndexBlockChainLeafName[i]);
      }

      first_entry.stream_size = gsl::narrow<WORD>(
          reinterpret_cast<BYTE*>(&fn1.name[fn1.name_length]) -
          reinterpret_cast<BYTE*>(&fn1));
      first_entry.flags = NtfsBrowser::Flag::IndexEntry::LAST;
      first_entry.size = gsl::narrow<WORD>(AlignAttrSize(
          &first_entry.stream - reinterpret_cast<BYTE*>(&first_entry) +
          first_entry.stream_size));
    }

    block.total_entry_size = first_entry.size;
    block.alloc_entry_size = first_entry.size;
  }

  return image;
}

std::vector<BYTE> BuildFakeNtfsImageWithOrphanedIndexBlocks()
{
  std::vector<BYTE> image = BuildFakeNtfsImage();

  // Replace the root directory's (#5) whole record in place.
  const DWORD mftAddr = static_cast<DWORD>(kMftLcn) * kClusterSize;
  const size_t rootOffset = mftAddr + static_cast<size_t>(kFakeFileRecordSize) *
                                          static_cast<size_t>(MftIdx::ROOT);
  const FakeRecord record = MakeOrphanedIndexBlocksRootRecord();
  std::memcpy(&image.at(rootOffset), record.data(), record.size());

  const size_t blocksOffset =
      static_cast<size_t>(kOrphanedBlocksLcn) * kClusterSize;
  const size_t blocksBytes =
      static_cast<size_t>(kOrphanedBlocksCount) * kClusterSize;
  // FULL_CACHE always reads a whole 64KiB-aligned block, so the image must
  // extend past the blocks' real end or its last read fails outright.
  constexpr size_t kFullCacheReadBlockSize = size_t{64} * 1024;
  const size_t blocksEnd = blocksOffset + blocksBytes;
  const size_t alignedBlocksEnd =
      ((blocksEnd + kFullCacheReadBlockSize - 1) / kFullCacheReadBlockSize) *
      kFullCacheReadBlockSize;
  if (image.size() < alignedBlocksEnd)
  {
    image.resize(alignedBlocksEnd, 0);
  }

  const ULONGLONG rootRef = MakeFileReference(
      static_cast<ULONGLONG>(MftIdx::ROOT), kRootSequenceNumber);

  // VCN 0: reachable through $INDEX_ROOT's own sub-node pointer.
  WriteOrphanedIndexLeafBlock(image, 0, kOrphanedBlockReachableMftRef, rootRef,
                              kOrphanedBlockReachableName);
  // VCN 1: orphaned, but still filed under this directory.
  WriteOrphanedIndexLeafBlock(image, 1, kOrphanedBlockOrphanMftRef, rootRef,
                              kOrphanedBlockOrphanName);
  // VCN 2: orphaned, and filed under a different parent - a recovery scan
  // must find the block but reject the entry.
  WriteOrphanedIndexLeafBlock(
      image, 2, kOrphanedBlockStaleMftRef,
      MakeFileReference(kOrphanedBlockStaleParentRef, kRootSequenceNumber),
      kOrphanedBlockStaleName);

  return image;
}

std::vector<BYTE> BuildFakeNtfsImageWithHugeOrphanScanBlockCount()
{
  std::vector<BYTE> image = BuildFakeNtfsImageWithOrphanedIndexBlocks();

  // Replace the root directory's (#5) $INDEX_ALLOCATION real_size only: the
  // three real blocks (and $INDEX_ROOT's own pointer to VCN 0) stay exactly
  // as BuildFakeNtfsImageWithOrphanedIndexBlocks() wrote them.
  const DWORD mftAddr = static_cast<DWORD>(kMftLcn) * kClusterSize;
  const size_t rootOffset = mftAddr + static_cast<size_t>(kFakeFileRecordSize) *
                                          static_cast<size_t>(MftIdx::ROOT);
  const FakeRecord record =
      MakeOrphanedIndexBlocksRootRecord(kHugeOrphanScanDeclaredBlockCount);
  std::memcpy(&image.at(rootOffset), record.data(), record.size());

  return image;
}

std::vector<BYTE> BuildFakeNtfsImageWithOrphanedIndexBlockSequenceMismatch()
{
  std::vector<BYTE> image = BuildFakeNtfsImageWithOrphanedIndexBlocks();

  // Give kOrphanedBlockSequenceMismatchTargetIdx a real, in-use record on
  // disk, under kOrphanedBlockSequenceMismatchRecordSeq.
  const FakeRecord record =
      MakeMftTreeRecord(NtfsBrowser::Flag::FileRecord::INUSE,
                        kOrphanedBlockSequenceMismatchRecordSeq, {});
  PutMftRecord(image, static_cast<DWORD>(kMftLcn) * kClusterSize,
               kOrphanedBlockSequenceMismatchTargetIdx, record);

  // Redirect the "Orphan" entry (VCN 1) to name that record instead of
  // kOrphanedBlockOrphanMftRef, leaving its mft_sn exactly as
  // WriteOrphanedIndexLeafBlock() wrote it (1) - now a mismatch against the
  // record's own sequence number (kOrphanedBlockSequenceMismatchRecordSeq).
  const size_t vcn1Offset =
      (static_cast<size_t>(kOrphanedBlocksLcn) + 1) * kClusterSize;
  auto& entry = *reinterpret_cast<NtfsBrowser::Data::IndexEntry*>(
      &image.at(vcn1Offset + sizeof(NtfsBrowser::Data::IndexBlock)));
  entry.mft_index = kOrphanedBlockSequenceMismatchTargetIdx;

  return image;
}

std::vector<BYTE> BuildFakeNtfsImageWithMultiClusterOrphanedIndexBlock()
{
  std::vector<BYTE> image = BuildFakeNtfsImage();

  // Only this fixture's directory needs an index block bigger than 1
  // cluster - patch the shared BPB in place, the same trick
  // BuildFakeNtfsImageWithForgedIndexBlock() uses.
  auto& bpb = *reinterpret_cast<NtfsBrowser::Data::NtfsBpb*>(image.data());
  bpb.clusters_per_index_block =
      static_cast<DWORD>(kMultiClusterOrphanClustersPerBlock);

  const DWORD mftAddr = static_cast<DWORD>(kMftLcn) * kClusterSize;
  const size_t rootOffset = mftAddr + static_cast<size_t>(kFakeFileRecordSize) *
                                          static_cast<size_t>(MftIdx::ROOT);
  const FakeRecord record = MakeMultiClusterOrphanedIndexBlocksRootRecord();
  std::memcpy(&image.at(rootOffset), record.data(), record.size());

  const size_t blocksOffset =
      static_cast<size_t>(kMultiClusterOrphanLcn) * kClusterSize;
  const size_t blocksBytes =
      static_cast<size_t>(kMultiClusterOrphanBlockCount) *
      kMultiClusterOrphanIndexBlockSize;
  // FULL_CACHE always reads a whole 64KiB-aligned block, so the image must
  // extend past the blocks' real end or its last read fails outright.
  constexpr size_t kFullCacheReadBlockSize = size_t{64} * 1024;
  const size_t blocksEnd = blocksOffset + blocksBytes;
  const size_t alignedBlocksEnd =
      ((blocksEnd + kFullCacheReadBlockSize - 1) / kFullCacheReadBlockSize) *
      kFullCacheReadBlockSize;
  if (image.size() < alignedBlocksEnd)
  {
    image.resize(alignedBlocksEnd, 0);
  }

  const ULONGLONG rootRef = MakeFileReference(
      static_cast<ULONGLONG>(MftIdx::ROOT), kRootSequenceNumber);

  WriteMultiClusterIndexLeafBlock(image, 0, kMultiClusterReachableMftRef,
                                  rootRef, kMultiClusterReachableName);
  WriteMultiClusterIndexLeafBlock(image, 1, kMultiClusterOrphanMftRef, rootRef,
                                  kMultiClusterOrphanName);

  return image;
}

std::vector<BYTE> BuildFakeNtfsImageWithSubClusterOrphanedIndexBlocks()
{
  std::vector<BYTE> image = BuildFakeNtfsImage();

  // Patch the shared BPB: index blocks of 2^9 bytes, half a cluster.
  auto& bpb = *reinterpret_cast<NtfsBrowser::Data::NtfsBpb*>(image.data());
  bpb.clusters_per_index_block = kSubClusterIndexBlockEncoding;

  constexpr DWORD kIndexBlockSize = 512;
  constexpr DWORD kAllocLcn = 220;
  const std::array<FakeIndexAllocExtent, 1> extents{FakeIndexAllocExtent{
      .start_vcn = 0, .clusters = kSubClusterAllocClusters, .lcn = kAllocLcn}};
  const FakeRecord record = MakeDirectoryWithIndexAllocation(
      kIndexBlockSize, extents, kSubClusterBlockNames.size());
  const size_t rootOffset = static_cast<size_t>(kMftLcn) * kClusterSize +
                            static_cast<size_t>(kFakeFileRecordSize) *
                                static_cast<size_t>(MftIdx::ROOT);
  std::memcpy(&image.at(rootOffset), record.data(), record.size());

  const size_t blocksOffset = static_cast<size_t>(kAllocLcn) * kClusterSize;
  // FULL_CACHE always reads a whole 64KiB-aligned block, so the image must
  // extend past the blocks' real end or its last read fails outright.
  constexpr size_t kFullCacheReadBlockSize = size_t{64} * 1024;
  const size_t blocksEnd =
      blocksOffset + kSubClusterBlockNames.size() * kIndexBlockSize;
  const size_t alignedBlocksEnd =
      ((blocksEnd + kFullCacheReadBlockSize - 1) / kFullCacheReadBlockSize) *
      kFullCacheReadBlockSize;
  if (image.size() < alignedBlocksEnd)
  {
    image.resize(alignedBlocksEnd, 0);
  }

  const ULONGLONG rootRef = MakeFileReference(
      static_cast<ULONGLONG>(MftIdx::ROOT), kRootSequenceNumber);
  for (size_t i = 0; i < kSubClusterBlockNames.size(); i++)
  {
    WriteIndexLeafBlockAt(image, blocksOffset + i * kIndexBlockSize,
                          kIndexBlockSize, i, kSubClusterBlockRecordBase + i,
                          rootRef, kSubClusterBlockNames.at(i));
  }

  return image;
}

std::vector<BYTE> BuildFakeNtfsImageWithSplitIndexAllocation()
{
  std::vector<BYTE> image = BuildFakeNtfsImage();

  // Index blocks are one cluster each: block i is VCN i.
  constexpr DWORD kFirstLcn = 230;
  constexpr DWORD kSecondLcn = 240;
  const std::array<FakeIndexAllocExtent, 2> extents{
      FakeIndexAllocExtent{
          .start_vcn = 0, .clusters = kSplitExtentClusters, .lcn = kFirstLcn},
      FakeIndexAllocExtent{.start_vcn = kSplitExtentClusters,
                           .clusters = kSplitExtentClusters,
                           .lcn = kSecondLcn}};
  const FakeRecord record = MakeDirectoryWithIndexAllocation(
      kClusterSize, extents, kSplitBlockNames.size());
  const size_t rootOffset = static_cast<size_t>(kMftLcn) * kClusterSize +
                            static_cast<size_t>(kFakeFileRecordSize) *
                                static_cast<size_t>(MftIdx::ROOT);
  std::memcpy(&image.at(rootOffset), record.data(), record.size());

  // FULL_CACHE always reads a whole 64KiB-aligned block, so the image must
  // extend past the blocks' real end or its last read fails outright.
  constexpr size_t kFullCacheReadBlockSize = size_t{64} * 1024;
  const size_t blocksEnd =
      static_cast<size_t>(kSecondLcn + kSplitExtentClusters) * kClusterSize;
  const size_t alignedBlocksEnd =
      ((blocksEnd + kFullCacheReadBlockSize - 1) / kFullCacheReadBlockSize) *
      kFullCacheReadBlockSize;
  if (image.size() < alignedBlocksEnd)
  {
    image.resize(alignedBlocksEnd, 0);
  }

  const ULONGLONG rootRef = MakeFileReference(
      static_cast<ULONGLONG>(MftIdx::ROOT), kRootSequenceNumber);
  for (size_t i = 0; i < kSplitBlockNames.size(); i++)
  {
    const size_t lcn = (i < kSplitExtentClusters)
                           ? kFirstLcn + i
                           : kSecondLcn + (i - kSplitExtentClusters);
    WriteIndexLeafBlockAt(image, lcn * kClusterSize, kClusterSize, i,
                          kSplitBlockRecordBase + i, rootRef,
                          kSplitBlockNames.at(i));
  }

  return image;
}

std::vector<BYTE>
    BuildFakeNtfsImageWithOrphanedIndexBlockParentLink(FakeParentLink link)
{
  std::vector<BYTE> image = BuildFakeNtfsImageWithOrphanedIndexBlocks();

  const size_t rootOffset = static_cast<size_t>(kMftLcn) * kClusterSize +
                            static_cast<size_t>(kFakeFileRecordSize) *
                                static_cast<size_t>(MftIdx::ROOT);
  EditFileRecordHeader(
      std::span(image).subspan(rootOffset, kFakeFileRecordSize),
      [&](FileRecordHeader::Data& header)
      {
        header.seq_no = link.record_sequence;
        header.flags = link.record_in_use
                           ? (NtfsBrowser::Flag::FileRecord::INUSE |
                              NtfsBrowser::Flag::FileRecord::DIR)
                           : NtfsBrowser::Flag::FileRecord::DIR;
      });

  // VCN 1's entry names the directory unchecked (sequence 0), so it is
  // reported whatever generation the record is on.
  WriteOrphanedIndexLeafBlock(
      image, 1, kOrphanedBlockOrphanMftRef,
      MakeFileReference(static_cast<ULONGLONG>(MftIdx::ROOT), 0),
      kOrphanedBlockOrphanName);

  // Replace VCN 2's foreign-parent entry: same directory, this generation.
  WriteOrphanedIndexLeafBlock(
      image, 2, kOrphanedBlockGenerationMftRef,
      MakeFileReference(static_cast<ULONGLONG>(MftIdx::ROOT),
                        link.entry_parent_sequence),
      kOrphanedBlockGenerationName);

  return image;
}

std::vector<BYTE> BuildFakeNtfsImageWithMftTree()
{
  using NtfsBrowser::Flag::FilenameNamespace;
  using NtfsBrowser::Flag::StdInfoPermission;
  using RecordFlag = NtfsBrowser::Flag::FileRecord;

  // Sequence numbers the records below carry, where another record's
  // parent reference names them.
  constexpr WORD kDocsSequence = 1;
  constexpr WORD kOldDirSequence = 3;
  constexpr WORD kReportSequence = 3;
  constexpr WORD kNewDirSequence = 7;

  std::vector<BYTE> image = BuildFakeNtfsImage();

  const DWORD mftAddr = static_cast<DWORD>(kMftLcn) * kClusterSize;
  const auto putRecord = [&](ULONGLONG idx, const FakeRecord& record)
  {
    const size_t offset = mftAddr + static_cast<size_t>(kFakeFileRecordSize) *
                                        gsl::narrow<size_t>(idx);
    std::memcpy(&image.at(offset), record.data(), record.size());
  };

  const ULONGLONG root = MakeFileReference(static_cast<ULONGLONG>(MftIdx::ROOT),
                                           kRootSequenceNumber);
  const ULONGLONG docs = MakeFileReference(kMftTreeDocsIdx, kDocsSequence);

  // $MFT: a data run over every slot, written in place, so the records past
  // the first 16 read through it; plus the name real volumes give it.
  FakeRecord mft = MakeMftRecordWithRealDataRun(
      static_cast<DWORD>(kMftLcn), static_cast<DWORD>(kMftTreeRecordCount));
  EditFileRecordHeader(mft, [](FileRecordHeader::Data& header)
                       { header.seq_no = 1; });
  DWORD offset =
      kAttrOffset + reinterpret_cast<NtfsBrowser::Attr::HeaderNonResident*>(
                        &mft.at(kAttrOffset))
                        ->header.total_size;
  offset += WriteFileNameAttr(mft, offset,
                              {.name = L"$MFT", .parent_ref = root}, false);
  WriteEndOfAttributesMarker(mft, offset);
  putRecord(static_cast<ULONGLONG>(MftIdx::MFT), mft);

  const RecordFlag file = RecordFlag::INUSE;
  const RecordFlag dir = RecordFlag::INUSE | RecordFlag::DIR;
  const RecordFlag deletedFile{};
  const RecordFlag deletedDir = RecordFlag::DIR;

  putRecord(static_cast<ULONGLONG>(MftIdx::ROOT),
            MakeMftTreeRecord(dir, kRootSequenceNumber,
                              {{.name = L".", .parent_ref = root}}));
  putRecord(kMftTreeDocsIdx,
            MakeMftTreeRecord(dir, kDocsSequence,
                              {{.name = L"Docs", .parent_ref = root}}));
  putRecord(kMftTreeReportIdx,
            MakeMftTreeRecord(
                file, kReportSequence,
                {{.name = L"report.txt",
                  .parent_ref = docs,
                  .real_size = kMftTreeReportStaleSize,
                  .extra_flags = NtfsBrowser::Flag::Filename::READONLY |
                                 NtfsBrowser::Flag::Filename::ARCHIVE},
                 {.name = L"REPORT~1.TXT",
                  .parent_ref = docs,
                  .name_space = FilenameNamespace::DOS}},
                kMftTreeReportDataSize,
                StdInfoPermission::READONLY | StdInfoPermission::ARCHIVE));
  putRecord(kMftTreeHardLinkIdx,
            MakeMftTreeRecord(file, 1,
                              {{.name = L"link-a", .parent_ref = root},
                               {.name = L"link-b", .parent_ref = docs}}));
  // NTFS bumps a record's sequence number when it frees the record.
  putRecord(kMftTreeDeletedFileIdx,
            MakeMftTreeRecord(deletedFile, 2,
                              {{.name = L"old.tmp", .parent_ref = docs}}));
  putRecord(kMftTreeDeletedDirIdx,
            MakeMftTreeRecord(deletedDir, kOldDirSequence + 1,
                              {{.name = L"OldDir", .parent_ref = docs}}));
  putRecord(kMftTreeDeletedChildIdx,
            MakeMftTreeRecord(deletedFile, 2,
                              {{.name = L"draft.doc",
                                .parent_ref = MakeFileReference(
                                    kMftTreeDeletedDirIdx, kOldDirSequence)}}));
  putRecord(
      kMftTreeStaleChildIdx,
      MakeMftTreeRecord(deletedFile, 2,
                        {{.name = L"stale.txt",
                          .parent_ref = MakeFileReference(
                              kMftTreeReusedDirIdx, kNewDirSequence - 1)}}));
  putRecord(kMftTreeReusedDirIdx,
            MakeMftTreeRecord(dir, kNewDirSequence,
                              {{.name = L"NewDir", .parent_ref = root}}));
  putRecord(
      kMftTreeExtensionIdx,
      MakeMftTreeRecord(file, 1, {{.name = L"ext", .parent_ref = docs}}, {},
                        StdInfoPermission::NORMAL,
                        MakeFileReference(kMftTreeReportIdx, kReportSequence)));

  return image;
}

std::vector<BYTE> BuildFakeNtfsImageWithMftExtensionRecord()
{
  std::vector<BYTE> image = BuildFakeNtfsImageWithMftTree();

  // $MFT's sequence number, which BuildFakeNtfsImageWithMftTree() sets to 1.
  constexpr WORD kMftSequence = 1;
  const FakeRecord record = MakeMftTreeRecord(
      NtfsBrowser::Flag::FileRecord::INUSE, kMftSequence, {}, {},
      NtfsBrowser::Flag::StdInfoPermission::NORMAL,
      MakeFileReference(static_cast<ULONGLONG>(MftIdx::MFT), kMftSequence));

  const size_t offset = static_cast<size_t>(kMftLcn) * kClusterSize +
                        static_cast<size_t>(kFakeFileRecordSize) *
                            static_cast<size_t>(kMftTreeZeroedIdx);
  std::memcpy(&image.at(offset), record.data(), record.size());
  return image;
}

std::vector<BYTE> BuildFakeNtfsImageWithHugeMftRealSize()
{
  std::vector<BYTE> image = BuildFakeNtfsImageWithMftTree();

  const size_t mftOffset = static_cast<size_t>(kMftLcn) * kClusterSize;
  auto& attr = *reinterpret_cast<NtfsBrowser::Attr::HeaderNonResident*>(
      &image.at(mftOffset + kAttrOffset));
  attr.real_size = kHugeMftRealSize;
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
      gsl::narrow<WORD>(0x3000U | gsl::narrow<unsigned>(payload.size() - 1));

  std::vector<BYTE> chunk;
  chunk.reserve(payload.size() + 2);
  chunk.push_back(static_cast<BYTE>(header & kByteMask));
  chunk.push_back(static_cast<BYTE>(header >> kBitsPerByte));
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
    pattern.at(i) =
        static_cast<BYTE>((i * kPatternMul + kPatternAdd) % kPatternMod);
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

std::vector<BYTE> BuildFakeNtfsImageWithUninitializedTail()
{
  const std::vector<FakeDataRun> runs{
      {kCompressedDataLcn, kUninitializedTailClusters}};

  const FakeRecord record =
      MakeNonResidentDataRecord(NtfsBrowser::Flag::StdInfoPermission::ARCHIVE,
                                0, kUninitializedTailRealSize, runs,
                                {.ini_size = kUninitializedTailIniSize});

  return BuildCompressionImage(
      record, runs,
      CompressionFixturePattern(size_t{kUninitializedTailClusters} *
                                kClusterSize));
}

std::vector<BYTE> BuildFakeNtfsImageWithMultiClusterBitmap()
{
  const std::vector<FakeDataRun> runs{
      {kCompressedDataLcn, kMultiClusterBitmapClusters}};

  FakeRecord record =
      MakeRecordHeader(kAttrOffset, NtfsBrowser::Flag::FileRecord::INUSE);
  DWORD offset = kAttrOffset;
  offset += WriteStandardInformationAttr(
      record, offset, NtfsBrowser::Flag::StdInfoPermission::ARCHIVE);
  offset += WriteNonResidentAttr(
      record, offset, AttrType::BITMAP, 0,
      static_cast<ULONGLONG>(kMultiClusterBitmapClusters) * kClusterSize, runs);
  WriteEndOfAttributesMarker(record, offset);

  std::vector<BYTE> bitmap(
      static_cast<size_t>(kMultiClusterBitmapClusters) * kClusterSize, 0x00);
  std::fill_n(bitmap.begin(), kClusterSize, kAllBitsSet);
  bitmap.at(static_cast<size_t>(2) * kClusterSize) = 0x01;

  return BuildCompressionImage(record, runs, bitmap);
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

namespace
{

// Writes a malformed attribute at record[offset], as defect describes it.
void WriteTrailingDefect(FakeRecord& record, DWORD offset,
                         FakeTrailingDefect defect)
{
  // A resident header never fits in this: ParseAttrs() rejects it.
  constexpr DWORD kUndersizedTotalSize = 8;

  switch (defect)
  {
    case FakeTrailingDefect::UndersizedHeader:
    {
      auto& attr = *reinterpret_cast<NtfsBrowser::Attr::HeaderResident*>(
          &record.at(offset));
      attr.header.type = AttrType::BITMAP;
      attr.header.non_resident = 0;
      attr.header.total_size = kUndersizedTotalSize;
      break;
    }
    case FakeTrailingDefect::UndersizedCompressedField:
    {
      auto& attr = *reinterpret_cast<NtfsBrowser::Attr::HeaderNonResident*>(
          &record.at(offset));
      attr.header.type = AttrType::BITMAP;
      attr.header.non_resident = 1;
      attr.comp_unit_size = kCompressionUnitSizeShift;
      attr.header.total_size = NtfsBrowser::Attr::kHeaderNonResidentBaseSize;
      break;
    }
    case FakeTrailingDefect::RejectedAttribute:
    {
      auto& attr = *reinterpret_cast<NtfsBrowser::Attr::HeaderNonResident*>(
          &record.at(offset));
      attr.header.type = AttrType::STANDARD_INFORMATION;
      attr.header.non_resident = 1;
      attr.header.total_size = AlignAttrSize(
          NtfsBrowser::Attr::kHeaderNonResidentBaseSize + kRunListRoom);
      break;
    }
  }
}

}  // namespace

// Writes a resident $EFS attribute holding "body" and returns its total_size.
DWORD WriteResidentEfsAttr(FakeRecord& record, DWORD offset,
                           std::span<const BYTE> body)
{
  constexpr std::wstring_view kName = L"$EFS";
  const std::vector<WORD> encodedName = ToUtf16(kName);
  auto& attr =
      *reinterpret_cast<NtfsBrowser::Attr::HeaderResident*>(&record.at(offset));
  attr.header.type = AttrType::LOGGED_UTILITY_STREAM;
  attr.header.non_resident = 0;
  attr.header.flags = 0;
  attr.header.id = 0;
  attr.header.name_length = gsl::narrow<BYTE>(encodedName.size());
  attr.header.name_offset = static_cast<WORD>(sizeof(attr));
  attr.attr_size = gsl::narrow<DWORD>(body.size());
  attr.attr_offset =
      gsl::narrow<WORD>(sizeof(attr) + (encodedName.size() * sizeof(WORD)));
  attr.header.total_size = AlignAttrSize(attr.attr_offset + attr.attr_size);

  std::memcpy(&record.at(offset + attr.header.name_offset), encodedName.data(),
              encodedName.size() * sizeof(WORD));
  std::memcpy(&record.at(offset + attr.attr_offset), body.data(), body.size());
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
    offset += WriteNonResidentAttr(
        record, offset, AttrType::DATA, 0, stream.real_size, stream.runs,
        {.name = stream.name, .flags = flags, .ini_size = stream.ini_size});
  }

  std::vector<FakeDataRun> efsRuns;
  if (!file.efs_stream.empty())
  {
    if (file.efs_resident)
    {
      const DWORD efsOffset = offset;
      offset += WriteResidentEfsAttr(record, offset, file.efs_stream);
      if (file.efs_body_overruns)
      {
        // attr_offset + attr_size now reaches past total_size.
        constexpr DWORD kOverrun = 64;
        reinterpret_cast<NtfsBrowser::Attr::HeaderResident&>(
            record.at(efsOffset))
            .attr_size += kOverrun;
      }
    }
    else
    {
      efsRuns.push_back(
          {kFakeEfsStreamLcn,
           gsl::narrow<DWORD>((file.efs_stream.size() + kClusterSize - 1) /
                              kClusterSize)});
      offset += WriteNonResidentAttr(
          record, offset, AttrType::LOGGED_UTILITY_STREAM, 0,
          file.efs_stream.size(), efsRuns, {.name = L"$EFS"});
    }
  }
  if (file.trailing_undersized_attribute)
  {
    WriteTrailingDefect(record, offset, FakeTrailingDefect::UndersizedHeader);
  }
  else
  {
    WriteEndOfAttributesMarker(record, offset);
  }

  std::vector<BYTE> image = BuildFakeNtfsImage();
  const DWORD mftAddr = static_cast<DWORD>(kMftLcn) * kClusterSize;
  const size_t rootOffset = mftAddr + static_cast<size_t>(kFakeFileRecordSize) *
                                          static_cast<size_t>(MftIdx::ROOT);
  std::memcpy(&image.at(rootOffset), record.data(), record.size());

  for (const FakeEncryptedStream& stream : file.streams)
  {
    LayRunBytes(image, stream.runs, stream.cluster_bytes);
  }
  LayRunBytes(image, efsRuns, file.efs_stream);

  return image;
}

// Builds a root-directory replacement whose sole attribute is a RESIDENT
// $DATA carrying the EFS "encrypted" attribute-header flag. Real NTFS never
// encrypts a resident stream (EFS only ever leaves file data non-resident),
// but AttachEfsContext() must still handle a forged one.
FakeRecord MakeResidentEncryptedDataRecord()
{
  FakeRecord record =
      MakeRecordHeader(kAttrOffset, NtfsBrowser::Flag::FileRecord::INUSE);

  auto& attr = *reinterpret_cast<NtfsBrowser::Attr::HeaderResident*>(
      &record.at(kAttrOffset));
  attr.header.type = AttrType::DATA;
  attr.header.non_resident = 0;
  attr.header.name_length = 0;
  attr.header.flags = NtfsBrowser::Efs::kAttrFlagEncrypted;
  attr.header.id = 0;
  attr.attr_size = gsl::narrow<DWORD>(kResidentEncryptedDataContent.size());
  attr.attr_offset = static_cast<WORD>(sizeof(attr));
  attr.header.total_size = AlignAttrSize(sizeof(attr) + attr.attr_size);

  std::memcpy(&record.at(kAttrOffset + attr.attr_offset),
              kResidentEncryptedDataContent.data(),
              kResidentEncryptedDataContent.size());

  WriteEndOfAttributesMarker(record, kAttrOffset + attr.header.total_size);
  return record;
}

std::vector<BYTE> BuildFakeNtfsImageWithResidentEncryptedData()
{
  std::vector<BYTE> image = BuildFakeNtfsImage();

  const DWORD mftAddr = static_cast<DWORD>(kMftLcn) * kClusterSize;
  const size_t rootOffset = mftAddr + static_cast<size_t>(kFakeFileRecordSize) *
                                          static_cast<size_t>(MftIdx::ROOT);
  const FakeRecord record = MakeResidentEncryptedDataRecord();
  std::memcpy(&image.at(rootOffset), record.data(), record.size());

  return image;
}

std::vector<BYTE> BuildFakeNtfsImageWithEncryptedDirectory()
{
  const std::array<FakeIndexName, 1> rootNames{
      {{kEncryptedDirectoryNames.at(0), kEncryptedDirectoryMftRefs.at(0),
        false}}};
  const std::array<FakeIndexName, 1> blockNames{
      {{kEncryptedDirectoryNames.at(1), kEncryptedDirectoryMftRefs.at(1),
        true}}};

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
      {{kEncryptedDirectoryNames.at(0), kEncryptedDirectoryMftRefs.at(0),
        false}}};
  const std::array<FakeIndexName, 1> blockNames{
      {{kEncryptedDirectoryNames.at(1), kEncryptedDirectoryMftRefs.at(1),
        true}}};

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
      &record.at(kAttrOffset));
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
  std::memcpy(&image.at(rootOffset), record.data(), record.size());

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
      {{kSurrogateNames.at(0), kSurrogateNameMftRefs.at(0),
        kSurrogateNameIsDirectory.at(0)},
       {kSurrogateNames.at(1), kSurrogateNameMftRefs.at(1),
        kSurrogateNameIsDirectory.at(1)}}};
  const std::array<FakeIndexName, 2> blockNames{
      {{kSurrogateNames.at(2), kSurrogateNameMftRefs.at(2),
        kSurrogateNameIsDirectory.at(2)},
       {kSurrogateNames.at(3), kSurrogateNameMftRefs.at(3),
        kSurrogateNameIsDirectory.at(3)}}};

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
constexpr ULONGLONG kOverflowingLcn = 1ULL << 53U;

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
      *reinterpret_cast<NtfsBrowser::Attr::HeaderResident*>(&record.at(offset));
  rootAttr.header.type = AttrType::INDEX_ROOT;
  rootAttr.header.non_resident = 0;
  rootAttr.header.name_length = 0;
  rootAttr.header.flags = 0;
  rootAttr.header.id = 0;
  rootAttr.attr_offset = static_cast<WORD>(sizeof(rootAttr));

  const std::span<BYTE> body =
      std::span<BYTE>(record).subspan(offset + rootAttr.attr_offset);
  auto& root = *reinterpret_cast<NtfsBrowser::Attr::IndexRoot*>(body.data());
  root.attr_type = AttrType::FILE_NAME;
  root.coll_rule = 0;
  root.ib_size = kFakeFileRecordSize;
  root.clusters_per_ib = 1;
  root.entry_offset = gsl::narrow<DWORD>(
      (&gsl::at(body, sizeof(NtfsBrowser::Attr::IndexRoot))) -
      reinterpret_cast<BYTE*>(&root.entry_offset));

  auto& first_entry = *reinterpret_cast<NtfsBrowser::Data::IndexEntry*>(
      &gsl::at(body, sizeof(NtfsBrowser::Attr::IndexRoot)));
  first_entry.mft_index = 0;
  first_entry.mft_sn = 0;
  first_entry.stream_size = 0;
  first_entry.flags = NtfsBrowser::Flag::IndexEntry::SUBNODE |
                      NtfsBrowser::Flag::IndexEntry::LAST;
  first_entry.size = static_cast<WORD>(AlignAttrSize(
      offsetof(NtfsBrowser::Data::IndexEntry, stream) + sizeof(ULONGLONG)));
  auto& subNodeVcn = SubNodeVcnSlot(first_entry);
  subNodeVcn = 0;

  root.total_entry_size = first_entry.size;
  root.alloc_entry_size = first_entry.size;
  root.flags = 0;

  rootAttr.attr_size =
      static_cast<DWORD>(sizeof(NtfsBrowser::Attr::IndexRoot)) +
      first_entry.size;
  rootAttr.header.total_size =
      AlignAttrSize(sizeof(rootAttr) + rootAttr.attr_size);

  offset += rootAttr.header.total_size;

  // $INDEX_ALLOCATION: compressed, comp_unit_size == kCompressionUnitSizeShift.
  auto& allocAttr = *reinterpret_cast<NtfsBrowser::Attr::HeaderNonResident*>(
      &record.at(offset));
  allocAttr.header.type = AttrType::INDEX_ALLOCATION;
  allocAttr.header.non_resident = 1;
  allocAttr.header.name_length = 0;
  allocAttr.header.flags = kAttrFlagCompressed;
  allocAttr.header.id = 0;
  allocAttr.start_vcn = 0;
  allocAttr.last_vcn = kCompressionUnitClusters - 1;
  allocAttr.comp_unit_size = kCompressionUnitSizeShift;
  allocAttr.alloc_size = ULONGLONG{kCompressionUnitClusters} * kClusterSize;
  allocAttr.real_size = kFakeFileRecordSize;
  allocAttr.ini_size = allocAttr.real_size;

  const auto headerSize = static_cast<WORD>(
      sizeof(allocAttr) + NtfsBrowser::Attr::kCompressedSizeFieldSize);
  allocAttr.data_run_offset = headerSize;

  const ULONGLONG compressedSize =
      static_cast<ULONGLONG>(realRunClusters) * kClusterSize;
  std::memcpy(&record.at(offset + sizeof(allocAttr)), &compressedSize,
              sizeof(compressedSize));

  const std::span<BYTE> dataRun =
      std::span<BYTE>(record).subspan(offset + headerSize);
  DWORD runLen = 0;
  // Real run: header 0x81 (8-byte LCN-offset field), an 8-byte LE delta of
  // kOverflowingLcn, covering realRunClusters clusters.
  gsl::at(dataRun, runLen++) = kRunHeader8LcnBytes;
  gsl::at(dataRun, runLen++) = gsl::narrow<BYTE>(realRunClusters);
  {
    const auto delta = static_cast<LONGLONG>(kOverflowingLcn);
    std::memcpy(&gsl::at(dataRun, runLen), &delta, sizeof(delta));
    runLen += sizeof(delta);
  }
  if (realRunClusters < kCompressionUnitClusters)
  {
    // Sparse run padding the unit out to a whole compression unit (header
    // byte 0x01: 1-byte length field, 0-byte offset field).
    gsl::at(dataRun, runLen++) = 0x01;
    gsl::at(dataRun, runLen++) =
        gsl::narrow<BYTE>(kCompressionUnitClusters - realRunClusters);
  }
  gsl::at(dataRun, runLen++) = 0x00;  // terminate the run list

  allocAttr.header.total_size = AlignAttrSize(headerSize + runLen);

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
  std::memcpy(&image.at(rootOffset), record.data(), record.size());
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
  std::memcpy(&image.at(rootOffset), record.data(), record.size());
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

std::vector<BYTE> BuildFakeNtfsImageWithBadDataRun()
{
  // Two real 1-cluster runs (VCN 0, then VCN 1 if both were accepted), but
  // last_vcn is forged to 0: the second run's own last_vcn (1) then exceeds
  // it, mid-list.
  const std::vector<FakeDataRun> runs{{kBadDataRunFirstLcn, 1},
                                      {kBadDataRunSecondLcn, 1}};

  const FakeRecord record =
      MakeNonResidentDataRecord(NtfsBrowser::Flag::StdInfoPermission::ARCHIVE,
                                0, kClusterSize, runs, {.last_vcn = 0});

  std::vector<BYTE> image = BuildFakeNtfsImage();
  const DWORD mftAddr = static_cast<DWORD>(kMftLcn) * kClusterSize;
  const size_t rootOffset = mftAddr + static_cast<size_t>(kFakeFileRecordSize) *
                                          static_cast<size_t>(MftIdx::ROOT);
  std::memcpy(&image.at(rootOffset), record.data(), record.size());
  return image;
}

// LCN where BuildFakeNtfsImageWithBadIndexBlockEntry() writes its two real,
// sibling index blocks (VCN 0 and VCN 1), clear of every other fixture's
// placement in this file.
constexpr DWORD kBadIndexBlockLcn = 220;

// Builds a root-directory replacement whose $INDEX_ROOT holds two real
// subnode-pointer entries (VCN 0, VCN 1): both are true B+ tree children,
// reached by the normal walk with no recovery needed.
FakeRecord MakeBadIndexBlockEntryRootRecord()
{
  FakeRecord record =
      MakeRecordHeader(kAttrOffset, NtfsBrowser::Flag::FileRecord::INUSE |
                                        NtfsBrowser::Flag::FileRecord::DIR);

  DWORD offset = kAttrOffset;

  // $INDEX_ROOT
  auto& rootAttr =
      *reinterpret_cast<NtfsBrowser::Attr::HeaderResident*>(&record.at(offset));
  rootAttr.header.type = AttrType::INDEX_ROOT;
  rootAttr.header.non_resident = 0;
  rootAttr.header.name_length = 0;
  rootAttr.header.flags = 0;
  rootAttr.header.id = 0;
  rootAttr.attr_offset = static_cast<WORD>(sizeof(rootAttr));

  const std::span<BYTE> body =
      std::span<BYTE>(record).subspan(offset + rootAttr.attr_offset);
  auto& root = *reinterpret_cast<NtfsBrowser::Attr::IndexRoot*>(body.data());
  root.attr_type = AttrType::FILE_NAME;
  root.coll_rule = 0;
  root.ib_size = kClusterSize;
  root.clusters_per_ib = 1;
  root.entry_offset = gsl::narrow<DWORD>(
      (&gsl::at(body, sizeof(NtfsBrowser::Attr::IndexRoot))) -
      reinterpret_cast<BYTE*>(&root.entry_offset));

  // Entry 1: nameless, non-terminal subnode pointer to VCN 0 (the damaged
  // block).
  auto& first_entry = *reinterpret_cast<NtfsBrowser::Data::IndexEntry*>(
      &gsl::at(body, sizeof(NtfsBrowser::Attr::IndexRoot)));
  first_entry.mft_index = 0;
  first_entry.mft_sn = 0;
  first_entry.stream_size = 0;
  first_entry.flags = NtfsBrowser::Flag::IndexEntry::SUBNODE;
  first_entry.size = static_cast<WORD>(AlignAttrSize(
      offsetof(NtfsBrowser::Data::IndexEntry, stream) + sizeof(ULONGLONG)));
  SubNodeVcnSlot(first_entry) = 0;

  // Entry 2: nameless, terminal subnode pointer to VCN 1 (the good block).
  auto& second_entry =
      *reinterpret_cast<NtfsBrowser::Data::IndexEntry*>(&gsl::at(
          body, gsl::narrow<gsl::index>(sizeof(NtfsBrowser::Attr::IndexRoot) +
                                        first_entry.size)));
  second_entry.mft_index = 0;
  second_entry.mft_sn = 0;
  second_entry.stream_size = 0;
  second_entry.flags = NtfsBrowser::Flag::IndexEntry::SUBNODE |
                       NtfsBrowser::Flag::IndexEntry::LAST;
  second_entry.size = static_cast<WORD>(AlignAttrSize(
      offsetof(NtfsBrowser::Data::IndexEntry, stream) + sizeof(ULONGLONG)));
  SubNodeVcnSlot(second_entry) = 1;

  root.total_entry_size = first_entry.size + second_entry.size;
  root.alloc_entry_size = root.total_entry_size;
  root.flags = 0;

  rootAttr.attr_size =
      static_cast<DWORD>(sizeof(NtfsBrowser::Attr::IndexRoot)) +
      root.total_entry_size;
  rootAttr.header.total_size =
      AlignAttrSize(sizeof(rootAttr) + rootAttr.attr_size);

  offset += rootAttr.header.total_size;

  // $INDEX_ALLOCATION: 2 contiguous blocks at kBadIndexBlockLcn.
  auto& allocAttr = *reinterpret_cast<NtfsBrowser::Attr::HeaderNonResident*>(
      &record.at(offset));
  allocAttr.header.type = AttrType::INDEX_ALLOCATION;
  allocAttr.header.non_resident = 1;
  allocAttr.header.name_length = 0;
  allocAttr.header.flags = 0;
  allocAttr.header.id = 0;
  allocAttr.start_vcn = 0;
  allocAttr.last_vcn = 1;
  allocAttr.data_run_offset = static_cast<WORD>(sizeof(allocAttr));
  allocAttr.comp_unit_size = 0;
  allocAttr.real_size = 2ULL * kClusterSize;
  allocAttr.alloc_size = allocAttr.real_size;
  allocAttr.ini_size = allocAttr.real_size;

  const std::span<BYTE> dataRun =
      std::span<BYTE>(record).subspan(offset + allocAttr.data_run_offset);
  DWORD runLen = 0;
  // High nibble = LCN offset field size, low nibble = length field size.
  gsl::at(dataRun, runLen++) = kRunHeader4LcnBytes;
  gsl::at(dataRun, runLen++) = 2;  // 2 clusters
  {
    const DWORD lcn = kBadIndexBlockLcn;
    std::memcpy(&gsl::at(dataRun, runLen), &lcn, sizeof(lcn));
    runLen += sizeof(lcn);
  }
  gsl::at(dataRun, runLen++) = 0x00;  // terminate the run list

  allocAttr.header.total_size = AlignAttrSize(sizeof(allocAttr) + runLen);
  offset += allocAttr.header.total_size;

  WriteEndOfAttributesMarker(record, offset);
  return record;
}

// Writes one real (named, non-subnode) leaf entry at entryPtr and returns
// its size.
WORD WriteBadIndexBlockLeafEntry(BYTE* entryPtr, ULONGLONG mftRef,
                                 ULONGLONG parentRef, std::wstring_view name,
                                 bool last)
{
  const auto nameLength = gsl::narrow<BYTE>(name.size());
  auto& index_entry =
      *reinterpret_cast<NtfsBrowser::Data::IndexEntry*>(entryPtr);
  index_entry.mft_index = mftRef;
  index_entry.mft_sn = 1;

  auto& filename =
      *reinterpret_cast<NtfsBrowser::Attr::Filename*>(&index_entry.stream);
  filename.parent_ref = parentRef;
  filename.flags = NtfsBrowser::Flag::Filename::NONE;
  filename.name_length = nameLength;
  filename.name_space = NtfsBrowser::Flag::FilenameNamespace::WIN_32;
  for (BYTE i = 0; i < nameLength; i++)
  {
    // nameLength is name.size(), so i < name.size() by the loop condition.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    filename.name[i] = gsl::narrow<WORD>(name[i]);
  }

  index_entry.stream_size =
      gsl::narrow<WORD>(reinterpret_cast<BYTE*>(&filename.name[nameLength]) -
                        reinterpret_cast<BYTE*>(&filename));
  index_entry.flags = last ? NtfsBrowser::Flag::IndexEntry::LAST
                           : NtfsBrowser::Flag::IndexEntry{};
  index_entry.size = gsl::narrow<WORD>(AlignAttrSize(
      &index_entry.stream - reinterpret_cast<BYTE*>(&index_entry) +
      index_entry.stream_size));
  return index_entry.size;
}

// Returns the bytes of bad index block "vcn" that follow its header.
std::span<BYTE> BadIndexBlockBody(std::vector<BYTE>& image, DWORD vcn)
{
  const size_t blockOffset =
      (static_cast<size_t>(kBadIndexBlockLcn) + vcn) * kClusterSize;
  return std::span<BYTE>(image).subspan(
      blockOffset + sizeof(NtfsBrowser::Data::IndexBlock),
      kClusterSize - sizeof(NtfsBrowser::Data::IndexBlock));
}

// Writes the block header (magic, fixup, entry_offset) shared by both of
// BuildFakeNtfsImageWithBadIndexBlockEntry()'s blocks, at VCN vcn (relative
// to kBadIndexBlockLcn).
NtfsBrowser::Data::IndexBlock&
    WriteBadIndexBlockHeader(std::vector<BYTE>& image, DWORD vcn)
{
  const size_t blockOffset =
      (static_cast<size_t>(kBadIndexBlockLcn) + vcn) * kClusterSize;
  const std::span<BYTE> blockStart =
      std::span<BYTE>(image).subspan(blockOffset);
  auto& block =
      *reinterpret_cast<NtfsBrowser::Data::IndexBlock*>(blockStart.data());
  std::memset(&block, 0, sizeof(block));
  block.magic = kIndexBlockMagic;
  // Points at the block's own last 6 bytes, so PatchUS() succeeds trivially
  // without a real fixup array.
  block.offset_of_us = static_cast<WORD>(kClusterSize - kUsSlotSize);
  block.size_of_us = 3;
  block.vcn = vcn;
  block.entry_offset = gsl::narrow<DWORD>(
      (&gsl::at(blockStart, sizeof(NtfsBrowser::Data::IndexBlock))) -
      reinterpret_cast<BYTE*>(&block.entry_offset));
  block.not_leaf = 0;
  return block;
}

// VCN 0: one real entry ("First"), then an entry whose declared size
// overruns the block - AttrIndexAlloc::ParseIndexBlock() must reject the
// whole block when strict, but keep "First" (parsed before the bad entry)
// when recovering.
void WriteBadIndexBlockDamagedBlock(std::vector<BYTE>& image,
                                    ULONGLONG parentRef)
{
  NtfsBrowser::Data::IndexBlock& block = WriteBadIndexBlockHeader(image, 0);
  const std::span<BYTE> body = BadIndexBlockBody(image, 0);

  const WORD sizeA = WriteBadIndexBlockLeafEntry(
      body.data(), kBadIndexBlockFirstMftRef, parentRef,
      kBadIndexBlockFirstName, /*last=*/false);

  auto& entryB =
      *reinterpret_cast<NtfsBrowser::Data::IndexEntry*>(&gsl::at(body, sizeA));
  entryB.mft_index = 0;
  entryB.mft_sn = 0;
  entryB.stream_size = 0;
  entryB.flags = NtfsBrowser::Flag::IndexEntry::LAST;
  // Declares far more than the block actually has left after entry A:
  // ParseIndexBlock()'s "index entry exceeds block bounds" check.
  entryB.size = static_cast<WORD>(kClusterSize);

  block.total_entry_size = static_cast<DWORD>(sizeA) + entryB.size;
  block.alloc_entry_size = block.total_entry_size;
}

// VCN 1: one well-formed, terminal leaf entry ("Good") - a sibling,
// unaffected by the other block's defect.
void WriteBadIndexBlockGoodBlock(std::vector<BYTE>& image, ULONGLONG parentRef)
{
  NtfsBrowser::Data::IndexBlock& block = WriteBadIndexBlockHeader(image, 1);
  const std::span<BYTE> body = BadIndexBlockBody(image, 1);

  const WORD sizeGood = WriteBadIndexBlockLeafEntry(
      body.data(), kBadIndexBlockGoodMftRef, parentRef, kBadIndexBlockGoodName,
      /*last=*/true);

  block.total_entry_size = sizeGood;
  block.alloc_entry_size = sizeGood;
}

std::vector<BYTE> BuildFakeNtfsImageWithBadIndexBlockEntry()
{
  std::vector<BYTE> image = BuildFakeNtfsImage();

  const DWORD mftAddr = static_cast<DWORD>(kMftLcn) * kClusterSize;
  const size_t rootOffset = mftAddr + static_cast<size_t>(kFakeFileRecordSize) *
                                          static_cast<size_t>(MftIdx::ROOT);
  const FakeRecord record = MakeBadIndexBlockEntryRootRecord();
  std::memcpy(&image.at(rootOffset), record.data(), record.size());

  const size_t blocksOffset =
      static_cast<size_t>(kBadIndexBlockLcn) * kClusterSize;
  const size_t blocksBytes = 2ULL * kClusterSize;
  // FULL_CACHE always reads a whole 64KiB-aligned block, so the image must
  // extend past the blocks' real end or its last read fails outright.
  constexpr size_t kFullCacheReadBlockSize = size_t{64} * 1024;
  const size_t blocksEnd = blocksOffset + blocksBytes;
  const size_t alignedBlocksEnd =
      ((blocksEnd + kFullCacheReadBlockSize - 1) / kFullCacheReadBlockSize) *
      kFullCacheReadBlockSize;
  if (image.size() < alignedBlocksEnd)
  {
    image.resize(alignedBlocksEnd, 0);
  }

  const ULONGLONG rootRef = MakeFileReference(
      static_cast<ULONGLONG>(MftIdx::ROOT), kRootSequenceNumber);

  WriteBadIndexBlockDamagedBlock(image, rootRef);
  WriteBadIndexBlockGoodBlock(image, rootRef);

  return image;
}

// Builds a root-directory replacement whose $INDEX_ROOT holds a single,
// terminal entry: a real 3-character name is written on disk, but
// name_length claims far more than that.
FakeRecord MakeMalformedIndexEntryFilenameRootRecord()
{
  FakeRecord record =
      MakeRecordHeader(kAttrOffset, NtfsBrowser::Flag::FileRecord::INUSE |
                                        NtfsBrowser::Flag::FileRecord::DIR);

  DWORD offset = kAttrOffset;

  auto& rootAttr =
      *reinterpret_cast<NtfsBrowser::Attr::HeaderResident*>(&record.at(offset));
  rootAttr.header.type = AttrType::INDEX_ROOT;
  rootAttr.header.non_resident = 0;
  rootAttr.header.name_length = 0;
  rootAttr.header.flags = 0;
  rootAttr.header.id = 0;
  rootAttr.attr_offset = static_cast<WORD>(sizeof(rootAttr));

  const std::span<BYTE> body =
      std::span<BYTE>(record).subspan(offset + rootAttr.attr_offset);
  auto& root = *reinterpret_cast<NtfsBrowser::Attr::IndexRoot*>(body.data());
  root.attr_type = AttrType::FILE_NAME;
  root.coll_rule = 0;
  root.ib_size = kClusterSize;
  root.clusters_per_ib = 1;
  root.entry_offset = gsl::narrow<DWORD>(
      (&gsl::at(body, sizeof(NtfsBrowser::Attr::IndexRoot))) -
      reinterpret_cast<BYTE*>(&root.entry_offset));

  auto& first_entry = *reinterpret_cast<NtfsBrowser::Data::IndexEntry*>(
      &gsl::at(body, sizeof(NtfsBrowser::Attr::IndexRoot)));
  first_entry.mft_index = kMalformedIndexEntryMftRef;
  first_entry.mft_sn = 1;

  auto& fn1 =
      *reinterpret_cast<NtfsBrowser::Attr::Filename*>(&first_entry.stream);
  fn1.parent_ref = MakeFileReference(static_cast<ULONGLONG>(MftIdx::ROOT),
                                     kRootSequenceNumber);
  fn1.flags = NtfsBrowser::Flag::Filename::NONE;
  fn1.name_space = NtfsBrowser::Flag::FilenameNamespace::WIN_32;

  // The real, on-disk name is short; stream_size (and hence e1.size) is
  // sized to it, not to the forged name_length below.
  constexpr std::wstring_view kRealName = L"Bad";
  constexpr BYTE kRealNameLength = 3;
  for (BYTE i = 0; i < kRealNameLength; i++)
  {
    fn1.name[i] = gsl::narrow<WORD>(kRealName[i]);
  }
  // Claims far more characters than the entry has room for.
  fn1.name_length = kOverlongNameLength;

  first_entry.stream_size =
      gsl::narrow<WORD>(reinterpret_cast<BYTE*>(&fn1.name[kRealNameLength]) -
                        reinterpret_cast<BYTE*>(&fn1));
  first_entry.flags = NtfsBrowser::Flag::IndexEntry::LAST;
  first_entry.size = gsl::narrow<WORD>(AlignAttrSize(
      &first_entry.stream - reinterpret_cast<BYTE*>(&first_entry) +
      first_entry.stream_size));

  root.total_entry_size = first_entry.size;
  root.alloc_entry_size = first_entry.size;
  root.flags = 0;

  rootAttr.attr_size =
      static_cast<DWORD>(sizeof(NtfsBrowser::Attr::IndexRoot)) +
      first_entry.size;
  rootAttr.header.total_size =
      AlignAttrSize(sizeof(rootAttr) + rootAttr.attr_size);

  offset += rootAttr.header.total_size;
  WriteEndOfAttributesMarker(record, offset);
  return record;
}

std::vector<BYTE> BuildFakeNtfsImageWithMalformedIndexEntryFilename()
{
  std::vector<BYTE> image = BuildFakeNtfsImage();
  const DWORD mftAddr = static_cast<DWORD>(kMftLcn) * kClusterSize;
  const size_t rootOffset = mftAddr + static_cast<size_t>(kFakeFileRecordSize) *
                                          static_cast<size_t>(MftIdx::ROOT);
  const FakeRecord record = MakeMalformedIndexEntryFilenameRootRecord();
  std::memcpy(&image.at(rootOffset), record.data(), record.size());
  return image;
}

// Builds a root-directory replacement whose sole resident $DATA attribute's
// total_size reaches exactly to the end of the file record: no bytes are
// left for a trailing AttrType::ALL end-of-attributes marker. The trailing
// bytes stay zero (never written), matching kOffsetOfUs's self-consistent
// fixup trick.
FakeRecord MakeNoEndMarkerRecord()
{
  FakeRecord record =
      MakeRecordHeader(kAttrOffset, NtfsBrowser::Flag::FileRecord::INUSE);

  auto& attr = *reinterpret_cast<NtfsBrowser::Attr::HeaderResident*>(
      &record.at(kAttrOffset));
  attr.header.type = AttrType::DATA;
  attr.header.non_resident = 0;
  attr.header.name_length = 0;
  attr.header.flags = 0;
  attr.header.id = 0;
  attr.attr_offset = static_cast<WORD>(sizeof(attr));

  // Fills the record exactly to its own end, leaving no room for a marker.
  const DWORD totalSize = kFakeFileRecordSize - kAttrOffset;
  attr.attr_size = totalSize - static_cast<DWORD>(sizeof(attr));
  attr.header.total_size = totalSize;

  return record;
}

std::vector<BYTE> BuildFakeNtfsImageWithNoEndMarker()
{
  std::vector<BYTE> image = BuildFakeNtfsImage();

  const DWORD mftAddr = static_cast<DWORD>(kMftLcn) * kClusterSize;
  const size_t rootOffset = mftAddr + static_cast<size_t>(kFakeFileRecordSize) *
                                          static_cast<size_t>(MftIdx::ROOT);
  const FakeRecord record = MakeNoEndMarkerRecord();
  std::memcpy(&image.at(rootOffset), record.data(), record.size());

  return image;
}

std::filesystem::path WriteFakeNtfsImage()
{
  const std::vector<BYTE> image = BuildFakeNtfsImage();

  std::random_device random_device;
  const std::filesystem::path path =
      std::filesystem::temp_directory_path() /
      (L"ntfsbrowser-fake-volume-" + std::to_wstring(random_device()) +
       L".img");

  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  out.write(reinterpret_cast<const char*>(image.data()),
            gsl::narrow<std::streamsize>(image.size()));

  return path;
}

namespace
{

// Fixed on-disk size of a nameless $ATTRIBUTE_LIST entry.
constexpr WORD kListEntrySize = static_cast<WORD>(
    AlignAttrSize(NtfsBrowser::Attr::kAttributeListEntryHeaderSize));

// LCNs the lifetime fixtures use for cluster data: past the $MFT records
// (LCN 1 onwards) and distinct, so no two streams share a cluster.
constexpr DWORD kLifetimeBaseDataLcn = 50;
constexpr DWORD kLifetimeExtDataLcn = 51;
constexpr DWORD kLifetimeListLcn = 52;

// Writes one nameless $ATTRIBUTE_LIST entry at dest: type, the record it
// points to (with the sequence number it claims for it), and the record_size
// to declare (0 is the forged value).
void WriteListEntry(BYTE* dest, AttrType type, ULONGLONG record,
                    WORD recordSize, WORD sequence = 0)
{
  auto& entry = *reinterpret_cast<NtfsBrowser::Attr::AttributeList*>(dest);
  entry.attr_type = type;
  entry.record_size = recordSize;
  entry.name_length = 0;
  entry.name_offset = 0;
  entry.start_vcn = 0;
  entry.base_ref.segment_number = record;
  entry.base_ref.sequence_number = sequence;
  entry.attr_id = 0;
}

// Writes a resident $ATTRIBUTE_LIST holding "entryCount" entries, all for
// $DATA in the lifetime extension record, and returns its total_size. The
// entry at index "zeroSizeIndex", if any, declares a record_size of 0. Every
// entry claims "entrySequence" for the extension record.
DWORD WriteLifetimeResidentList(FakeRecord& record, DWORD offset,
                                size_t entryCount,
                                std::optional<size_t> zeroSizeIndex = {},
                                WORD entrySequence = 0)
{
  auto& attr =
      *reinterpret_cast<NtfsBrowser::Attr::HeaderResident*>(&record.at(offset));
  attr.header.type = AttrType::ATTRIBUTE_LIST;
  attr.header.non_resident = 0;
  attr.header.name_length = 0;
  attr.header.flags = 0;
  attr.header.id = 0;
  attr.attr_offset = static_cast<WORD>(sizeof(attr));
  attr.attr_size = gsl::narrow<DWORD>(kListEntrySize * entryCount);
  attr.header.total_size = AlignAttrSize(sizeof(attr) + attr.attr_size);

  for (size_t i = 0; i < entryCount; i++)
  {
    WriteListEntry(&record.at(offset + attr.attr_offset + (i * kListEntrySize)),
                   AttrType::DATA, kAttrListLifetimeExtIdx,
                   zeroSizeIndex == i ? static_cast<WORD>(0) : kListEntrySize,
                   entrySequence);
  }
  return attr.header.total_size;
}

// The extension link of an ordinary extension of kAttrListLifetimeBaseIdx.
constexpr FakeExtensionLink kOrdinaryLifetimeLink{.base_ref =
                                                      kAttrListLifetimeBaseIdx};

// Extension record holding one resident $DATA of kAttrListLifetimeDataContent.
FakeRecord MakeLifetimeResidentDataExtensionRecord(
    const FakeExtensionLink& link = kOrdinaryLifetimeLink)
{
  FakeRecord record =
      MakeRecordHeader(kAttrOffset, NtfsBrowser::Flag::FileRecord::INUSE);
  SetRecordLink(record, link.record_sequence, link.base_ref);

  DWORD offset = kAttrOffset;
  const DWORD dataOffset = offset;
  offset += WriteResidentDataAttr(record, offset,
                                  kAttrListLifetimeDataContent.size());
  std::memcpy(
      &record.at(dataOffset + sizeof(NtfsBrowser::Attr::HeaderResident)),
      kAttrListLifetimeDataContent.data(), kAttrListLifetimeDataContent.size());

  WriteEndOfAttributesMarker(record, offset);
  return record;
}

}  // namespace

std::vector<BYTE> BuildFakeNtfsImageWithAttributeListImportThenZeroRecordSize()
{
  std::vector<BYTE> image = BuildFakeNtfsImage();
  const DWORD mftAddr = static_cast<DWORD>(kMftLcn) * kClusterSize;

  FakeRecord base =
      MakeRecordHeader(kAttrOffset, NtfsBrowser::Flag::FileRecord::INUSE);
  DWORD offset = kAttrOffset;
  offset += WriteLifetimeResidentList(base, offset, 2, 1);
  WriteEndOfAttributesMarker(base, offset);

  PutMftRecord(image, mftAddr, kAttrListLifetimeBaseIdx, base);
  PutMftRecord(image, mftAddr, kAttrListLifetimeExtIdx,
               MakeLifetimeResidentDataExtensionRecord());
  return image;
}

std::vector<BYTE> BuildFakeNtfsImageWithSplitAttributeListAttribute()
{
  std::vector<BYTE> image = BuildFakeNtfsImage();
  const DWORD mftAddr = static_cast<DWORD>(kMftLcn) * kClusterSize;

  FakeRecord base =
      MakeRecordHeader(kAttrOffset, NtfsBrowser::Flag::FileRecord::INUSE);
  DWORD offset = kAttrOffset;
  // VCN 0-0: empty.
  offset +=
      WriteNonResidentAttr(base, offset, AttrType::ATTRIBUTE_LIST, 0, 0, {});
  // VCN 1-1: one entry, stored in its own cluster.
  const std::vector<FakeDataRun> runs{{kLifetimeListLcn, 1}};
  offset += WriteNonResidentAttr(base, offset, AttrType::ATTRIBUTE_LIST, 0,
                                 kListEntrySize, runs, {.start_vcn = 1});
  WriteEndOfAttributesMarker(base, offset);

  PutMftRecord(image, mftAddr, kAttrListLifetimeBaseIdx, base);
  PutMftRecord(image, mftAddr, kAttrListLifetimeExtIdx,
               MakeLifetimeResidentDataExtensionRecord());

  std::vector<BYTE> entry(kListEntrySize, 0);
  WriteListEntry(entry.data(), AttrType::DATA, kAttrListLifetimeExtIdx,
                 kListEntrySize);
  LayRunBytes(image, runs, entry);
  return image;
}

std::vector<BYTE>
    BuildFakeNtfsImageWithSplitDataAndTrailingDefect(FakeTrailingDefect defect)
{
  std::vector<BYTE> image = BuildFakeNtfsImage();
  const DWORD mftAddr = static_cast<DWORD>(kMftLcn) * kClusterSize;

  const std::vector<FakeDataRun> baseRuns{{kLifetimeBaseDataLcn, 1}};
  const std::vector<FakeDataRun> extRuns{{kLifetimeExtDataLcn, 1}};

  FakeRecord base =
      MakeRecordHeader(kAttrOffset, NtfsBrowser::Flag::FileRecord::INUSE);
  DWORD offset = kAttrOffset;
  offset += WriteLifetimeResidentList(base, offset, 1);
  offset += WriteNonResidentAttr(base, offset, AttrType::DATA, 0, kClusterSize,
                                 baseRuns);
  WriteTrailingDefect(base, offset, defect);

  FakeRecord ext =
      MakeRecordHeader(kAttrOffset, NtfsBrowser::Flag::FileRecord::INUSE);
  SetRecordLink(ext, 0, kAttrListLifetimeBaseIdx);
  DWORD extOffset = kAttrOffset;
  extOffset += WriteNonResidentAttr(ext, extOffset, AttrType::DATA, 0, 0,
                                    extRuns, {.start_vcn = 1});
  WriteEndOfAttributesMarker(ext, extOffset);

  PutMftRecord(image, mftAddr, kAttrListLifetimeBaseIdx, base);
  PutMftRecord(image, mftAddr, kAttrListLifetimeExtIdx, ext);
  LayRunBytes(image, baseRuns, {});
  LayRunBytes(image, extRuns, {});
  return image;
}

std::vector<BYTE> BuildFakeNtfsImageWithExtensionLink(FakeExtensionLink link)
{
  std::vector<BYTE> image = BuildFakeNtfsImage();
  const DWORD mftAddr = static_cast<DWORD>(kMftLcn) * kClusterSize;

  FakeRecord base =
      MakeRecordHeader(kAttrOffset, NtfsBrowser::Flag::FileRecord::INUSE);
  DWORD offset = kAttrOffset;
  offset += WriteLifetimeResidentList(base, offset, 1, {}, link.entry_sequence);
  WriteEndOfAttributesMarker(base, offset);

  PutMftRecord(image, mftAddr, kAttrListLifetimeBaseIdx, base);
  PutMftRecord(image, mftAddr, kAttrListLifetimeExtIdx,
               MakeLifetimeResidentDataExtensionRecord(link));
  return image;
}

std::vector<BYTE> BuildFakeNtfsImageWithMftDataSplitLink(FakeExtensionLink link)
{
  std::vector<BYTE> image = BuildFakeNtfsImage();
  const DWORD mftAddr = static_cast<DWORD>(kMftLcn) * kClusterSize;

  const std::array<std::pair<ULONGLONG, ULONGLONG>, 1> continuations{
      {{kMftDataSplitExtIdx, kMftDataSplitTargetIdx}}};
  PutMftRecord(image, mftAddr, static_cast<ULONGLONG>(MftIdx::MFT),
               MakeMftRecordWithDataContinuations(continuations, 0,
                                                  link.entry_sequence));
  PutMftRecord(image, mftAddr, kMftDataSplitExtIdx,
               MakeMftDataContinuationExtensionRecord(kMftDataSplitTargetIdx,
                                                      kMftDataSplitLcn, 1,
                                                      link.record_sequence));

  FakeRecord targetRecord =
      MakeRecordHeader(kAttrOffset, NtfsBrowser::Flag::FileRecord::INUSE);
  WriteEndOfAttributesMarker(targetRecord, kAttrOffset);
  PutRecordAt(image, static_cast<size_t>(kMftDataSplitLcn) * kClusterSize,
              targetRecord);

  return image;
}

std::vector<BYTE> BuildFakeNtfsImageWithMftDataTwoExtentsInOneRecord()
{
  std::vector<BYTE> image = BuildFakeNtfsImage();
  const DWORD mftAddr = static_cast<DWORD>(kMftLcn) * kClusterSize;

  const std::array<std::pair<ULONGLONG, ULONGLONG>, 2> continuations{
      {{kMftTwoExtentsExtIdx, kMftTwoExtentsFirstVcn},
       {kMftTwoExtentsExtIdx, kMftTwoExtentsSecondVcn}}};
  PutMftRecord(image, mftAddr, static_cast<ULONGLONG>(MftIdx::MFT),
               MakeMftRecordWithDataContinuations(continuations));

  const std::vector<FakeDataRun> firstRuns{{kMftTwoExtentsFirstLcn, 1}};
  const std::vector<FakeDataRun> secondRuns{{kMftTwoExtentsSecondLcn, 1}};
  FakeRecord ext =
      MakeRecordHeader(kAttrOffset, NtfsBrowser::Flag::FileRecord::INUSE);
  SetRecordLink(ext, 0, static_cast<ULONGLONG>(MftIdx::MFT));
  DWORD extOffset = kAttrOffset;
  extOffset += WriteNonResidentAttr(ext, extOffset, AttrType::DATA, 0,
                                    kFakeFileRecordSize, firstRuns,
                                    {.start_vcn = kMftTwoExtentsFirstVcn});
  extOffset += WriteNonResidentAttr(ext, extOffset, AttrType::DATA, 0,
                                    kFakeFileRecordSize, secondRuns,
                                    {.start_vcn = kMftTwoExtentsSecondVcn});
  WriteEndOfAttributesMarker(ext, extOffset);
  PutMftRecord(image, mftAddr, kMftTwoExtentsExtIdx, ext);

  FakeRecord targetRecord =
      MakeRecordHeader(kAttrOffset, NtfsBrowser::Flag::FileRecord::INUSE);
  WriteEndOfAttributesMarker(targetRecord, kAttrOffset);
  PutRecordAt(image, static_cast<size_t>(kMftTwoExtentsFirstLcn) * kClusterSize,
              targetRecord);
  PutRecordAt(image,
              static_cast<size_t>(kMftTwoExtentsSecondLcn) * kClusterSize,
              targetRecord);

  return image;
}

namespace
{

// Appends one run to "runs": a 1-cluster run whose LCN offset field is the
// 8-byte value "delta".
void AppendEightByteLcnRun(std::vector<BYTE>& runs, LONGLONG delta)
{
  runs.push_back(kRunHeader8LcnBytes);
  runs.push_back(1);
  const size_t position = runs.size();
  runs.resize(position + sizeof(delta));
  std::memcpy(&runs.at(position), &delta, sizeof(delta));
}

// Same volume as BuildFakeNtfsImage(), with the root record (#5) replaced by
// one whose stream, of "host" kind, has the hand-encoded run list "runs".
// It covers "clusters" 1-cluster runs. No cluster is laid down: a run is
// never expected to be read successfully.
std::vector<BYTE> BuildImageWithRawRuns(FakeRunHost host,
                                        std::vector<BYTE> runs, DWORD clusters)
{
  runs.push_back(0x00);  // terminate the run list

  const std::vector<FakeDataRun> placeholder{{{}, clusters}};
  const ULONGLONG realSize = static_cast<ULONGLONG>(clusters) * kClusterSize;
  const FakeNonResidentOverrides overrides{.raw_runs = std::move(runs)};
  const auto permission = NtfsBrowser::Flag::StdInfoPermission::ARCHIVE;

  const FakeRecord record =
      (host == FakeRunHost::Data)
          ? MakeNonResidentDataRecord(permission, 0, realSize, placeholder,
                                      overrides)
          : MakeIndexAllocationDirRecord(permission, 0, realSize, placeholder,
                                         overrides);
  return BuildCompressionImage(record, {}, {});
}

}  // namespace

std::vector<BYTE> BuildFakeNtfsImageWithWrappingLcn(FakeRunHost host)
{
  std::vector<BYTE> runs;
  AppendEightByteLcnRun(runs, static_cast<LONGLONG>(kWrappingLcn));
  return BuildImageWithRawRuns(host, std::move(runs), 1);
}

std::vector<BYTE> BuildFakeNtfsImageWithOverflowingLcnSum(FakeRunHost host)
{
  std::vector<BYTE> runs;
  AppendEightByteLcnRun(runs, std::numeric_limits<LONGLONG>::max());
  AppendEightByteLcnRun(runs, std::numeric_limits<LONGLONG>::max());
  return BuildImageWithRawRuns(host, std::move(runs), 2);
}

}  // namespace NtfsBrowserTests
