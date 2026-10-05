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
constexpr BYTE sentinel_byte = 0xCC;

}  // namespace

TEMPLATE_TEST_CASE_SIG(
    "A cluster read whose byte address wraps past 2^64 fails instead of "
    "reading the wrapped address",
    "[attr-non-resident][regression]", ((Strategy S), S), Strategy::NoCache,
    Strategy::FullCache)
{
  const std::vector<BYTE> image =
      NtfsBrowserTests::BuildFakeNtfsImageWithWrappingLcn(FakeRunHost::Data);

  NtfsVolume<S> const volume(
      std::make_unique<NtfsBrowserTests::MemoryDiskReader>(
          std::vector<BYTE>(image)));
  REQUIRE(volume.IsVolumeOK());

  FileRecord<S> record(volume);
  REQUIRE(record.ParseFileRecord(static_cast<ULONGLONG>(MftIdx::Root)));
  REQUIRE(record.ParseAttrs());

  const auto& data_attrs = record.GetAttr(AttrType::Data);
  REQUIRE(data_attrs.size() == 1);

  std::vector<BYTE> buffer(NtfsBrowserTests::fake_cluster_size, sentinel_byte);
  // The REQUIRE above checks the size of dataAttrs.
  // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
  const std::optional<ULONGLONG> read = data_attrs[0]->ReadData(0, buffer);

  // The wrapped address is 0: the boot sector must not come back as data.
  CHECK_FALSE(read.has_value());
  const std::vector<BYTE> boot_sector(
      image.begin(),
      image.begin() + gsl::narrow<std::ptrdiff_t>(buffer.size()));
  const bool returned_boot_sector = buffer == boot_sector;
  CHECK_FALSE(returned_boot_sector);
}

TEMPLATE_TEST_CASE_SIG(
    "A data run list whose cumulative LCN overflows is rejected",
    "[attr-non-resident][regression]", ((Strategy S), S), Strategy::NoCache,
    Strategy::FullCache)
{
  SECTION("strict: the whole attribute is rejected")
  {
    NtfsVolume<S> const volume(
        std::make_unique<NtfsBrowserTests::MemoryDiskReader>(
            NtfsBrowserTests::BuildFakeNtfsImageWithOverflowingLcnSum(
                FakeRunHost::Data)));
    REQUIRE(volume.IsVolumeOK());

    FileRecord<S> record(volume);
    REQUIRE(record.ParseFileRecord(static_cast<ULONGLONG>(MftIdx::Root)));
    CHECK_FALSE(record.ParseAttrs());
    CHECK(record.GetAttr(AttrType::Data).empty());
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
    REQUIRE(record.ParseFileRecord(static_cast<ULONGLONG>(MftIdx::Root)));
    CHECK(record.ParseAttrs());

    const auto& data_attrs = record.GetAttr(AttrType::Data);
    REQUIRE(data_attrs.size() == 1);

    // Only the first run (VCN 0) was kept: the second cluster is unmapped.
    std::vector<BYTE> buffer(size_t{2} * NtfsBrowserTests::fake_cluster_size,
                             sentinel_byte);
    // The REQUIRE above checks the size of dataAttrs.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    CHECK_FALSE(data_attrs[0]->ReadData(0, buffer).has_value());
  }
}
