#include <array>
#include <cstring>
#include <memory>
#include <optional>

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

namespace
{

// Sentinel the read buffer is pre-filled with, so the bytes ReadData() never
// actually touches stay recognizable instead of looking like valid data.
constexpr BYTE kSentinelByte = 0xCC;

// Bigger than kSmallResidentDataContent's 4 bytes, mirroring
// NTFSLibTests/ntfsundel/ntfsundelDlg.cpp's real-world pattern of always
// requesting a fixed, oversized buffer (64KiB there) regardless of the
// attribute's actual size.
constexpr size_t kBufferSize = 8;

template <Strategy S>
void CheckReadDataReturnsActualByteCount()
{
  auto reader = std::make_unique<NtfsBrowserTests::MemoryDiskReader>(
      NtfsBrowserTests::BuildFakeNtfsImageWithSmallResidentData());

  NtfsVolume<S> volume(std::move(reader));
  REQUIRE(volume.IsVolumeOK());

  FileRecord<S> record(volume);
  REQUIRE(record.ParseFileRecord(static_cast<ULONGLONG>(MftIdx::ROOT)));
  REQUIRE(record.ParseAttrs());

  const auto& dataAttrs = record.getAttr(AttrType::DATA);
  REQUIRE(dataAttrs.size() == 1);

  std::array<BYTE, kBufferSize> buffer{};
  buffer.fill(kSentinelByte);

  const std::optional<ULONGLONG> result = dataAttrs[0]->ReadData(0, buffer);

  REQUIRE(result.has_value());
  // Before the fix, this wrongly reports kBufferSize (8, the requested
  // buffer size) instead of the real, truncated byte count actually copied.
  CHECK(*result == NtfsBrowserTests::kSmallResidentDataContent.size());

  // The known content must have been copied into the front of the buffer
  // regardless of the bug - the memcpy itself was always correctly bounded.
  CHECK(std::memcmp(buffer.data(),
                    NtfsBrowserTests::kSmallResidentDataContent.data(),
                    NtfsBrowserTests::kSmallResidentDataContent.size()) == 0);

  // The tail of the buffer, past the attribute's real data size, must remain
  // untouched sentinel bytes - proving a caller must not trust the full
  // buffer just because a length was returned.
  for (size_t i = NtfsBrowserTests::kSmallResidentDataContent.size();
       i < buffer.size(); i++)
  {
    CHECK(buffer[i] == kSentinelByte);
  }
}

}  // namespace

// Regression test for bug F10 (docs/bug-reports/2026-09-03-full-repo.md):
// AttrResident<S>::ReadData() (src/attr-resident.cpp) computes "actural" -
// the real, possibly-truncated number of bytes the memcpy actually copied -
// but returns "bufLen" (the caller's full requested buffer size) instead.
// The memcpy itself always stays correctly bounded by "actural"; the defect
// is purely in the misleading return value, letting a caller believe it
// received a full read when the rest of its buffer was left untouched. The
// bug report's own scenario is NTFSLibTests/ntfsundel/ntfsundelDlg.cpp,
// which always requests a fixed 64KiB buffer and trusts the returned length
// to decide how much of it to write back out - corrupting recovered files
// whose real resident $DATA is smaller than that.
TEST_CASE(
    "AttrResident::ReadData returns the actual bytes copied, not the "
    "requested buffer size (F10)",
    "[attr-resident][regression]")
{
  CheckReadDataReturnsActualByteCount<Strategy::NO_CACHE>();
}

// Same fixture under FULL_CACHE: AttrResident<S>::ReadData() (the base
// template in src/attr-resident.cpp) runs identically regardless of
// strategy - only GetData()/GetDataSize() differ between
// AttrResidentNoCache and AttrResidentFullCache - confirming the defect
// (and its fix) isn't an artifact of one strategy, mirroring the paired
// NO_CACHE/FULL_CACHE tests elsewhere in this test suite (eg.
// attr-name-bounds-tests.cpp, parse-attrs-total-size-tests.cpp).
TEST_CASE(
    "AttrResident::ReadData returns the actual bytes copied, not the "
    "requested buffer size (F10, FULL_CACHE)",
    "[attr-resident][regression]")
{
  CheckReadDataReturnsActualByteCount<Strategy::FULL_CACHE>();
}
