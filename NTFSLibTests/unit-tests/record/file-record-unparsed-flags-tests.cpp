#include <memory>
#include <utility>
#include <vector>

#include <catch2/catch_template_test_macros.hpp>
#include <catch2/catch_test_macros.hpp>

#include <ntfs-browser/namespace.h>
#include <ntfs-browser/test/disk-readers/memory-disk-reader.h>
#include <ntfs-browser/test/fake-image/fake-ntfs-image.h>

#include "cache/strategy.h"
#include "io/file-record.h"
#include "ntfs-volume.h"

namespace NTFS_BROWSER_NAMESPACE {
namespace Io {

template <Cache::Strategy S>
class FileRecord;

}  // namespace Io
}  // namespace NTFS_BROWSER_NAMESPACE

using NTFS_BROWSER_NAMESPACE::NtfsVolume;
using NTFS_BROWSER_NAMESPACE::Io::FileRecord;
namespace Cache = NTFS_BROWSER_NAMESPACE::Cache;

TEMPLATE_TEST_CASE_SIG(
    "FileRecord::IsDeleted()/IsDirectory() must not dereference an empty "
    "file_record_ when called before any successful ParseFileRecord()",
    "[file-record][regression]", ((Cache::Strategy S), S),
    Cache::Strategy::NoCache, Cache::Strategy::FullCache) {
  auto reader = std::make_unique<
      NTFS_BROWSER_NAMESPACE::Test::DiskReaders::MemoryDiskReader>(
      NTFS_BROWSER_NAMESPACE::Test::FakeImage::BuildFakeNtfsImage());

  const NtfsVolume<S> volume(std::move(reader));
  REQUIRE(volume.IsVolumeOK());

  const NTFS_BROWSER_NAMESPACE::Io::FileRecord<S> record(volume);

  // A defect here can abort the whole process, not just fail this check.
  CHECK_FALSE(record.IsDeleted());
  CHECK_FALSE(record.IsDirectory());
}
