#include <memory>
#include <utility>
#include <vector>

#include <catch2/catch_template_test_macros.hpp>
#include <catch2/catch_test_macros.hpp>

#include <ntfs-browser/disk-reader.h>
#include <ntfs-browser/file-record.h>
#include <ntfs-browser/ntfs-volume.h>
#include <ntfs-browser/strategy.h>

#include "fake-ntfs-image.h"
#include "memory-disk-reader.h"

using NtfsBrowser::FileRecord;
using NtfsBrowser::NtfsVolume;
using NtfsBrowser::Strategy;

TEMPLATE_TEST_CASE_SIG(
    "FileRecord::IsDeleted()/IsDirectory() must not dereference an empty "
    "file_record_ when called before any successful ParseFileRecord()",
    "[file-record][regression]", ((Strategy S), S), Strategy::NO_CACHE,
    Strategy::FULL_CACHE)
{
  auto reader = std::make_unique<NtfsBrowserTests::MemoryDiskReader>(
      NtfsBrowserTests::BuildFakeNtfsImage());

  NtfsVolume<S> const volume(std::move(reader));
  REQUIRE(volume.IsVolumeOK());

  FileRecord<S> const record(volume);

  // A defect here can abort the whole process, not just fail this check.
  CHECK_FALSE(record.IsDeleted());
  CHECK_FALSE(record.IsDirectory());
}
