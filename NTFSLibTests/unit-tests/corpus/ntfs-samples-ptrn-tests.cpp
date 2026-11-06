#include <ntfs-browser/win-types.h>

#include <optional>
#include <span>

#include <catch2/catch_test_macros.hpp>

#include <ntfs-browser/cache/strategy.h>
#include <ntfs-browser/index-entry.h>
#include <ntfs-browser/io/file-record.h>
#include <ntfs-browser/namespace.h>
#include <ntfs-browser/ntfs-volume.h>

#include "corpus-test-support.h"
#include "ntfs-samples-support.h"
#include "optional-access.h"

using NTFS_BROWSER_NAMESPACE::IndexEntry;
using NTFS_BROWSER_NAMESPACE::NtfsVolume;
using NTFS_BROWSER_NAMESPACE::Io::FileRecord;
namespace Cache = NTFS_BROWSER_NAMESPACE::Cache;

TEST_CASE("Reads /2.txt and finds the $Repair PTRN artifact (ntfs-ptrn.raw)",
          "[ntfs-samples][integration][slack]") {
  NTFS_BROWSER_NAMESPACE::Tests::RequireCorpusImage(ptrn_image, "ntfs-ptrn");

  const NtfsVolume<Cache::Strategy::NoCache> volume(
      NTFS_BROWSER_NAMESPACE::Tests::OpenCorpusImage(
          ptrn_image, small_image_partition_offset, "ntfs-ptrn"));
  REQUIRE(volume.IsVolumeOK());

  NTFS_BROWSER_NAMESPACE::Io::FileRecord<Cache::Strategy::NoCache> root(volume);
  OpenRootDir(root);
  const std::optional<IndexEntry> entry = root.FindSubEntry(L"2.txt");
  REQUIRE(entry.has_value());
  CHECK(NTFS_BROWSER_NAMESPACE::Tests::Unwrap(entry).GetFileSize() > 0);

  // /2.txt's own cluster slack (past its real size, still inside its last
  // allocated cluster) isn't checked: ReadData() clamps every read to
  // real_size (AttrNonResident::ReadData), and the ReadMe gives no absolute
  // offset for it here (unlike ntfs-ramslack.raw, below) to read around that.
  CheckRepairStreamsHoldPtrnPattern(volume);
}

TEST_CASE(
    "Finds the RAM slack and cluster slack PTRN pattern (ntfs-ramslack.raw)",
    "[ntfs-samples][integration][slack]") {
  NTFS_BROWSER_NAMESPACE::Tests::RequireCorpusImage(ramslack_image,
                                                    "ntfs-ramslack");

  const NtfsVolume<Cache::Strategy::NoCache> volume(
      NTFS_BROWSER_NAMESPACE::Tests::OpenCorpusImage(
          ramslack_image, small_image_partition_offset, "ntfs-ramslack"));
  REQUIRE(volume.IsVolumeOK());

  NTFS_BROWSER_NAMESPACE::Io::FileRecord<Cache::Strategy::NoCache> root(volume);
  OpenRootDir(root);
  REQUIRE(root.FindSubEntry(L"1.txt").has_value());

  // RAM slack: the tail of /1.txt's last written sector, past its own real
  // size but still inside the sector Windows wrote, holding whatever was on
  // disk before (the "PTRN" pattern).
  LARGE_INTEGER ram_slack_addr{.QuadPart = ram_slack_address};
  const std::optional<std::span<const BYTE>> ram_slack =
      volume.Read(ram_slack_addr, 4);
  REQUIRE(ram_slack.has_value());
  CHECK(MatchesPtrnPattern(NTFS_BROWSER_NAMESPACE::Tests::Unwrap(ram_slack)));

  // Cluster slack: the rest of the cluster past that same sector.
  LARGE_INTEGER cluster_slack_addr{.QuadPart = cluster_slack_address};
  const std::optional<std::span<const BYTE>> cluster_slack =
      volume.Read(cluster_slack_addr, 4);
  REQUIRE(cluster_slack.has_value());
  CHECK(
      MatchesPtrnPattern(NTFS_BROWSER_NAMESPACE::Tests::Unwrap(cluster_slack)));

  CheckRepairStreamsHoldPtrnPattern(volume);
}
