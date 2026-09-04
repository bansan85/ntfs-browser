#include <catch2/catch_test_macros.hpp>

#include <ntfs-browser/ntfs-volume.h>
#include <ntfs-browser/strategy.h>

#include "fake-ntfs-image.h"
#include "memory-disk-reader.h"

using NtfsBrowser::NtfsVolume;
using NtfsBrowser::Strategy;

// Regression fixture for bug F6: NtfsVolume<S>::ParseBootSector()
// (src/ntfs-volume.cpp) derives index_block_size_ straight from the BPB's
// clusters_per_index_block field - fully attacker-controlled on a
// forged/corrupted volume - and never checks the result against
// sizeof(Data::IndexBlock) (40 bytes), the structure
// AttrIndexAlloc<S>::ParseIndexBlock() (src/attr-index-alloc.cpp) is about to
// allocate and interpret every index block through. With
// clusters_per_index_block = 0xFF (read as a signed char: -1),
// index_block_size_ ends up 1 << 1 = 2 bytes: AllocIndexBlock(2) then
// allocates a 2-byte buffer that ibBuf->magic alone (a DWORD, 4 bytes)
// already overruns by 2 bytes, before any of F4's own fixes even come into
// play.
//
// file_record_size_ has the exact same missing check in ParseBootSector(),
// but happens to be caught downstream, by accident, by FileRecordHeader's
// ctor hard-requiring buffer.size() == 1024 (src/data/file-record-header.cpp)
// - so it doesn't yet manifest as an observable bug on its own, and this
// test does not attempt to demonstrate it.
//
// Rather than building a full $INDEX_ALLOCATION traversal to observe the
// resulting out-of-bounds read directly (see index-block-fixup-tests.cpp for
// that style, F4's regression test), this test takes the simpler route the
// bug report itself suggests: it shows that ParseBootSector() currently
// accepts (IsVolumeOK() == true) a volume whose declared index_block_size_ is
// nowhere near large enough to hold Data::IndexBlock's own header - proof
// that nothing validates it, independent of any particular downstream
// consumer.
TEST_CASE(
    "NtfsVolume must not accept a volume whose BPB describes an index block "
    "far smaller than Data::IndexBlock (F6)",
    "[ntfs-volume][regression]")
{
  auto reader = std::make_unique<NtfsBrowserTests::MemoryDiskReader>(
      NtfsBrowserTests::BuildFakeNtfsImageWithTinyIndexBlock());

  NtfsVolume<Strategy::NO_CACHE> volume(std::move(reader));

  INFO("GetIndexBlockSize() = " << volume.GetIndexBlockSize());
  CHECK(volume.GetIndexBlockSize() == NtfsBrowserTests::kTinyIndexBlockSize);

  // Before the F6 fix: ParseBootSector() happily accepts index_block_size_
  // == 2 (kTinyIndexBlockSize), nowhere near sizeof(Data::IndexBlock) == 40,
  // and the volume is reported OK. After the fix, a value this small must be
  // rejected outright.
  CHECK_FALSE(volume.IsVolumeOK());
}
