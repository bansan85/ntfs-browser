#include <optional>

#include <catch2/catch_test_macros.hpp>

#include <ntfs-browser/ntfs-volume.h>
#include <ntfs-browser/strategy.h>

#include "fake-ntfs-image.h"
#include "memory-disk-reader.h"

using NtfsBrowser::NtfsVolume;
using NtfsBrowser::Strategy;

// Regression fixture for bug F8: NtfsVolume<S>::ParseBootSector()
// (src/ntfs-volume.cpp) computes mft_addr_ = bpb->lcn_mft * cluster_size_
// straight from the BPB - fully attacker-controlled on a forged/corrupted
// volume - and never validates the result before it is used. With lcn_mft ==
// kHugeMftLcn (2^53) and this fixture's fixed 1024-byte cluster size,
// mft_addr_ ends up exactly 2^63: still representable in the ULONGLONG
// mft_addr_ is stored as, but FileRecord<S>::ReadFileRecord()
// (src/file-record.cpp:247-248) immediately narrows GetMFTAddr() (plus a
// per-record offset) down to a LONGLONG for LARGE_INTEGER::QuadPart via
// gsl::narrow<LONGLONG>() - and a value that large makes narrow() throw
// gsl::narrowing_error.
//
// gsl::narrowing_error derives from std::exception, *not*
// std::runtime_error (3rdparty/gsl/include/gsl/narrow). The parser's two
// catch clauses (FileRecord::ParseAttr, src/file-record.cpp:217; the
// FileRecordHeader::Factory call in FileRecord::ReadFileRecord,
// src/file-record.cpp:260) only ever catch std::runtime_error, and neither
// of them wraps the narrow() call at file-record.cpp:247-248 anyway. The
// exception therefore propagates uncaught out of
// FileRecord::ParseFileRecord() -> NtfsVolume::Init() -> the NtfsVolume
// constructor itself, breaking the "construct, then check IsVolumeOK()"
// contract every constructor caller in this repo relies on (eg.
// NTFSLibTests/ntfsdir/main.cpp:217, none of which wrap construction in a
// try/catch).
//
// REQUIRE_NOTHROW below documents that contract directly: it wraps the
// construction in Catch2's own catch(...), so an escaping exception is
// reported as a failed assertion instead of terminating the test binary.
// Before the F8 fix, this assertion fails because gsl::narrowing_error does
// escape. After the fix, construction must complete without throwing, and
// the volume must report IsVolumeOK() == false (mft_addr_ was never valid to
// begin with).
TEST_CASE(
    "NtfsVolume construction must not let an exception escape when the BPB "
    "encodes an mft_addr_ too large for a LONGLONG (F8)",
    "[ntfs-volume][regression]")
{
  auto reader = std::make_unique<NtfsBrowserTests::MemoryDiskReader>(
      NtfsBrowserTests::BuildFakeNtfsImageWithHugeMftLcn());

  std::optional<NtfsVolume<Strategy::NO_CACHE>> volume;
  REQUIRE_NOTHROW(volume.emplace(std::move(reader)));

  REQUIRE(volume.has_value());
  CHECK_FALSE(volume->IsVolumeOK());
}
