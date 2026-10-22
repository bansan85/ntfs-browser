#include <ntfs-browser/win-types.h>

#include <array>
#include <cstddef>
#include <memory>
#include <optional>
#include <vector>

#include <catch2/catch_template_test_macros.hpp>
#include <catch2/catch_test_macros.hpp>
#include <gsl/narrow>

#include <ntfs-browser/attr/type.h>
#include <ntfs-browser/cache/strategy.h>
#include <ntfs-browser/io/file-record.h>
#include <ntfs-browser/mft/idx.h>
#include <ntfs-browser/namespace.h>
#include <ntfs-browser/ntfs-volume.h>

#include "catch2/catch_message.hpp"
#include "fake-ntfs-image.h"
#include "memory-disk-reader.h"
#include "optional-access.h"

namespace NTFS_BROWSER_NAMESPACE {
namespace Io {

template <Cache::Strategy S>
class FileRecord;

}  // namespace Io
}  // namespace NTFS_BROWSER_NAMESPACE

namespace Attr = NTFS_BROWSER_NAMESPACE::Attr;
using NTFS_BROWSER_NAMESPACE::NtfsVolume;
using NTFS_BROWSER_NAMESPACE::Io::FileRecord;
namespace Cache = NTFS_BROWSER_NAMESPACE::Cache;
namespace Mft = NTFS_BROWSER_NAMESPACE::Mft;

namespace {

// Fills the read buffer so a byte ReadData() left untouched shows up.
constexpr BYTE sentinel_byte = 0xCC;

// What a read of [offset, offset + length) must return: the residue pattern
// below the initialized size, zeros between it and the real size, and nothing
// past the real size.
std::vector<BYTE> ExpectedBytes(ULONGLONG offset, size_t length) {
  const std::vector<BYTE> residue =
      NTFS_BROWSER_NAMESPACE::Tests::CompressionFixturePattern(
          NTFS_BROWSER_NAMESPACE::Tests::uninitialized_tail_real_size);

  std::vector<BYTE> expected;
  for (ULONGLONG at = offset;
       at < offset + length &&
       at < NTFS_BROWSER_NAMESPACE::Tests::uninitialized_tail_real_size;
       at++) {
    expected.push_back(
        at < NTFS_BROWSER_NAMESPACE::Tests::uninitialized_tail_ini_size
            // at < uninitialized_tail_real_size = residue.size() by the loop
            // condition.
            // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
            ? residue[gsl::narrow<size_t>(at)]
            : static_cast<BYTE>(0));
  }
  return expected;
}

template <Cache::Strategy S>
void CheckReadsBeyondTheInitializedSizeAreZero() {
  const NtfsVolume<S> volume(
      std::make_unique<NTFS_BROWSER_NAMESPACE::Tests::MemoryDiskReader>(
          NTFS_BROWSER_NAMESPACE::Tests::
              BuildFakeNtfsImageWithUninitializedTail()));
  REQUIRE(volume.IsVolumeOK());

  NTFS_BROWSER_NAMESPACE::Io::FileRecord<S> record(volume);
  REQUIRE(record.ParseFileRecord(static_cast<ULONGLONG>(Mft::Idx::Root)));
  REQUIRE(record.ParseAttrs());

  const auto& data_attrs = record.GetAttr(Attr::Type::Data);
  REQUIRE(data_attrs.size() == 1);

  struct Range {
    ULONGLONG offset;
    size_t length;
  };

  const auto ranges = std::to_array<Range>({
      {0, 3000},     // the whole stream, aligned start, unaligned end
      {0, 1024},     // wholly initialized, one aligned cluster
      {1400, 300},   // straddles the initialized size
      {1500, 500},   // starts exactly at the initialized size
      {2000, 500},   // wholly uninitialized, unaligned
      {1024, 2048},  // aligned start, reaches past the real size
      {2990, 100}    // clamped by the real size
  });

  for (const Range& range : ranges) {
    INFO("offset " << range.offset << ", length " << range.length);
    std::vector<BYTE> buffer(range.length, sentinel_byte);
    const std::optional<ULONGLONG> read =
        // The REQUIRE above checks that there is one DATA attribute.
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        data_attrs[0]->ReadData(range.offset, buffer);
    REQUIRE(read.has_value());

    const std::vector<BYTE> expected =
        ExpectedBytes(range.offset, range.length);
    REQUIRE(NTFS_BROWSER_NAMESPACE::Tests::Unwrap(read) == expected.size());
    buffer.resize(
        gsl::narrow<size_t>(NTFS_BROWSER_NAMESPACE::Tests::Unwrap(read)));
    const bool same = buffer == expected;
    CHECK(same);
  }
}

}  // namespace

TEMPLATE_TEST_CASE_SIG(
    "A non-resident stream reads as zeros beyond its initialized size",
    "[attr-non-resident][regression]", ((Cache::Strategy S), S),
    Cache::Strategy::NoCache, Cache::Strategy::FullCache) {
  CheckReadsBeyondTheInitializedSizeAreZero<S>();
}
