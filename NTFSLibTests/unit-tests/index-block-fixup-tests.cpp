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

using NtfsBrowser::FileRecord;
using NtfsBrowser::IndexEntryView;
using NtfsBrowser::NtfsVolume;
using NtfsBrowser::Strategy;
using NtfsBrowser::Data::index_block_magic;
using NtfsBrowser::Data::IndexBlock;
using NtfsBrowser::Data::IndexBlockUsOffsetInBounds;

TEST_CASE(
    "IndexBlockUsOffsetInBounds rejects an offset_of_us that would run the "
    "Update Sequence Array past the index block buffer",
    "[attr-index-alloc][regression]") {
  constexpr DWORD index_block_size = NtfsBrowserTests::forged_index_block_size;
  constexpr DWORD sectors =
      index_block_size / NtfsBrowser::FileRecordHeader::update_sequence_stride;

  CHECK_FALSE(IndexBlockUsOffsetInBounds(
      NtfsBrowserTests::forged_index_block_offset_of_us, sectors,
      index_block_size));

  // Right after the header, with room for the whole array: accepted.
  constexpr WORD valid_offset = static_cast<WORD>(sizeof(IndexBlock));
  CHECK(IndexBlockUsOffsetInBounds(valid_offset, sectors, index_block_size));

  // Inside the header, though still within the buffer: rejected.
  CHECK_FALSE(IndexBlockUsOffsetInBounds(
      static_cast<WORD>(sizeof(IndexBlock) - 1), sectors, index_block_size));

  // Exactly fills the buffer: accepted.
  constexpr WORD exact_fit_offset =
      static_cast<WORD>(index_block_size - 2 * (1 + sectors));
  CHECK(
      IndexBlockUsOffsetInBounds(exact_fit_offset, sectors, index_block_size));

  // One byte more spills past the buffer: rejected.
  CHECK_FALSE(IndexBlockUsOffsetInBounds(exact_fit_offset + 1, sectors,
                                         index_block_size));
}

TEMPLATE_TEST_CASE_SIG(
    "FileRecord::TraverseSubEntries must not crash when an index block's "
    "offset_of_us is out of bounds",
    "[attr-index-alloc][regression]", ((Strategy S), S), Strategy::NoCache,
    Strategy::FullCache) {
  auto reader = std::make_unique<NtfsBrowserTests::MemoryDiskReader>(
      NtfsBrowserTests::BuildFakeNtfsImageWithForgedIndexBlock());

  const NtfsVolume<S> volume(std::move(reader));
  REQUIRE(volume.IsVolumeOK());

  FileRecord<S> record(volume);
  REQUIRE(record.ParseFileRecord(NtfsBrowserTests::index_alloc_dir_idx));
  REQUIRE(record.ParseAttrs());

  int callback_count = 0;
  record.TraverseSubEntries(
      [](const IndexEntryView&, void* context) {
        ++*static_cast<int*>(context);
      },
      &callback_count);

  // Entries live behind the rejected block: the callback must never run.
  CHECK(callback_count == 0);
}

TEMPLATE_TEST_CASE_SIG(
    "FileRecord::TraverseSubEntries must reject an index block whose first "
    "512-byte block does not end with the update sequence number",
    "[attr-index-alloc][regression]", ((Strategy S), S), Strategy::NoCache,
    Strategy::FullCache) {
  constexpr size_t us_block_size = 512;
  constexpr WORD torn_word = 0xDEAD;

  std::vector<BYTE> image =
      NtfsBrowserTests::BuildFakeNtfsImageWithOrphanedIndexBlocks();

  // The first cluster-aligned index block is VCN 0, the one $INDEX_ROOT
  // points at.
  size_t block_offset = 0;
  for (; block_offset + sizeof(DWORD) <= image.size();
       block_offset += NtfsBrowserTests::fake_cluster_size) {
    DWORD magic = 0;
    std::memcpy(&magic, &image.at(block_offset), sizeof(magic));
    if (magic == index_block_magic) {
      break;
    }
  }
  REQUIRE(block_offset + NtfsBrowserTests::fake_cluster_size <= image.size());

  // A torn write: the end of the block's first 512 bytes was never given
  // the sequence number.
  std::memcpy(&image.at(block_offset + us_block_size - sizeof(WORD)),
              &torn_word, sizeof(torn_word));

  auto reader =
      std::make_unique<NtfsBrowserTests::MemoryDiskReader>(std::move(image));
  const NtfsVolume<S> volume(std::move(reader));
  REQUIRE(volume.IsVolumeOK());

  FileRecord<S> record(volume);
  REQUIRE(record.ParseFileRecord(
      static_cast<ULONGLONG>(NtfsBrowser::Enum::MftIdx::Root)));
  REQUIRE(record.ParseAttrs());

  int callback_count = 0;
  record.TraverseSubEntries(
      [](const IndexEntryView&, void* context) {
        ++*static_cast<int*>(context);
      },
      &callback_count);

  CHECK(callback_count == 0);
}
