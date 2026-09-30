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
    "far smaller than Data::IndexBlock",
    "[ntfs-volume][regression]", ((Strategy S), S), Strategy::NO_CACHE,
    Strategy::FULL_CACHE)
{
  auto reader = std::make_unique<NtfsBrowserTests::MemoryDiskReader>(
      NtfsBrowserTests::BuildFakeNtfsImageWithTinyIndexBlock());

  NtfsVolume<S> volume(std::move(reader));

  INFO("GetIndexBlockSize() = " << volume.GetIndexBlockSize());
  CHECK(volume.GetIndexBlockSize() == NtfsBrowserTests::kTinyIndexBlockSize);

  // kTinyIndexBlockSize doesn't fit Data::IndexBlock's own header.
  CHECK_FALSE(volume.IsVolumeOK());
}
