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

// Regression fixture for bug F4: AttrIndexAlloc<S>::ParseIndexBlock()
// (src/attr-index-alloc.cpp) used to dereference
// "reinterpret_cast<const BYTE*>(ibBuf) + ibBuf->offset_of_us" with no check
// whatsoever that offset_of_us (an attacker-controlled WORD, straight off
// "disk") still lands inside the index_block_size-byte buffer
// AllocIndexBlock() just allocated.
//
// The fix (src/attr-index-alloc.cpp, src/attr-index-alloc.h) extracts the
// bounds check into IndexBlockUsOffsetInBounds(), a small, pure,
// non-templated function taking only the already-validated integers
// (offset_of_us, sectors, index_block_size) - no buffer, no allocation, no
// disk image. That makes it directly unit-testable: no need to force the
// vulnerable read into unmapped memory to prove it would have gone out of
// bounds, unlike an earlier version of this test which redirected
// ParseIndexBlock()'s internal make_shared<BYTE[]> allocation to a
// guard-paged VirtualAlloc region via a process-wide operator new override
// and caught the resulting STATUS_ACCESS_VIOLATION under SEH. That approach
// silently proved nothing whenever NtfsBrowser was built as a shared library
// (BUILD_SHARED_LIBS=ON, one of the CI matrix's own configurations): the
// override lives in the test executable, and allocations made from inside
// AttrIndexAlloc<S>::ParseIndexBlock() - compiled into NtfsBrowser.dll -
// never reach it, since global operator new/delete overrides don't cross
// Windows DLL boundaries. Testing the extracted function directly sidesteps
// the whole problem.
TEST_CASE(
    "IndexBlockUsOffsetInBounds rejects an offset_of_us that would run the "
    "Update Sequence Array past the index block buffer (F4)",
    "[attr-index-alloc][regression]")
{
  // Matches BuildFakeNtfsImageWithForgedIndexBlock()'s fixture: a 7-sector,
  // 7168-byte index block (kForgedIndexBlockSize / sector size, both 1024).
  constexpr DWORD kIndexBlockSize = NtfsBrowserTests::kForgedIndexBlockSize;
  constexpr DWORD kSectors = kIndexBlockSize / 1024;

  // The bug report's own scenario: offset_of_us = 0xFFFF is far past the
  // buffer.
  CHECK_FALSE(
      IndexBlockUsOffsetInBounds(NtfsBrowserTests::kForgedIndexBlockOffsetOfUs,
                                 kSectors, kIndexBlockSize));

  // A well-formed offset (right after the Data::IndexBlock header, with room
  // for the USN plus one WORD per sector) must be accepted.
  constexpr WORD kValidOffset = static_cast<WORD>(sizeof(IndexBlock));
  CHECK(IndexBlockUsOffsetInBounds(kValidOffset, kSectors, kIndexBlockSize));

  // Landing inside the Data::IndexBlock header itself must be rejected, even
  // though it technically stays inside the buffer - PatchUS() would
  // otherwise be free to overwrite header fields with sector data before
  // they are read.
  CHECK_FALSE(IndexBlockUsOffsetInBounds(
      static_cast<WORD>(sizeof(IndexBlock) - 1), kSectors, kIndexBlockSize));

  // Boundary: offset_of_us + the USN + the per-sector array exactly fills
  // the buffer -> must be accepted.
  constexpr WORD kExactFitOffset =
      static_cast<WORD>(kIndexBlockSize - 2 * (1 + kSectors));
  CHECK(IndexBlockUsOffsetInBounds(kExactFitOffset, kSectors, kIndexBlockSize));

  // One byte further and the array's last WORD spills past the buffer's end
  // -> must be rejected.
  CHECK_FALSE(IndexBlockUsOffsetInBounds(kExactFitOffset + 1, kSectors,
                                         kIndexBlockSize));
}

// End-to-end smoke test: traverses a real forged directory whose sole index
// block declares offset_of_us = 0xFFFF, and checks that the whole parse
// completes normally instead of crashing or throwing. With the fix, this is
// deterministic regardless of heap layout or allocator - the malformed block
// is rejected by IndexBlockUsOffsetInBounds() before anything is
// dereferenced through it, in exactly the same call this exercises.
TEST_CASE(
    "FileRecord::TraverseSubEntries must not crash when an index block's "
    "offset_of_us is out of bounds (F4)",
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

  // The forged directory's only entries live behind the rejected index
  // block, so none of them should ever reach the callback.
  CHECK(callbackCount == 0);
}
