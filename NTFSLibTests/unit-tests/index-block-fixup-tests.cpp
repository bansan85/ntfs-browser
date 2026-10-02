#include <ntfs-browser/win-types.h>

#include <cstddef>
#include <cstring>
#include <memory>
#include <utility>
#include <vector>

#include <catch2/catch_template_test_macros.hpp>
#include <catch2/catch_test_macros.hpp>

#include <ntfs-browser/disk-reader.h>
#include <ntfs-browser/file-record.h>
#include <ntfs-browser/mft-idx.h>
#include <ntfs-browser/ntfs-volume.h>
#include <ntfs-browser/strategy.h>

#include "attr-index-alloc.h"
#include "data/file-record-header.h"
#include "data/index-block.h"
#include "fake-ntfs-image.h"
#include "memory-disk-reader.h"

namespace NtfsBrowser
{
class IndexEntry;
}  // namespace NtfsBrowser

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
  constexpr DWORD kSectors =
      kIndexBlockSize / NtfsBrowser::kUpdateSequenceStride;

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

TEMPLATE_TEST_CASE_SIG(
    "FileRecord::TraverseSubEntries must not crash when an index block's "
    "offset_of_us is out of bounds",
    "[attr-index-alloc][regression]", ((Strategy S), S), Strategy::NO_CACHE,
    Strategy::FULL_CACHE)
{
  auto reader = std::make_unique<NtfsBrowserTests::MemoryDiskReader>(
      NtfsBrowserTests::BuildFakeNtfsImageWithForgedIndexBlock());

  NtfsVolume<S> const volume(std::move(reader));
  REQUIRE(volume.IsVolumeOK());

  FileRecord<S> record(volume);
  REQUIRE(record.ParseFileRecord(NtfsBrowserTests::kIndexAllocDirIdx));
  REQUIRE(record.ParseAttrs());

  int callbackCount = 0;
  record.TraverseSubEntries([](const IndexEntry&, void* context)
                            { ++*static_cast<int*>(context); }, &callbackCount);

  // Entries live behind the rejected block: the callback must never run.
  CHECK(callbackCount == 0);
}

TEMPLATE_TEST_CASE_SIG(
    "FileRecord::TraverseSubEntries must reject an index block whose first "
    "512-byte block does not end with the update sequence number",
    "[attr-index-alloc][regression]", ((Strategy S), S), Strategy::NO_CACHE,
    Strategy::FULL_CACHE)
{
  constexpr size_t kUsBlockSize = 512;
  constexpr WORD kTornWord = 0xDEAD;

  std::vector<BYTE> image =
      NtfsBrowserTests::BuildFakeNtfsImageWithOrphanedIndexBlocks();

  // The first cluster-aligned index block is VCN 0, the one $INDEX_ROOT
  // points at.
  size_t blockOffset = 0;
  for (; blockOffset + sizeof(DWORD) <= image.size();
       blockOffset += NtfsBrowserTests::kFakeClusterSize)
  {
    DWORD magic = 0;
    std::memcpy(&magic, image.data() + blockOffset, sizeof(magic));
    if (magic == kIndexBlockMagic)
    {
      break;
    }
  }
  REQUIRE(blockOffset + NtfsBrowserTests::kFakeClusterSize <= image.size());

  // A torn write: the end of the block's first 512 bytes was never given
  // the sequence number.
  std::memcpy(image.data() + blockOffset + kUsBlockSize - sizeof(WORD),
              &kTornWord, sizeof(kTornWord));

  auto reader =
      std::make_unique<NtfsBrowserTests::MemoryDiskReader>(std::move(image));
  NtfsVolume<S> const volume(std::move(reader));
  REQUIRE(volume.IsVolumeOK());

  FileRecord<S> record(volume);
  REQUIRE(record.ParseFileRecord(
      static_cast<ULONGLONG>(NtfsBrowser::Enum::MftIdx::ROOT)));
  REQUIRE(record.ParseAttrs());

  int callbackCount = 0;
  record.TraverseSubEntries([](const IndexEntry&, void* context)
                            { ++*static_cast<int*>(context); }, &callbackCount);

  CHECK(callbackCount == 0);
}
