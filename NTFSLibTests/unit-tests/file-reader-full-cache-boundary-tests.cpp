#include <cstddef>
#include <memory>
#include <optional>
#include <span>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include <ntfs-browser/file-reader.h>
#include <ntfs-browser/strategy.h>

#include "memory-disk-reader.h"

using NtfsBrowser::FileReader;
using NtfsBrowser::Strategy;

// Regression fixture for bug F19: FileReader<Strategy::FULL_CACHE>::Read()
// (src/file-reader.cpp, ~lines 69-105) only guards against a read crossing
// a 64KiB cache block boundary with a plain assert() from <cassert> -
// compiled out entirely in Release builds (NDEBUG). Should such a read ever
// slip through in Release, the two
// "return std::span<const BYTE>{..., length}" statements (lines ~84-86 and
// ~103) never clamp "length" to what's actually left in the current 64KiB
// slice: NextMemory() hands out slices carved out of separate 512*64KiB
// (32MiB) make_unique<BYTE[]> allocations, so a crossing read landing on the
// 512th (last) slice of one of those allocations returns a span reaching
// straight past the whole make_unique<BYTE[]> - an attacker-controlled
// out-of-bounds heap read, later memcpy'd into the caller's buffer by
// AttrNonResident<S>::ReadVirtualClusters()
// (src/attr-non-resident.cpp:219).
//
// This is reachable on a nominal FULL_CACHE directory traversal
// (ParseIndexBlock -> ReadData -> ReadVirtualClusters -> ReadClusters) once
// ParseBootSector() is fed a forged BPB whose cluster_size_ doesn't evenly
// divide 65536 (eg. sector_size 512 * sectors_per_cluster 3 = 1536) - see
// the bug report for the full attacker-controlled path.
//
// Demonstrating the Release-mode out-of-bounds *read* deterministically
// in-process is not practical here: the assert() is the very first
// statement of Read(), evaluated unconditionally before any span is ever
// constructed, and MSVC's assert() failure path calls abort() directly
// (verified empirically - it does not go through _CrtDbgReportW/
// _CrtSetReportMode, so there is no supported way to defang it from test
// code without rebuilding the library itself with NDEBUG). Catching that
// abort() in-process (eg. via signal(SIGABRT, ...) + setjmp/longjmp) would
// only let this test observe "it aborted", while also abandoning the very
// Read() call whose span we wanted to inspect - it can't get us any closer
// to the Release-mode symptom either.
//
// So this test demonstrates the other, explicitly-acceptable half of the
// bug instead (see the bug report's "Consequence" paragraph: "En Debug,
// l'assert transforme l'entree forgee en abort(): deni de service."): a
// single Read() call whose [addr, addr+length) range crosses a 64KiB cache
// block boundary - the exact condition the assert() exists to catch - takes
// this process down instead of being rejected as an ordinary error, in the
// one build configuration (Debug, matching this repo's
// "ctest --test-dir build -C Debug") that actually exercises the check.
// CMake's catch_discover_tests registers every TEST_CASE as an independent
// ctest test invoked in its own process (see
// NTFSLibTests/unit-tests/CMakeLists.txt), so this test crashing the
// process it runs in is exactly what makes ctest report *this* test - and
// only this test - as failed; it does not disturb any other test.
//
// After the fix, Read() must handle a boundary-crossing request without
// aborting the process, and if it does return data, that data must
// actually be the bytes the caller asked for.
TEST_CASE(
    "FileReader<FULL_CACHE>::Read must not abort/misbehave on a read "
    "crossing a 64KiB cache block boundary (F19)",
    "[file-reader][regression]")
{
  // Two 64KiB blocks' worth of backing data, each byte distinct from its
  // offset, so a correct (fixed) Read() spanning both can be checked for
  // returning the right bytes from the right place - not just "didn't
  // crash".
  std::vector<BYTE> backing(2 * 65536);
  for (size_t i = 0; i < backing.size(); i++)
  {
    backing[i] = static_cast<BYTE>(i);
  }

  auto reader_double =
      std::make_unique<NtfsBrowserTests::MemoryDiskReader>(backing);
  FileReader<Strategy::FULL_CACHE> reader(std::move(reader_double));

  // [65500, 65600) straddles the boundary between the first 64KiB cache
  // block (bytes [0, 65536)) and the second ([65536, 131072)).
  LARGE_INTEGER addr{.QuadPart = 65536 - 36};
  constexpr DWORD kLength = 100;

  // Before the F19 fix: Read()'s assert() fires here and aborts the whole
  // process (this test's own, isolated ctest process - see the file
  // comment above), which ctest reports as this test failing. After the
  // fix, this call must return without aborting.
  const std::optional<std::span<const BYTE>> result =
      reader.Read(addr, kLength);

  if (result)
  {
    CHECK(result->size() == kLength);
    for (size_t i = 0; i < result->size(); i++)
    {
      CHECK((*result)[i] == backing[65536 - 36 + i]);
    }
  }
}
