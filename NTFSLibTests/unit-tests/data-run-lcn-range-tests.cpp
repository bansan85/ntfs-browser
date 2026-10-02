#include <ntfs-browser/win-types.h>

#include <cstddef>
#include <memory>
#include <optional>
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
#include <ntfs-browser/volume-options.h>

#include "catch2/catch_message.hpp"
#include "fake-ntfs-image.h"
#include "memory-disk-reader.h"

using NtfsBrowser::AttrType;
using NtfsBrowser::FileRecord;
using NtfsBrowser::NtfsVolume;
using NtfsBrowser::Strategy;
using NtfsBrowser::VolumeOptions;
using NtfsBrowser::Enum::MftIdx;
using NtfsBrowserTests::FakeRunHost;

namespace
{

// Fills the read buffer so a byte ReadData() left untouched shows up.
constexpr BYTE kSentinelByte = 0xCC;

}  // namespace

TEMPLATE_TEST_CASE_SIG(
    "A cluster read whose byte address wraps past 2^64 fails instead of "
    "reading the wrapped address",
    "[attr-non-resident][regression]", ((Strategy S), S), Strategy::NO_CACHE,
    Strategy::FULL_CACHE)
{
  const std::vector<BYTE> image =
      NtfsBrowserTests::BuildFakeNtfsImageWithWrappingLcn(FakeRunHost::Data);

  NtfsVolume<S> const volume(
      std::make_unique<NtfsBrowserTests::MemoryDiskReader>(
          std::vector<BYTE>(image)));
  REQUIRE(volume.IsVolumeOK());

  FileRecord<S> record(volume);
  REQUIRE(record.ParseFileRecord(static_cast<ULONGLONG>(MftIdx::ROOT)));
  REQUIRE(record.ParseAttrs());

  const auto& dataAttrs = record.getAttr(AttrType::DATA);
  REQUIRE(dataAttrs.size() == 1);

  std::vector<BYTE> buffer(NtfsBrowserTests::kFakeClusterSize, kSentinelByte);
  const std::optional<ULONGLONG> read = dataAttrs[0]->ReadData(0, buffer);

  // The wrapped address is 0: the boot sector must not come back as data.
  CHECK_FALSE(read.has_value());
  const std::vector<BYTE> bootSector(
      image.begin(),
      image.begin() + gsl::narrow<std::ptrdiff_t>(buffer.size()));
  const bool returnedBootSector = buffer == bootSector;
  CHECK_FALSE(returnedBootSector);
}

TEMPLATE_TEST_CASE_SIG(
    "A data run list whose cumulative LCN overflows is rejected",
    "[attr-non-resident][regression]", ((Strategy S), S), Strategy::NO_CACHE,
    Strategy::FULL_CACHE)
{
  SECTION("strict: the whole attribute is rejected")
  {
    NtfsVolume<S> const volume(
        std::make_unique<NtfsBrowserTests::MemoryDiskReader>(
            NtfsBrowserTests::BuildFakeNtfsImageWithOverflowingLcnSum(
                FakeRunHost::Data)));
    REQUIRE(volume.IsVolumeOK());

    FileRecord<S> record(volume);
    REQUIRE(record.ParseFileRecord(static_cast<ULONGLONG>(MftIdx::ROOT)));
    CHECK_FALSE(record.ParseAttrs());
    CHECK(record.getAttr(AttrType::DATA).empty());
  }

  SECTION("recovering: the run decoded before the overflow is kept")
  {
    NtfsVolume<S> const volume(
        std::make_unique<NtfsBrowserTests::MemoryDiskReader>(
            NtfsBrowserTests::BuildFakeNtfsImageWithOverflowingLcnSum(
                FakeRunHost::Data)),
        VolumeOptions{.recover_errors = true});
    REQUIRE(volume.IsVolumeOK());

    FileRecord<S> record(volume);
    REQUIRE(record.ParseFileRecord(static_cast<ULONGLONG>(MftIdx::ROOT)));
    CHECK(record.ParseAttrs());

    const auto& dataAttrs = record.getAttr(AttrType::DATA);
    REQUIRE(dataAttrs.size() == 1);

    // Only the first run (VCN 0) was kept: the second cluster is unmapped.
    std::vector<BYTE> buffer(2 * NtfsBrowserTests::kFakeClusterSize,
                             kSentinelByte);
    CHECK_FALSE(dataAttrs[0]->ReadData(0, buffer).has_value());
  }
}
