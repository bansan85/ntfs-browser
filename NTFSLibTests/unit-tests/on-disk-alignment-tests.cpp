#include <ntfs-browser/win-types.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <optional>
#include <span>
#include <utility>
#include <vector>

#include <catch2/catch_template_test_macros.hpp>
#include <catch2/catch_test_macros.hpp>

#include <ntfs-browser/data/attr-header-common.h>
#include <ntfs-browser/data/attr-type.h>
#include <ntfs-browser/disk-reader.h>
#include <ntfs-browser/file-record.h>
#include <ntfs-browser/index-entry.h>
#include <ntfs-browser/mft-idx.h>
#include <ntfs-browser/ntfs-volume.h>
#include <ntfs-browser/strategy.h>

#include "attr/filename.h"
#include "attr/header-non-resident.h"
#include "attr/header-resident.h"
#include "attr/index-root.h"
#include "data/file-record-header.h"
#include "data/index-block.h"
#include "data/index-entry.h"
#include "fake-ntfs-image.h"
#include "file-record-header-edit.h"
#include "memory-disk-reader.h"
#include "optional-access.h"

using NtfsBrowser::AttrHeaderCommon;
using NtfsBrowser::AttrType;
using NtfsBrowser::FileRecord;
using NtfsBrowser::FileRecordHeader;
using NtfsBrowser::FileRecordHeaderImpl;
using NtfsBrowser::IndexEntry;
using NtfsBrowser::IndexEntryView;
using NtfsBrowser::kFileRecordMagic;
using NtfsBrowser::NtfsVolume;
using NtfsBrowser::Strategy;
using NtfsBrowser::Enum::MftIdx;

namespace
{

// Every image BuildFakeNtfsImage() makes puts $MFT in its second cluster.
constexpr size_t kMftOffset = NtfsBrowserTests::kFakeClusterSize;

// The first attribute of every fake record starts right after the fixed
// header.
constexpr size_t kFirstAttrOffset = 48;

// Size of the resident attributes below: their header and one body byte. It
// is not a multiple of 4, so the attribute after it sits off any alignment.
constexpr DWORD kOddAttrSize = sizeof(NtfsBrowser::Attr::HeaderResident) + 1;

size_t RecordOffset(ULONGLONG idx)
{
  return kMftOffset + (NtfsBrowserTests::kFakeFileRecordSize * idx);
}

// Copies value into image at offset. A plain store through a typed pointer
// would itself be a misaligned access.
template <class T>
void Put(std::vector<BYTE>& image, size_t offset, T value)
{
  std::memcpy(&image.at(offset), &value, sizeof(value));
}

// Writes a resident attribute of type at offset, kOddAttrSize bytes long.
void PutOddSizedAttr(std::vector<BYTE>& image, size_t offset, AttrType type)
{
  NtfsBrowser::Attr::HeaderResident header{};
  header.header.type = type;
  header.header.total_size = kOddAttrSize;
  header.attr_size = 1;
  header.attr_offset = sizeof(header);
  Put(image, offset, header);
}

template <Strategy S>
void RunOddSizedAttributesAreParsedAligned()
{
  std::vector<BYTE> image = NtfsBrowserTests::BuildFakeNtfsImage();
  const size_t rootOffset = RecordOffset(static_cast<ULONGLONG>(MftIdx::ROOT));
  size_t offset = rootOffset + kFirstAttrOffset;
  PutOddSizedAttr(image, offset, AttrType::DATA);
  offset += kOddAttrSize;
  PutOddSizedAttr(image, offset, AttrType::REPARSE_POINT);
  offset += kOddAttrSize;
  Put(image, offset, static_cast<DWORD>(AttrType::ALL));

  NtfsVolume<S> const volume(
      std::make_unique<NtfsBrowserTests::MemoryDiskReader>(std::move(image)));
  REQUIRE(volume.IsVolumeOK());

  FileRecord<S> record(volume);
  REQUIRE(record.ParseFileRecord(static_cast<ULONGLONG>(MftIdx::ROOT)));
  REQUIRE(record.ParseAttrs());

  for (const AttrType type : {AttrType::DATA, AttrType::REPARSE_POINT})
  {
    const auto& attrs = record.getAttr(type);
    REQUIRE(attrs.size() == 1);
    const auto address =
        reinterpret_cast<std::uintptr_t>(&attrs.front()->GetAttrHeader());
    CHECK(address % alignof(NtfsBrowser::Attr::HeaderNonResident) == 0);
    CHECK(attrs.front()->GetAttrTotalSize() == kOddAttrSize);
  }
}

// Number of entries TraverseSubEntries() reports for the root directory.
size_t CountRootEntries(std::vector<BYTE> image)
{
  NtfsVolume<Strategy::NO_CACHE> const volume(
      std::make_unique<NtfsBrowserTests::MemoryDiskReader>(std::move(image)));
  REQUIRE(volume.IsVolumeOK());

  FileRecord<Strategy::NO_CACHE> record(volume);
  REQUIRE(record.ParseFileRecord(static_cast<ULONGLONG>(MftIdx::ROOT)));
  REQUIRE(record.ParseAttrs());

  size_t count = 0;
  record.TraverseSubEntries([](const IndexEntryView&, void* context)
                            { ++*static_cast<size_t*>(context); }, &count);
  return count;
}

}  // namespace

TEMPLATE_TEST_CASE_SIG(
    "ParseAttrs binds every attribute at an aligned address, whatever the "
    "total_size of the attribute before it",
    "[file-record][alignment][regression]", ((Strategy S), S),
    Strategy::NO_CACHE, Strategy::FULL_CACHE)
{
  RunOddSizedAttributesAreParsedAligned<S>();
}

TEMPLATE_TEST_CASE_SIG(
    "FindSubEntry reads an $INDEX_ROOT entry that starts off an 8-byte "
    "boundary",
    "[index-entry][alignment][regression]", ((Strategy S), S),
    Strategy::NO_CACHE, Strategy::FULL_CACHE)
{
  constexpr BYTE kShortenedNameLength = 2;
  constexpr WORD kShortenedStreamSize =
      offsetof(NtfsBrowser::Attr::Filename, name) +
      (size_t{2} * kShortenedNameLength);
  constexpr WORD kShortenedEntrySize =
      offsetof(NtfsBrowser::Data::IndexEntry, stream) + kShortenedStreamSize;
  constexpr WORD kTerminatorSize =
      offsetof(NtfsBrowser::Data::IndexEntry, stream);
  constexpr DWORD kEntriesSize = kShortenedEntrySize + kTerminatorSize;
  static_assert(kShortenedEntrySize % alignof(NtfsBrowser::Data::IndexEntry) !=
                0);

  std::vector<BYTE> image =
      NtfsBrowserTests::BuildFakeNtfsImageWithIndexRootVariants();
  const size_t attrOffset =
      RecordOffset(NtfsBrowserTests::kIndexRootVariantADirIdx) +
      kFirstAttrOffset;
  const size_t rootOffset =
      attrOffset + sizeof(NtfsBrowser::Attr::HeaderResident);
  const size_t entryOffset = rootOffset + sizeof(NtfsBrowser::Attr::IndexRoot);

  // The fixture's one real entry is "AAA" (88 bytes), then the terminator.
  // Dropping a character moves the terminator 2 bytes down.
  constexpr size_t kOriginalEntrySize = 88;
  Put(image,
      entryOffset + offsetof(NtfsBrowser::Data::IndexEntry, stream) +
          offsetof(NtfsBrowser::Attr::Filename, name_length),
      kShortenedNameLength);
  Put(image, entryOffset + offsetof(NtfsBrowser::Data::IndexEntry, stream_size),
      kShortenedStreamSize);
  Put(image, entryOffset + offsetof(NtfsBrowser::Data::IndexEntry, size),
      kShortenedEntrySize);
  std::memmove(&image.at(entryOffset + kShortenedEntrySize),
               &image.at(entryOffset + kOriginalEntrySize), kTerminatorSize);
  std::memset(&image.at(entryOffset + kEntriesSize), 0, 2);

  Put(image,
      rootOffset + offsetof(NtfsBrowser::Attr::IndexRoot, total_entry_size),
      kEntriesSize);
  Put(image,
      rootOffset + offsetof(NtfsBrowser::Attr::IndexRoot, alloc_entry_size),
      kEntriesSize);
  const DWORD attrSize = sizeof(NtfsBrowser::Attr::IndexRoot) + kEntriesSize;
  Put(image,
      attrOffset + offsetof(NtfsBrowser::Attr::HeaderResident, attr_size),
      attrSize);
  const DWORD totalSize = sizeof(NtfsBrowser::Attr::HeaderResident) + attrSize;
  Put(image, attrOffset + offsetof(NtfsBrowser::AttrHeaderCommon, total_size),
      totalSize);
  std::memset(&image.at(attrOffset + totalSize), 0, 2 * sizeof(DWORD));
  Put(image, attrOffset + totalSize, static_cast<DWORD>(AttrType::ALL));

  NtfsVolume<S> const volume(
      std::make_unique<NtfsBrowserTests::MemoryDiskReader>(std::move(image)));
  REQUIRE(volume.IsVolumeOK());

  FileRecord<S> record(volume);
  REQUIRE(record.ParseFileRecord(NtfsBrowserTests::kIndexRootVariantADirIdx));
  REQUIRE(record.ParseAttrs());

  const std::optional<IndexEntry> entry = record.FindSubEntry(L"AA");
  REQUIRE(entry.has_value());
  CHECK(NtfsBrowserTests::Unwrap(entry).GetFileReference() ==
        NtfsBrowserTests::kIndexRootVariantAMftRef);
}

TEMPLATE_TEST_CASE_SIG(
    "FileRecordHeader reads an Update Sequence Array that starts at an odd "
    "offset",
    "[file-record-header][alignment][regression]", ((Strategy S), S),
    Strategy::NO_CACHE, Strategy::FULL_CACHE)
{
  constexpr size_t kRecordSize = 1024;
  constexpr WORD kOddOffsetOfUs = 49;
  constexpr WORD kUsn = 0x1234;
  constexpr WORD kFirstBlockWord = 0xAAAA;
  constexpr WORD kSecondBlockWord = 0xBBBB;

  std::vector<BYTE> storage(kRecordSize, 0);
  NtfsBrowserTests::EditFileRecordHeader(storage,
                                         [](FileRecordHeader::Data& header)
                                         {
                                           header.magic = kFileRecordMagic;
                                           header.offset_of_us = kOddOffsetOfUs;
                                           header.size_of_us = 3;
                                         });
  Put(storage, kOddOffsetOfUs, kUsn);
  Put(storage, kOddOffsetOfUs + sizeof(WORD), kFirstBlockWord);
  Put(storage, kOddOffsetOfUs + (2 * sizeof(WORD)), kSecondBlockWord);

  const auto record = FileRecordHeaderImpl<S>(std::span<const BYTE>(storage));

  CHECK(record.us_number == kUsn);
  REQUIRE(record.us_array.size() == 2);
  // The REQUIRE above checks the size of record.us_array.
  // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
  CHECK(record.us_array[0] == kFirstBlockWord);
  // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
  CHECK(record.us_array[1] == kSecondBlockWord);
}

TEST_CASE(
    "TraverseSubEntries reads an index block whose Update Sequence Array "
    "starts at an odd offset",
    "[attr-index-alloc][alignment][regression]")
{
  constexpr WORD kOddOffsetOfUs = NtfsBrowserTests::kFakeClusterSize - 7;

  std::vector<BYTE> image =
      NtfsBrowserTests::BuildFakeNtfsImageWithOrphanedIndexBlocks();
  const size_t expected = CountRootEntries(image);
  REQUIRE(expected > 0);

  // The first cluster-aligned index block is VCN 0, the one $INDEX_ROOT
  // points at. Its array is all zeros, so moving it one byte changes no
  // value.
  size_t blockOffset = 0;
  for (; blockOffset + sizeof(DWORD) <= image.size();
       blockOffset += NtfsBrowserTests::kFakeClusterSize)
  {
    DWORD magic = 0;
    std::memcpy(&magic, &image.at(blockOffset), sizeof(magic));
    if (magic == kIndexBlockMagic)
    {
      break;
    }
  }
  REQUIRE(blockOffset + NtfsBrowserTests::kFakeClusterSize <= image.size());
  Put(image,
      blockOffset + offsetof(NtfsBrowser::Data::IndexBlock, offset_of_us),
      kOddOffsetOfUs);

  const size_t actual = CountRootEntries(std::move(image));
  CHECK(actual == expected);
}
