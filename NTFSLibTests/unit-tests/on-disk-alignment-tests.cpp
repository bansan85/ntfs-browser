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
using NtfsBrowser::file_record_magic;
using NtfsBrowser::FileRecord;
using NtfsBrowser::FileRecordHeader;
using NtfsBrowser::FileRecordHeaderImpl;
using NtfsBrowser::IndexEntry;
using NtfsBrowser::IndexEntryView;
using NtfsBrowser::NtfsVolume;
using NtfsBrowser::Strategy;
using NtfsBrowser::Enum::MftIdx;

namespace
{

// Every image BuildFakeNtfsImage() makes puts $MFT in its second cluster.
constexpr size_t mft_offset = NtfsBrowserTests::fake_cluster_size;

// The first attribute of every fake record starts right after the fixed
// header.
constexpr size_t first_attr_offset = 48;

// Size of the resident attributes below: their header and one body byte. It
// is not a multiple of 4, so the attribute after it sits off any alignment.
constexpr DWORD odd_attr_size = sizeof(NtfsBrowser::Attr::HeaderResident) + 1;

size_t RecordOffset(ULONGLONG idx)
{
  return mft_offset + (NtfsBrowserTests::fake_file_record_size * idx);
}

// Copies value into image at offset. A plain store through a typed pointer
// would itself be a misaligned access.
template <class T>
void Put(std::vector<BYTE>& image, size_t offset, T value)
{
  std::memcpy(&image.at(offset), &value, sizeof(value));
}

// Writes a resident attribute of type at offset, odd_attr_size bytes long.
void PutOddSizedAttr(std::vector<BYTE>& image, size_t offset, AttrType type)
{
  NtfsBrowser::Attr::HeaderResident header{};
  header.header.type = type;
  header.header.total_size = odd_attr_size;
  header.attr_size = 1;
  header.attr_offset = sizeof(header);
  Put(image, offset, header);
}

template <Strategy S>
void RunOddSizedAttributesAreParsedAligned()
{
  std::vector<BYTE> image = NtfsBrowserTests::BuildFakeNtfsImage();
  const size_t root_offset = RecordOffset(static_cast<ULONGLONG>(MftIdx::Root));
  size_t offset = root_offset + first_attr_offset;
  PutOddSizedAttr(image, offset, AttrType::Data);
  offset += odd_attr_size;
  PutOddSizedAttr(image, offset, AttrType::ReparsePoint);
  offset += odd_attr_size;
  Put(image, offset, static_cast<DWORD>(AttrType::All));

  NtfsVolume<S> const volume(
      std::make_unique<NtfsBrowserTests::MemoryDiskReader>(std::move(image)));
  REQUIRE(volume.IsVolumeOK());

  FileRecord<S> record(volume);
  REQUIRE(record.ParseFileRecord(static_cast<ULONGLONG>(MftIdx::Root)));
  REQUIRE(record.ParseAttrs());

  for (const AttrType type : {AttrType::Data, AttrType::ReparsePoint})
  {
    const auto& attrs = record.GetAttr(type);
    REQUIRE(attrs.size() == 1);
    const auto address =
        reinterpret_cast<std::uintptr_t>(&attrs.front()->GetAttrHeader());
    CHECK(address % alignof(NtfsBrowser::Attr::HeaderNonResident) == 0);
    CHECK(attrs.front()->GetAttrTotalSize() == odd_attr_size);
  }
}

// Number of entries TraverseSubEntries() reports for the root directory.
size_t CountRootEntries(std::vector<BYTE> image)
{
  NtfsVolume<Strategy::NoCache> const volume(
      std::make_unique<NtfsBrowserTests::MemoryDiskReader>(std::move(image)));
  REQUIRE(volume.IsVolumeOK());

  FileRecord<Strategy::NoCache> record(volume);
  REQUIRE(record.ParseFileRecord(static_cast<ULONGLONG>(MftIdx::Root)));
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
    Strategy::NoCache, Strategy::FullCache)
{
  RunOddSizedAttributesAreParsedAligned<S>();
}

TEMPLATE_TEST_CASE_SIG(
    "FindSubEntry reads an $INDEX_ROOT entry that starts off an 8-byte "
    "boundary",
    "[index-entry][alignment][regression]", ((Strategy S), S),
    Strategy::NoCache, Strategy::FullCache)
{
  constexpr BYTE shortened_name_length = 2;
  constexpr WORD shortened_stream_size =
      offsetof(NtfsBrowser::Attr::Filename, name) +
      (size_t{2} * shortened_name_length);
  constexpr WORD shortened_entry_size =
      offsetof(NtfsBrowser::Data::IndexEntry, stream) + shortened_stream_size;
  constexpr WORD terminator_size =
      offsetof(NtfsBrowser::Data::IndexEntry, stream);
  constexpr DWORD entries_size = shortened_entry_size + terminator_size;
  static_assert(shortened_entry_size % alignof(NtfsBrowser::Data::IndexEntry) !=
                0);

  std::vector<BYTE> image =
      NtfsBrowserTests::BuildFakeNtfsImageWithIndexRootVariants();
  const size_t attr_offset =
      RecordOffset(NtfsBrowserTests::index_root_variant_a_dir_idx) +
      first_attr_offset;
  const size_t root_offset =
      attr_offset + sizeof(NtfsBrowser::Attr::HeaderResident);
  const size_t entry_offset =
      root_offset + sizeof(NtfsBrowser::Attr::IndexRoot);

  // The fixture's one real entry is "AAA" (88 bytes), then the terminator.
  // Dropping a character moves the terminator 2 bytes down.
  constexpr size_t original_entry_size = 88;
  Put(image,
      entry_offset + offsetof(NtfsBrowser::Data::IndexEntry, stream) +
          offsetof(NtfsBrowser::Attr::Filename, name_length),
      shortened_name_length);
  Put(image,
      entry_offset + offsetof(NtfsBrowser::Data::IndexEntry, stream_size),
      shortened_stream_size);
  Put(image, entry_offset + offsetof(NtfsBrowser::Data::IndexEntry, size),
      shortened_entry_size);
  std::memmove(&image.at(entry_offset + shortened_entry_size),
               &image.at(entry_offset + original_entry_size), terminator_size);
  std::memset(&image.at(entry_offset + entries_size), 0, 2);

  Put(image,
      root_offset + offsetof(NtfsBrowser::Attr::IndexRoot, total_entry_size),
      entries_size);
  Put(image,
      root_offset + offsetof(NtfsBrowser::Attr::IndexRoot, alloc_entry_size),
      entries_size);
  const DWORD attr_size = sizeof(NtfsBrowser::Attr::IndexRoot) + entries_size;
  Put(image,
      attr_offset + offsetof(NtfsBrowser::Attr::HeaderResident, attr_size),
      attr_size);
  const DWORD total_size =
      sizeof(NtfsBrowser::Attr::HeaderResident) + attr_size;
  Put(image, attr_offset + offsetof(NtfsBrowser::AttrHeaderCommon, total_size),
      total_size);
  std::memset(&image.at(attr_offset + total_size), 0, 2 * sizeof(DWORD));
  Put(image, attr_offset + total_size, static_cast<DWORD>(AttrType::All));

  NtfsVolume<S> const volume(
      std::make_unique<NtfsBrowserTests::MemoryDiskReader>(std::move(image)));
  REQUIRE(volume.IsVolumeOK());

  FileRecord<S> record(volume);
  REQUIRE(
      record.ParseFileRecord(NtfsBrowserTests::index_root_variant_a_dir_idx));
  REQUIRE(record.ParseAttrs());

  const std::optional<IndexEntry> entry = record.FindSubEntry(L"AA");
  REQUIRE(entry.has_value());
  CHECK(NtfsBrowserTests::Unwrap(entry).GetFileReference() ==
        NtfsBrowserTests::index_root_variant_a_mft_ref);
}

TEMPLATE_TEST_CASE_SIG(
    "FileRecordHeader reads an Update Sequence Array that starts at an odd "
    "offset",
    "[file-record-header][alignment][regression]", ((Strategy S), S),
    Strategy::NoCache, Strategy::FullCache)
{
  constexpr size_t record_size = 1024;
  constexpr WORD odd_offset_of_us = 49;
  constexpr WORD usn = 0x1234;
  constexpr WORD first_block_word = 0xAAAA;
  constexpr WORD second_block_word = 0xBBBB;

  std::vector<BYTE> storage(record_size, 0);
  NtfsBrowserTests::EditFileRecordHeader(storage,
                                         [](FileRecordHeader::Data& header)
                                         {
                                           header.magic = file_record_magic;
                                           header.offset_of_us =
                                               odd_offset_of_us;
                                           header.size_of_us = 3;
                                         });
  Put(storage, odd_offset_of_us, usn);
  Put(storage, odd_offset_of_us + sizeof(WORD), first_block_word);
  Put(storage, odd_offset_of_us + (2 * sizeof(WORD)), second_block_word);

  const auto record = FileRecordHeaderImpl<S>(std::span<const BYTE>(storage));

  CHECK(record.us_number == usn);
  REQUIRE(record.us_array.size() == 2);
  // The REQUIRE above checks the size of record.us_array.
  // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
  CHECK(record.us_array[0] == first_block_word);
  // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
  CHECK(record.us_array[1] == second_block_word);
}

TEST_CASE(
    "TraverseSubEntries reads an index block whose Update Sequence Array "
    "starts at an odd offset",
    "[attr-index-alloc][alignment][regression]")
{
  constexpr WORD odd_offset_of_us = NtfsBrowserTests::fake_cluster_size - 7;

  std::vector<BYTE> image =
      NtfsBrowserTests::BuildFakeNtfsImageWithOrphanedIndexBlocks();
  const size_t expected = CountRootEntries(image);
  REQUIRE(expected > 0);

  // The first cluster-aligned index block is VCN 0, the one $INDEX_ROOT
  // points at. Its array is all zeros, so moving it one byte changes no
  // value.
  size_t block_offset = 0;
  for (; block_offset + sizeof(DWORD) <= image.size();
       block_offset += NtfsBrowserTests::fake_cluster_size)
  {
    DWORD magic = 0;
    std::memcpy(&magic, &image.at(block_offset), sizeof(magic));
    if (magic == index_block_magic)
    {
      break;
    }
  }
  REQUIRE(block_offset + NtfsBrowserTests::fake_cluster_size <= image.size());
  Put(image,
      block_offset + offsetof(NtfsBrowser::Data::IndexBlock, offset_of_us),
      odd_offset_of_us);

  const size_t actual = CountRootEntries(std::move(image));
  CHECK(actual == expected);
}
