#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include <ntfs-browser/cache/strategy.h>
#include <ntfs-browser/index-entry.h>
#include <ntfs-browser/io/file-record.h>
#include <ntfs-browser/namespace.h>
#include <ntfs-browser/ntfs-volume.h>

#include "ntfs-samples-support.h"

using NTFS_BROWSER_NAMESPACE::IndexEntryView;
using NTFS_BROWSER_NAMESPACE::NtfsVolume;
using NTFS_BROWSER_NAMESPACE::Io::FileRecord;
namespace Cache = NTFS_BROWSER_NAMESPACE::Cache;

TEST_CASE("Opens a volume with 2 MiB clusters (ntfs-2m.raw)",
          "[ntfs-samples][integration]") {
  NTFS_BROWSER_NAMESPACE::Tests::RequireCorpusImage(k2m_image, "ntfs-2m");

  const NtfsVolume<Cache::Strategy::NoCache> volume(
      NTFS_BROWSER_NAMESPACE::Tests::OpenCorpusImage(
          k2m_image, small_image_partition_offset, "ntfs-2m"));
  REQUIRE(volume.IsVolumeOK());
  CHECK(volume.GetClusterSize() == 2097152);

  NTFS_BROWSER_NAMESPACE::Io::FileRecord<Cache::Strategy::NoCache> root(volume);
  OpenRootDir(root);

  std::vector<std::wstring> names;
  root.TraverseSubEntries(
      [](const IndexEntryView& index_entry, void* context) {
        static_cast<std::vector<std::wstring>*>(context)->emplace_back(
            index_entry.GetFilename());
      },
      &names);
  CHECK_FALSE(names.empty());
}
