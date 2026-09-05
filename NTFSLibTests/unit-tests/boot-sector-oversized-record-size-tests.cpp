#include <catch2/catch_test_macros.hpp>

#include <ntfs-browser/ntfs-volume.h>
#include <ntfs-browser/strategy.h>

#include "fake-ntfs-image.h"
#include "memory-disk-reader.h"

using NtfsBrowser::NtfsVolume;
using NtfsBrowser::Strategy;

// Regression fixture for bug F5: NtfsVolume<S>::ParseBootSector()
// (src/ntfs-volume.cpp) derives index_block_size_ (and file_record_size_,
// the exact same way) from a BPB byte read as a signed char (sz); for sz <=
// 0 it computes size = 1U << static_cast<unsigned char>(-sz), with no bound
// on sz at all. clusters_per_index_block = 0xE1 gives sz = -31: the shift
// itself is well-defined (31 < 32, unlike eg. sz = -128, which really is
// UB), so this is not the UB half of F5 - but the result, 1U << 31 ==
// 0x80000000 (2 GiB), is exactly the "well-defined but absurd" half F6's own
// fix (checking the *result* against sizeof(Data::IndexBlock) and against
// sector_size_) does not catch: 0x80000000 is far bigger than 40 bytes and
// evenly divisible by every common sector size. See F5 in
// docs/bug-reports/2026-09-03-full-repo.md.
//
// Same reasoning, same style as boot-sector-index-block-size-tests.cpp's F6
// test: stay at the ParseBootSector()/accessor level, never touch
// FileRecord directly, and let CHECK_FALSE(IsVolumeOK()) be the actual
// red/green assertion (GetIndexBlockSize() itself doesn't change across the
// fix - only whether ParseBootSector() goes on to accept the volume).
TEST_CASE(
    "NtfsVolume must not accept a volume whose BPB describes an index block "
    "far larger than any plausible size (F5)",
    "[ntfs-volume][regression]")
{
  auto reader = std::make_unique<NtfsBrowserTests::MemoryDiskReader>(
      NtfsBrowserTests::BuildFakeNtfsImageWithOversizedIndexBlock());

  NtfsVolume<Strategy::NO_CACHE> volume(std::move(reader));

  // GetIndexBlockSize() itself is deliberately not asserted against
  // kOversizedIndexBlockSize here (unlike F6's own test, where the fix only
  // ever checks the *result* after it's already assigned, so the value is
  // stable across the fix): F5's fix bounds sz *before* the shift/multiply
  // even runs, so a rejected sz never reaches the assignment at all, and
  // index_block_size_ instead stays at its default (0) post-fix. Logged
  // purely for diagnostics.
  INFO("GetIndexBlockSize() = " << volume.GetIndexBlockSize());

  // Before the F5 fix: ParseBootSector() happily accepts index_block_size_
  // == 0x80000000 (2 GiB) - F6's own checks only reject values that are too
  // *small*, not implausibly large - and the volume is reported OK. After
  // the fix, sz itself is bounded before the shift ever runs, so a magnitude
  // this large must be rejected outright.
  CHECK_FALSE(volume.IsVolumeOK());
}

// Same defect, same fixed 0xE1 -> sz = -31 -> 1U << 31 == 0x80000000 shift,
// but on clusters_per_file_record / file_record_size_ instead of
// clusters_per_index_block / index_block_size_ - the exact same few lines of
// ParseBootSector(), duplicated for the other field.
//
// Unlike the index-block case above, IsVolumeOK() itself can't be the
// red/green assertion here: NtfsVolume<S>::OpenVolume() only calls Init() (
// which parses $Volume, then $MFT) once ParseBootSector() has already
// returned true, and FileRecord<S>::ReadFileRecord() immediately does
// record_buffer_.resize(volume_.GetFileRecordSize()) for *any* record,
// $Volume included. Before the F5 fix, ParseBootSector() accepts
// file_record_size_ == 0x80000000 outright, so Init() *does* run and
// performs that resize(0x80000000) - verified directly (not assumed): a
// real ~2 GiB allocation, ~0.9s wall clock on this machine - before merely
// failing to read $Volume's actual content off the tiny backing image
// ("Cannot read file record 3"). So IsVolumeOK() ends up false either way -
// pre-fix because of that unrelated downstream read failure, post-fix
// because ParseBootSector() itself now rejects the size before Init() is
// ever even called - and checking it here would prove nothing.
//
// GetFileRecordSize() does distinguish the two: ParseBootSector() sets it
// BEFORE Init() (and any of its downstream failures) ever runs. Before the
// fix it holds the accepted 0x80000000. After the fix, sz is bounded before
// the shift that would produce it ever executes, so ParseBootSector()
// returns false, OpenVolume() short-circuits, Init() is never called, and
// file_record_size_ stays at its default (0) - so this check is genuinely
// red before the fix and green after it, unlike a check on IsVolumeOK()
// here would be.
TEST_CASE(
    "NtfsVolume must not accept a volume whose BPB describes a file record "
    "far larger than any plausible size (F5)",
    "[ntfs-volume][regression]")
{
  auto reader = std::make_unique<NtfsBrowserTests::MemoryDiskReader>(
      NtfsBrowserTests::BuildFakeNtfsImageWithOversizedFileRecord());

  NtfsVolume<Strategy::NO_CACHE> volume(std::move(reader));

  INFO("GetFileRecordSize() = " << volume.GetFileRecordSize());
  CHECK(volume.GetFileRecordSize() !=
        NtfsBrowserTests::kOversizedFileRecordSize);
}

// Same F9 upper-bound rejection (file_record_size_ > kMaxFileRecordSize) as
// the test above, but reached via the OTHER arithmetic branch:
// clusters_per_file_record = kFileRecordSizeTooBigClustersPerFileRecord (8,
// sz > 0) gives file_record_size_ = cluster_size_ * 8 = 8192 - legal under
// every check that predates F9 (a whole number of sectors, an sz magnitude
// within F5's own [-12, 8] bound) but still twice
// FileRecordHeader::kMaxFileRecordSize (4096). Unlike the shift-based case
// above, the multiply itself is never UB, so F5's own fix has no reason to
// bound sz before computing it here - file_record_size_ is actually assigned
// before F9's check rejects it, so GetFileRecordSize() legitimately still
// holds kFileRecordSizeTooBig afterwards. IsVolumeOK() is the meaningful
// assertion instead: ParseBootSector() returns false before Init() (and any
// downstream file-record read) ever runs. See F9 in
// docs/bug-reports/2026-09-03-full-repo.md.
TEST_CASE(
    "NtfsVolume must not accept a volume whose BPB describes a file record "
    "size that exceeds kMaxFileRecordSize via the positive "
    "clusters_per_file_record branch (F9)",
    "[ntfs-volume][regression]")
{
  auto reader = std::make_unique<NtfsBrowserTests::MemoryDiskReader>(
      NtfsBrowserTests::BuildFakeNtfsImageWithFileRecordSizeTooBig());

  NtfsVolume<Strategy::NO_CACHE> volume(std::move(reader));

  CHECK(volume.GetFileRecordSize() == NtfsBrowserTests::kFileRecordSizeTooBig);
  CHECK_FALSE(volume.IsVolumeOK());
}
