#include <memory>

#include <catch2/catch_test_macros.hpp>

#include <ntfs-browser/data/attr-type.h>
#include <ntfs-browser/file-record.h>
#include <ntfs-browser/mft-idx.h>
#include <ntfs-browser/ntfs-volume.h>
#include <ntfs-browser/strategy.h>

#include "fake-ntfs-image.h"
#include "memory-disk-reader.h"

using NtfsBrowser::AttrType;
using NtfsBrowser::FileRecord;
using NtfsBrowser::NtfsVolume;
using NtfsBrowser::Strategy;
using NtfsBrowser::Enum::MftIdx;

// Regression test for bug F1: FileRecord::ParseAttrs() only ever checked
// that an attribute's total_size fits within the 1024-byte record buffer
// (dataPtr + total_size <= 1024). It never checked total_size against
// sizeof(Attr::HeaderResident) (24) / sizeof(Attr::HeaderNonResident) (64)
// before calling ParseAttr(), which constructs AttrResident{No,Full}Cache /
// AttrNonResident - both of which reinterpret_cast the raw attribute bytes
// to those structs and read fields (attr_size/attr_offset, or
// start_vcn/last_vcn/data_run_offset/real_size) unconditionally. An
// attribute whose total_size is smaller than its own header type therefore
// gets its "header" fields read from bytes that lie past its declared
// extent - in general (see docs/bug-reports/2026-09-03-full-repo.md, F1)
// this can run past the end of the 1024-byte record buffer entirely; here
// BuildFakeNtfsImageWithUndersizedAttribute() places the attribute well
// inside the buffer so the wrong behavior is deterministic and doesn't
// depend on what happens to be in adjacent heap memory.
//
// Before the fix, ParseAttrs() wrongly accepts the 17-byte REPARSE_POINT
// attribute (attr_size/attr_offset read as 0 from the zeroed bytes past
// total_size) instead of rejecting a record no valid NTFS parser could
// produce. After the fix, the too-small total_size is caught before
// ParseAttr() ever runs, and ParseAttrs() rejects the whole record.
TEST_CASE(
    "ParseAttrs rejects a resident attribute whose total_size is smaller "
    "than its header (F1)",
    "[file-record][regression]")
{
  auto reader = std::make_unique<NtfsBrowserTests::MemoryDiskReader>(
      NtfsBrowserTests::BuildFakeNtfsImageWithUndersizedAttribute());

  NtfsVolume<Strategy::NO_CACHE> volume(std::move(reader));
  REQUIRE(volume.IsVolumeOK());

  FileRecord<Strategy::NO_CACHE> record(volume);
  REQUIRE(record.ParseFileRecord(NtfsBrowserTests::kUndersizedAttrRecordIdx));

  CHECK_FALSE(record.ParseAttrs());
  CHECK(record.getAttr(AttrType::REPARSE_POINT).empty());
}

// Same fixture under FULL_CACHE: AttrResidentFullCache's ctor runs the same
// ValidateResidentBounds() + attr_size/attr_offset read as the NO_CACHE
// variant above, just against a copy of the record instead of a view into
// it - confirms the defect (and its fix) isn't an artifact of one strategy.
TEST_CASE(
    "ParseAttrs rejects a resident attribute whose total_size is smaller "
    "than its header (F1, FULL_CACHE)",
    "[file-record][regression]")
{
  auto reader = std::make_unique<NtfsBrowserTests::MemoryDiskReader>(
      NtfsBrowserTests::BuildFakeNtfsImageWithUndersizedAttribute());

  NtfsVolume<Strategy::FULL_CACHE> volume(std::move(reader));
  REQUIRE(volume.IsVolumeOK());

  FileRecord<Strategy::FULL_CACHE> record(volume);
  REQUIRE(record.ParseFileRecord(NtfsBrowserTests::kUndersizedAttrRecordIdx));

  CHECK_FALSE(record.ParseAttrs());
  CHECK(record.getAttr(AttrType::REPARSE_POINT).empty());
}

// Regression test for the HeaderCommon() half of bug F9:
// FileRecordHeader::HeaderCommon() (src/data/file-record-header.cpp) locates
// a record's first attribute via offset_of_attr, bounded against this
// instance's own buffer_size_ - not just the union's static capacity (see
// file-record-header-size-tests.cpp for a test targeting HeaderCommon()
// directly). FileRecord::ParseAttrs() calls HeaderCommon() to find that first
// attribute, so a forged offset_of_attr pointing well past the root
// directory's real, exactly-1024-byte extent must make ParseAttrs() reject
// the whole record instead of reading past it.
TEST_CASE(
    "ParseAttrs rejects a record whose offset_of_attr exceeds its own file "
    "record size (F9)",
    "[file-record][regression]")
{
  auto reader = std::make_unique<NtfsBrowserTests::MemoryDiskReader>(
      NtfsBrowserTests::BuildFakeNtfsImageWithAttrOffsetOutOfBounds());

  NtfsVolume<Strategy::NO_CACHE> volume(std::move(reader));
  REQUIRE(volume.IsVolumeOK());

  FileRecord<Strategy::NO_CACHE> record(volume);
  REQUIRE(record.ParseFileRecord(static_cast<ULONGLONG>(MftIdx::ROOT)));

  CHECK_FALSE(record.ParseAttrs());
}
