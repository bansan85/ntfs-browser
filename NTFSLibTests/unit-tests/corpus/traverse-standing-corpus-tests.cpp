#include <ntfs-browser/win-types.h>

#include <array>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <catch2/catch_template_test_macros.hpp>
#include <catch2/catch_test_macros.hpp>

#include <ntfs-browser/cache/strategy.h>
#include <ntfs-browser/disk-reader.h>
#include <ntfs-browser/namespace.h>
#include <ntfs-browser/ntfs-volume.h>
#include <ntfs-browser/volume-options.h>

#include "catch2/catch_message.hpp"
#include "corpus-test-support.h"
#include "memory-disk-reader.h"
#include "nps-ntfs1-test-support.h"
#include "walk-comparison.h"

namespace Cache = NTFS_BROWSER_NAMESPACE::Cache;
using NTFS_BROWSER_NAMESPACE::IDiskReader;
using NTFS_BROWSER_NAMESPACE::NtfsVolume;
using NTFS_BROWSER_NAMESPACE::VolumeOptions;
using NTFS_BROWSER_NAMESPACE::Tests::DfirNtfsDir;
using NTFS_BROWSER_NAMESPACE::Tests::MemoryDiskReader;
using NTFS_BROWSER_NAMESPACE::Tests::WalkResult;
using NTFS_BROWSER_NAMESPACE::Tests::WalkVolume;

namespace {

// Opens one more reader on the same image.
using OpenImage = std::function<std::unique_ptr<IDiskReader>()>;

// Walks the volume that open gives both ways, strict and recovering, with and
// without the deleted items, and checks that the batched walk shows the same
// entries, in the same order, with the same standing, and logs the same lines.
// The corpus traces cannot stand in for the image: they replay the reads of the
// default walk, in its order, and the batched walk reads in another.
template <Cache::Strategy S>
void CheckImageWalks(const OpenImage& open) {
  for (const bool include_deleted : {false, true}) {
    for (const bool recover_errors : {false, true}) {
      INFO("include_deleted " << include_deleted << ", recover_errors "
                              << recover_errors);
      const VolumeOptions options{.include_deleted = include_deleted,
                                  .recover_errors = recover_errors};
      const NtfsVolume<S> plain(open(), options);
      const NtfsVolume<S> batched(open(), options);
      REQUIRE(plain.IsVolumeOK());
      REQUIRE(batched.IsVolumeOK());

      const WalkResult expected = WalkVolume(plain, false);
      const WalkResult actual = WalkVolume(batched, true);
      CHECK_FALSE(actual.entries.empty());
      CHECK(actual.entries == expected.entries);
      CHECK(actual.log == expected.log);
    }
  }
}

}  // namespace

TEMPLATE_TEST_CASE_SIG(
    "The batched walk matches the default walk on the NPS image",
    "[corpus][traverse-standing]", ((Cache::Strategy S), S),
    Cache::Strategy::NoCache, Cache::Strategy::FullCache) {
  NTFS_BROWSER_NAMESPACE::Tests::RequireCorpusImage(
      NTFS_BROWSER_NAMESPACE::Tests::ntfs1_image);

  CheckImageWalks<S>(NTFS_BROWSER_NAMESPACE::Tests::OpenNtfs1Image);
}

TEMPLATE_TEST_CASE_SIG(
    "The batched walk matches the default walk on a volume with attribute "
    "lists",
    "[corpus][traverse-standing]", ((Cache::Strategy S), S),
    Cache::Strategy::NoCache, Cache::Strategy::FullCache) {
  const std::filesystem::path image =
      NTFS_BROWSER_NAMESPACE::Tests::RequireCorpusFile(
          DfirNtfsDir() / "test_data" / "ntfs3_attrlist.raw");
  const std::vector<BYTE> bytes =
      NTFS_BROWSER_NAMESPACE::Tests::ReadCorpusFile(image);

  CheckImageWalks<S>(
      [&bytes] { return std::make_unique<MemoryDiskReader>(bytes); });
}
