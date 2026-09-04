#include <catch2/catch_test_macros.hpp>

#include <ntfs-browser/ntfs-volume.h>
#include <ntfs-browser/strategy.h>

#include "fake-ntfs-image.h"
#include "memory-disk-reader.h"

using NtfsBrowser::NtfsVolume;
using NtfsBrowser::Strategy;

// Regression fixture for bug F14: NtfsVolume<S>::Init() (src/ntfs-volume.cpp,
// ~lines 120-134) sets volume_ok_ = true right after $Volume's
// VOLUME_INFORMATION attribute is successfully parsed - *before*
// mft_record_.ParseFileRecord()/ParseAttrs() have even run. If either of
// those fails, or $MFT's DATA attribute vector comes back empty, Init()
// returns early leaving mft_data_ == nullptr while IsVolumeOK() already
// reports true. GetRecordsCount() (src/ntfs-volume.cpp, ~lines 271-274) then
// dereferences mft_data_ unconditionally through a virtual call
// (GetDataSize()) and is marked noexcept.
//
// This fixture forges exactly that scenario: the boot sector and $Volume (#3)
// are both valid (NtfsVolume<S>::Init() gets as far as
// "volume_ok_ = true;"), but $MFT (#0) is zero-filled - its magic no longer
// matches kFileRecordMagic, so FileRecord<S>::ParseFileRecord() fails on it
// and mft_data_ is never assigned. This mirrors a real caller:
// NTFSLibTests/ntfsundel/ntfsundelDlg.cpp:185-191 does exactly
// "if (!volume.IsVolumeOK()) return;" then "volume.GetRecordsCount()" -
// respecting the documented "construct, then check IsVolumeOK()" contract -
// and still crashes, because IsVolumeOK() lied.
//
// Before the F14 fix: IsVolumeOK() incorrectly reports true (the CHECK_FALSE
// below fails), and the subsequent GetRecordsCount() call dereferences a null
// mft_data_ through a virtual call - an access violation, not a C++
// exception, so it cannot be caught with try/catch here. CMake's
// catch_discover_tests registers every TEST_CASE as an independent ctest
// process (NTFSLibTests/unit-tests/CMakeLists.txt), so this test crashing its
// own process is exactly what makes ctest report *this* test - and only this
// test - as failed; it does not disturb any other test. See
// file-reader-full-cache-boundary-tests.cpp (F19) for the same
// acceptance criteria/rationale.
//
// After the fix: volume_ok_ is only set once mft_data_ is actually assigned,
// so IsVolumeOK() correctly reports false here, and GetRecordsCount() must
// not dereference a null mft_data_ regardless of what IsVolumeOK() says -
// both CHECKs below then pass without crashing.
TEST_CASE(
    "NtfsVolume must not report IsVolumeOK() == true, nor let "
    "GetRecordsCount() dereference a null $MFT DATA attribute, when $MFT's "
    "own file record fails to parse (F14)",
    "[ntfs-volume][regression]")
{
  auto reader = std::make_unique<NtfsBrowserTests::MemoryDiskReader>(
      NtfsBrowserTests::BuildFakeNtfsImageWithCorruptMftRecord());

  NtfsVolume<Strategy::NO_CACHE> volume(std::move(reader));

  // Before the fix: fails here (IsVolumeOK() wrongly true) - a plain, safe
  // assertion failure, not a crash. After the fix: passes.
  CHECK_FALSE(volume.IsVolumeOK());

  // Before the fix: crashes the whole (isolated) test process regardless of
  // the CHECK above, since GetRecordsCount() dereferences the still-null
  // mft_data_ unconditionally. After the fix: mft_data_ is still null, but
  // GetRecordsCount() must guard against that and return a safe value (0)
  // instead of crashing.
  CHECK(volume.GetRecordsCount() == 0);
}
