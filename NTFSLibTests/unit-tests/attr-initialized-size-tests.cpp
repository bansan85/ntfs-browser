#include <ntfs-browser/win-types.h>

#include <algorithm>
#include <cstddef>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

#include <catch2/catch_template_test_macros.hpp>
#include <catch2/catch_test_macros.hpp>
#include <gsl/narrow>

#include <ntfs-browser/attr-base.h>  // IWYU pragma: keep
#include <ntfs-browser/data/attr-type.h>
#include <ntfs-browser/file-record.h>
#include <ntfs-browser/mft-idx.h>
#include <ntfs-browser/ntfs-volume.h>  // IWYU pragma: keep
#include <ntfs-browser/strategy.h>

#include "catch2/catch_message.hpp"
#include "fake-ntfs-image.h"
#include "memory-disk-reader.h"

using NtfsBrowser::AttrType;
using NtfsBrowser::FileRecord;
using NtfsBrowser::NtfsVolume;
using NtfsBrowser::Strategy;
using NtfsBrowser::Enum::MftIdx;

namespace
{

// Fills the read buffer so a byte ReadData() left untouched shows up.
constexpr BYTE kSentinelByte = 0xCC;

// What a read of [offset, offset + length) must return: the residue pattern
// below the initialized size, zeros between it and the real size, and nothing
// past the real size.
std::vector<BYTE> ExpectedBytes(ULONGLONG offset, size_t length)
{
  const std::vector<BYTE> residue = NtfsBrowserTests::CompressionFixturePattern(
      NtfsBrowserTests::kUninitializedTailRealSize);

  std::vector<BYTE> expected;
  for (ULONGLONG at = offset; at < offset + length &&
                              at < NtfsBrowserTests::kUninitializedTailRealSize;
       at++)
  {
    expected.push_back(
        at < NtfsBrowserTests::kUninitializedTailIniSize
            // at < kUninitializedTailRealSize = residue.size() by the loop condition.
            // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
            ? residue[gsl::narrow<size_t>(at)]
            : static_cast<BYTE>(0));
  }
  return expected;
}

template <Strategy S>
void CheckReadsBeyondTheInitializedSizeAreZero()
{
  NtfsVolume<S> const volume(
      std::make_unique<NtfsBrowserTests::MemoryDiskReader>(
          NtfsBrowserTests::BuildFakeNtfsImageWithUninitializedTail()));
  REQUIRE(volume.IsVolumeOK());

  FileRecord<S> record(volume);
  REQUIRE(record.ParseFileRecord(static_cast<ULONGLONG>(MftIdx::ROOT)));
  REQUIRE(record.ParseAttrs());

  const auto& dataAttrs = record.getAttr(AttrType::DATA);
  REQUIRE(dataAttrs.size() == 1);

  struct Range
  {
    ULONGLONG offset;
    size_t length;
  };
  const Range ranges[] = {
      {0, 3000},     // the whole stream, aligned start, unaligned end
      {0, 1024},     // wholly initialized, one aligned cluster
      {1400, 300},   // straddles the initialized size
      {1500, 500},   // starts exactly at the initialized size
      {2000, 500},   // wholly uninitialized, unaligned
      {1024, 2048},  // aligned start, reaches past the real size
      {2990, 100}    // clamped by the real size
  };

  for (const Range& range : ranges)
  {
    INFO("offset " << range.offset << ", length " << range.length);
    std::vector<BYTE> buffer(range.length, kSentinelByte);
    const std::optional<ULONGLONG> read =
        // The REQUIRE above checks that there is one DATA attribute.
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        dataAttrs[0]->ReadData(range.offset, buffer);
    REQUIRE(read.has_value());

    const std::vector<BYTE> expected =
        ExpectedBytes(range.offset, range.length);
    REQUIRE(*read == expected.size());
    buffer.resize(gsl::narrow<size_t>(*read));
    const bool same = buffer == expected;
    CHECK(same);
  }
}

}  // namespace

TEMPLATE_TEST_CASE_SIG(
    "A non-resident stream reads as zeros beyond its initialized size",
    "[attr-non-resident][regression]", ((Strategy S), S), Strategy::NO_CACHE,
    Strategy::FULL_CACHE)
{
  CheckReadsBeyondTheInitializedSizeAreZero<S>();
}
