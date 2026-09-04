#include <catch2/catch_test_macros.hpp>

#include <ntfs-browser/file-record.h>
#include <ntfs-browser/index-entry.h>
#include <ntfs-browser/ntfs-volume.h>
#include <ntfs-browser/strategy.h>

#include "attr-index-alloc.h"
#include "data/index-block.h"
#include "fake-ntfs-image.h"
#include "memory-disk-reader.h"

using NtfsBrowser::FileRecord;
using NtfsBrowser::IndexBlockUsOffsetInBounds;
using NtfsBrowser::IndexEntry;
using NtfsBrowser::NtfsVolume;
using NtfsBrowser::Strategy;
using NtfsBrowser::Data::IndexBlock;

TEST_CASE(
    "IndexBlockUsOffsetInBounds rejects an offset_of_us that would run the "
    "Update Sequence Array past the index block buffer",
    "[attr-index-alloc][regression]")
{
  constexpr DWORD kIndexBlockSize = NtfsBrowserTests::kForgedIndexBlockSize;
  constexpr DWORD kSectors = kIndexBlockSize / 1024;

  CHECK_FALSE(
      IndexBlockUsOffsetInBounds(NtfsBrowserTests::kForgedIndexBlockOffsetOfUs,
                                 kSectors, kIndexBlockSize));

  // Right after the header, with room for the whole array: accepted.
  constexpr WORD kValidOffset = static_cast<WORD>(sizeof(IndexBlock));
  CHECK(IndexBlockUsOffsetInBounds(kValidOffset, kSectors, kIndexBlockSize));

  // Inside the header, though still within the buffer: rejected.
  CHECK_FALSE(IndexBlockUsOffsetInBounds(
      static_cast<WORD>(sizeof(IndexBlock) - 1), kSectors, kIndexBlockSize));

  // Exactly fills the buffer: accepted.
  constexpr WORD kExactFitOffset =
      static_cast<WORD>(kIndexBlockSize - 2 * (1 + kSectors));
  CHECK(IndexBlockUsOffsetInBounds(kExactFitOffset, kSectors, kIndexBlockSize));

  // One byte more spills past the buffer: rejected.
  CHECK_FALSE(IndexBlockUsOffsetInBounds(kExactFitOffset + 1, kSectors,
                                         kIndexBlockSize));
}

TEST_CASE(
    "FileRecord::TraverseSubEntries must not crash when an index block's "
    "offset_of_us is out of bounds",
    "[attr-index-alloc][regression]")
{
  auto reader = std::make_unique<NtfsBrowserTests::MemoryDiskReader>(
      NtfsBrowserTests::BuildFakeNtfsImageWithForgedIndexBlock());

  NtfsVolume<Strategy::NO_CACHE> volume(std::move(reader));
  REQUIRE(volume.IsVolumeOK());

  FileRecord<Strategy::NO_CACHE> record(volume);
  REQUIRE(record.ParseFileRecord(NtfsBrowserTests::kIndexAllocDirIdx));
  REQUIRE(record.ParseAttrs());

  int callbackCount = 0;
  record.TraverseSubEntries([](const IndexEntry&, void* context)
                            { ++*static_cast<int*>(context); }, &callbackCount);

  // Entries live behind the rejected block: the callback must never run.
  CHECK(callbackCount == 0);
}
