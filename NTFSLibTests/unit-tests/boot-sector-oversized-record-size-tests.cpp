#include <memory>
#include <utility>
#include <vector>

#include <catch2/catch_template_test_macros.hpp>
#include <catch2/catch_test_macros.hpp>

#include <ntfs-browser/disk-reader.h>
#include <ntfs-browser/ntfs-volume.h>
#include <ntfs-browser/strategy.h>

#include "catch2/catch_message.hpp"
#include "fake-ntfs-image.h"
#include "memory-disk-reader.h"

using NtfsBrowser::NtfsVolume;
using NtfsBrowser::Strategy;

TEMPLATE_TEST_CASE_SIG(
    "NtfsVolume must not accept a volume whose BPB describes an index block "
    "far larger than any plausible size",
    "[ntfs-volume][regression]", ((Strategy S), S), Strategy::NO_CACHE,
    Strategy::FULL_CACHE)
{
  auto reader = std::make_unique<NtfsBrowserTests::MemoryDiskReader>(
      NtfsBrowserTests::BuildFakeNtfsImageWithOversizedIndexBlock());

  NtfsVolume<S> const volume(std::move(reader));

  INFO("GetIndexBlockSize() = " << volume.GetIndexBlockSize());
  CHECK_FALSE(volume.IsVolumeOK());
}

TEMPLATE_TEST_CASE_SIG(
    "NtfsVolume must not accept a volume whose BPB describes a file record "
    "far larger than any plausible size",
    "[ntfs-volume][regression]", ((Strategy S), S), Strategy::NO_CACHE,
    Strategy::FULL_CACHE)
{
  auto reader = std::make_unique<NtfsBrowserTests::MemoryDiskReader>(
      NtfsBrowserTests::BuildFakeNtfsImageWithOversizedFileRecord());

  NtfsVolume<S> const volume(std::move(reader));

  INFO("GetFileRecordSize() = " << volume.GetFileRecordSize());
  // GetFileRecordSize() reflects the BPB value directly; IsVolumeOK() fails
  // regardless, from an unrelated $Volume read failure this size triggers.
  CHECK(volume.GetFileRecordSize() !=
        NtfsBrowserTests::kOversizedFileRecordSize);
}

TEMPLATE_TEST_CASE_SIG(
    "NtfsVolume must not accept a volume whose BPB describes a file record "
    "size that exceeds kMaxFileRecordSize via the positive "
    "clusters_per_file_record branch",
    "[ntfs-volume][regression]", ((Strategy S), S), Strategy::NO_CACHE,
    Strategy::FULL_CACHE)
{
  auto reader = std::make_unique<NtfsBrowserTests::MemoryDiskReader>(
      NtfsBrowserTests::BuildFakeNtfsImageWithFileRecordSizeTooBig());

  NtfsVolume<S> const volume(std::move(reader));

  CHECK(volume.GetFileRecordSize() == NtfsBrowserTests::kFileRecordSizeTooBig);
  CHECK_FALSE(volume.IsVolumeOK());
}
