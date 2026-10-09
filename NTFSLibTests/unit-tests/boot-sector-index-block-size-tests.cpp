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
namespace Cache = NtfsBrowser::Cache;

TEMPLATE_TEST_CASE_SIG(
    "NtfsVolume must not accept a volume whose BPB describes an index block "
    "far smaller than Data::IndexBlock",
    "[ntfs-volume][regression]", ((Cache::Strategy S), S),
    Cache::Strategy::NoCache, Cache::Strategy::FullCache) {
  auto reader = std::make_unique<NtfsBrowserTests::MemoryDiskReader>(
      NtfsBrowserTests::BuildFakeNtfsImageWithTinyIndexBlock());

  const NtfsVolume<S> volume(std::move(reader));

  INFO("GetIndexBlockSize() = " << volume.GetIndexBlockSize());
  CHECK(volume.GetIndexBlockSize() == NtfsBrowserTests::tiny_index_block_size);

  // tiny_index_block_size doesn't fit Data::IndexBlock's own header.
  CHECK_FALSE(volume.IsVolumeOK());
}
