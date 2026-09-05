#include <memory>

#include <catch2/catch_test_macros.hpp>

#include <ntfs-browser/file-record.h>
#include <ntfs-browser/mft-idx.h>
#include <ntfs-browser/ntfs-volume.h>
#include <ntfs-browser/strategy.h>

#include "fake-ntfs-image.h"
#include "memory-disk-reader.h"

using NtfsBrowser::FileRecord;
using NtfsBrowser::NtfsVolume;
using NtfsBrowser::Strategy;

namespace
{
static_assert(NtfsBrowserTests::kFragmentedMftInvalidRecordIdx ==
                  static_cast<ULONGLONG>(NtfsBrowser::Enum::MftIdx::USER),
              "this fixture's whole point is to be reached through "
              "FileRecord<S>::ReadFileRecord()'s \"fragmented $MFT\" branch "
              "(fileRef >= Enum::MftIdx::USER), not the direct-allocation "
              "one - see fake-ntfs-image.h");
}  // namespace

// F16 (docs/bug-reports/2026-09-03-full-repo.md): on the "fragmented $MFT"
// path (fileRef >= Enum::MftIdx::USER and volume_.mft_data_ != nullptr),
// FileRecord<S>::ReadFileRecord() (src/file-record.cpp) calls
// FileRecordHeader::Factory<S>() to parse the just-read record buffer. The
// bug report's own writeup found that call BARE on this specific branch -
// unlike the identical call on the "direct disk allocation" branch just
// above it, already wrapped in try/catch since before F8 - so an exception
// thrown from inside the ctor (eg. an out-of-bounds offset_of_us) would
// propagate all the way out of FileRecord::ParseFileRecord(), breaking the
// API's documented bool-returning, non-throwing contract for any caller
// without its own try (NTFSLibTests/ntfsdir/main.cpp,
// NTFSLibTests/ntfsundel/ntfsundelDlg.cpp).
//
// By the time this test was written, the fix was already in place - as an
// INCIDENTAL side effect of commit b7681a1 ("fix: validate mft_addr_ upfront
// and catch std::exception around gsl::narrow calls (F8)"), whose own
// message says so explicitly: "This fragmented-$MFT path ... reaches the
// very same FileRecordHeader::Factory<S>() call as the direct-allocation
// path above, which does guard it - noticed while widening that catch for
// F8, and fixed here too for the same reason." src/file-record.cpp's
// ReadFileRecord() now wraps BOTH FileRecordHeader::Factory<S>() calls in
// their own try/catch (const std::exception&), each returning {} instead of
// letting the exception escape.
//
// What was still missing is test coverage of that specific branch. The
// existing invalid_offset_of_us fuzz corpus entry (see
// fuzzer-regression-tests.cpp) only ever reaches this same ctor throw via
// the direct-allocation path (record 0 or 5) - a branch that was already
// protected even before F8/F16. This repo does separately carry a fuzz
// corpus entry that DOES exercise the fragmented-$MFT branch specifically -
// NTFSLibTests/fuzz/data/fragmented_record_header_factory_throw, added
// alongside F8 itself (commit 5f02927, "test: add AFL regression corpus for
// F8, F14, F17, F19 and N3") and replayed via kExpectedErrorMessages in
// fuzzer-regression-tests.cpp, checking for both "Offset must be lower than
// 1024." and "Attribute Parse error: 0x0020" (that fixture reaches record
// #16 through $ATTRIBUTE_LIST -> AttrList<S>::AttrList() ->
// FileRecord::ParseFileRecord(), one level removed from
// FileRecord::ReadFileRecord() itself). That entry is the reason no new fuzz
// corpus file is added here: F16's fragmented-$MFT branch already has fuzz
// coverage, just not a source-level Catch2 fixture calling
// FileRecord<S>::ParseFileRecord() directly.
//
// This test fills that gap directly and minimally: BuildFakeNtfsImageWith
// FragmentedMftInvalidRecord() (fake-ntfs-image.h/.cpp) gives $MFT's own
// DATA attribute a genuine (non-empty) data run reaching
// kFragmentedMftInvalidRecordIdx (16, Enum::MftIdx::USER) clusters - unlike
// every fixture above it, which relies on $MFT's data run being empty and
// immediately terminated - and forges the file record living at that offset
// with valid magic but an invalid offset_of_us. Calling
// FileRecord<S>::ParseFileRecord(16) directly - not through
// $ATTRIBUTE_LIST/AttrList, which would also work but exercises an extra,
// unrelated layer - drives fileRef (16) >= Enum::MftIdx::USER with
// volume.mft_data_ already assigned (IsVolumeOK() == true), landing
// unambiguously in the fragmented-$MFT branch under test, not the
// direct-allocation one.
//
// Sanity-checked (per this repo's own discipline of not trusting a test that
// has never been seen to fail for the right reason): temporarily commenting
// out the fragmented path's own try/catch in src/file-record.cpp (restoring
// the pre-b7681a1 bare call) makes REQUIRE_NOTHROW below fail - the
// FileRecordHeader ctor's std::runtime_error escapes ReadFileRecord(),
// ParseFileRecord() and this TEST_CASE uncaught, aborting the test process -
// confirming this test exercises the exact branch F16 is about, not a
// tautology. That temporary change was reverted before committing; it must
// never land alongside this test.
TEST_CASE(
    "FileRecord::ParseFileRecord() must not let an exception escape on the "
    "fragmented-$MFT path when the forged record has an invalid "
    "offset_of_us (F16)",
    "[file-record][regression]")
{
  auto reader = std::make_unique<NtfsBrowserTests::MemoryDiskReader>(
      NtfsBrowserTests::BuildFakeNtfsImageWithFragmentedMftInvalidRecord());

  NtfsVolume<Strategy::NO_CACHE> volume(std::move(reader));
  REQUIRE(volume.IsVolumeOK());

  FileRecord<Strategy::NO_CACHE> record(volume);

  bool parsed = true;
  REQUIRE_NOTHROW(parsed = record.ParseFileRecord(
                      NtfsBrowserTests::kFragmentedMftInvalidRecordIdx));
  CHECK_FALSE(parsed);
}

// Same fixture under FULL_CACHE - AttrNonResident<S>::ReadData() (which
// FileRecord<S>::ReadFileRecord()'s fragmented-$MFT path calls through
// volume_.mft_data_) runs identically regardless of strategy, but this
// confirms the defect (and its fix) isn't an artifact of one strategy, same
// as the FULL_CACHE variants elsewhere in this test suite.
TEST_CASE(
    "FileRecord::ParseFileRecord() must not let an exception escape on the "
    "fragmented-$MFT path when the forged record has an invalid "
    "offset_of_us (F16, FULL_CACHE)",
    "[file-record][regression]")
{
  auto reader = std::make_unique<NtfsBrowserTests::MemoryDiskReader>(
      NtfsBrowserTests::BuildFakeNtfsImageWithFragmentedMftInvalidRecord());

  NtfsVolume<Strategy::FULL_CACHE> volume(std::move(reader));
  REQUIRE(volume.IsVolumeOK());

  FileRecord<Strategy::FULL_CACHE> record(volume);

  bool parsed = true;
  REQUIRE_NOTHROW(parsed = record.ParseFileRecord(
                      NtfsBrowserTests::kFragmentedMftInvalidRecordIdx));
  CHECK_FALSE(parsed);
}
