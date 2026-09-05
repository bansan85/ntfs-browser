#include <memory>

#include <catch2/catch_test_macros.hpp>

#include <ntfs-browser/file-record.h>
#include <ntfs-browser/ntfs-volume.h>
#include <ntfs-browser/strategy.h>

#include "fake-ntfs-image.h"
#include "memory-disk-reader.h"

using NtfsBrowser::FileRecord;
using NtfsBrowser::NtfsVolume;
using NtfsBrowser::Strategy;

// Regression fixture for bug F15: FileRecord<S>::IsDeleted() and
// FileRecord<S>::IsDirectory() (src/file-record.cpp, ~lines 951-965) do
// "file_record_->GetData()->flags & ..." with no has_value()/truthiness
// check first, where file_record_ is a
// std::optional<FileRecordHeaderImpl<S>> (include/ntfs-browser/file-record.h)
// that ParseFileRecord() only assigns on its very last line, after every
// prior step has succeeded. Calling either method on a FileRecord that has
// never had a successful ParseFileRecord() call - eg. a caller who ignores
// its [[nodiscard]] bool return, or simply never calls it at all - invokes
// std::optional::operator->() on a disengaged optional: undefined behavior.
// This is the only reachable gap of its kind: the six other similarly-named
// accessors right below these two in the same file (IsReadOnly(), IsHidden(),
// IsSystem(), IsCompressed(), IsEncrypted(), IsSparse()) go through
// attr_list_[ATTR_INDEX(AttrType::STANDARD_INFORMATION)] instead, and already
// guard with "if (vec.empty()) { return false; }" before touching anything -
// this test (and the fix) simply brings IsDeleted()/IsDirectory() in line
// with that same established idiom.
//
// Unlike most bugs in this repo, an empty std::optional's operator->() isn't
// a deterministic "wrong but observable" value - it's genuine undefined
// behavior, so a naive test could not honestly claim a reproducible "red"
// result. Before writing this test, the actual behavior of MSVC STL's
// operator->() in this repo's own Debug build configuration was checked
// empirically (not merely assumed from memory):
//
//   1. The installed MSVC STL header
//      ".../VC/Tools/MSVC/14.44.35207/include/optional" guards both
//      operator->() overloads with:
//        #if _MSVC_STL_HARDENING_OPTIONAL || _ITERATOR_DEBUG_LEVEL != 0
//          _STL_VERIFY(this->_Has_value, "operator->() called on empty
//          optional");
//        #endif
//      and yvals.h defines _STL_VERIFY(cond, mesg) to call
//      _STL_REPORT_ERROR(mesg) (which does _RPTF0(_CRT_ASSERT, mesg) then
//      _MSVC_STL_DOOM_FUNCTION(mesg), ultimately aborting the process) when
//      "cond" is false - not compiled out under NDEBUG the way a plain
//      assert() is, but gated on _ITERATOR_DEBUG_LEVEL instead.
//   2. A standalone throwaway .cpp calling operator->() on a disengaged
//      std::optional<Foo> was compiled with cl.exe using the exact same
//      /MDd (MultiThreadedDebugDLL) runtime this repo's generated
//      NtfsBrowserTests.vcxproj uses for its Debug configuration (confirmed
//      by grepping that .vcxproj for RuntimeLibrary), then actually run.
//      /MDd's debug CRT defaults _ITERATOR_DEBUG_LEVEL to 2, so the guard
//      above is active. Result: the process aborted (observed exit code 3,
//      the standard CRT abort() exit code) immediately on the operator->()
//      call, before the next printf ran.
//
// This means - exactly like bug F19's assert() in
// file-reader-full-cache-boundary-tests.cpp, and bug F14's null-pointer
// dereference in mft-parse-failure-volume-ok-tests.cpp - this repo's own
// "ctest --test-dir build -C Debug" configuration deterministically aborts
// the whole process on this exact defect, before the fix. CMake's
// catch_discover_tests registers each TEST_CASE as an independent ctest
// test/process (NTFSLibTests/unit-tests/CMakeLists.txt), so this test
// crashing its own process is exactly what makes ctest report *this* test -
// and only this test - as failed; it does not disturb any other test.
//
// After the fix (an early "if (!file_record_) { return false; }" in both
// methods, mirroring the "if (vec.empty()) { return false; }" idiom used two
// screens away in the same file), both calls below must return false without
// aborting.
//
// Not reachable via either fuzzer: NtfsFuzzer (main.cpp) and NtfsFuzzerAfl
// (afl-main.cpp) both drive NtfsVolume/FileRecord through a full, successful
// ParseFileRecord()+ParseAttrs() before doing anything else (returning early
// otherwise), and neither ever calls IsDeleted()/IsDirectory() at all - see
// the "dead code from a fuzzing perspective" discussion for bug F9 near the
// end of fuzzer-regression-tests.cpp for the same reasoning applied to a
// different bug. This is purely a caller-discipline API contract issue
// (calling a public accessor before/without a successful parse), not
// something any malformed on-disk byte stream reachable through the fuzz
// harnesses' call sequence could trigger, so no fuzz corpus entry is added
// for it.
TEST_CASE(
    "FileRecord::IsDeleted()/IsDirectory() must not dereference an empty "
    "file_record_ when called before any successful ParseFileRecord() (F15)",
    "[file-record][regression]")
{
  auto reader = std::make_unique<NtfsBrowserTests::MemoryDiskReader>(
      NtfsBrowserTests::BuildFakeNtfsImage());

  NtfsVolume<Strategy::NO_CACHE> volume(std::move(reader));
  REQUIRE(volume.IsVolumeOK());

  // Deliberately never call ParseFileRecord() - file_record_ stays
  // default-constructed (empty), exactly the "never calls it at all"
  // trigger scenario from the bug report.
  FileRecord<Strategy::NO_CACHE> record(volume);

  // Before the fix: operator->() on the empty file_record_ aborts this
  // test's own isolated ctest process right here (see the file comment
  // above for how this was confirmed to be deterministic in this exact
  // build configuration). After the fix: returns false without touching
  // file_record_'s contents.
  CHECK_FALSE(record.IsDeleted());

  // Same defect, same fix, in the sibling method - only reached at all once
  // the fix is in place (see above).
  CHECK_FALSE(record.IsDirectory());
}
