#include <ntfs-browser/win-types.h>

#include <optional>
#include <span>

#include <catch2/catch_test_macros.hpp>

#include <ntfs-browser/cache/strategy.h>
#include <ntfs-browser/index-entry.h>
#include <ntfs-browser/io/file-record.h>
#include <ntfs-browser/ntfs-volume.h>

#include "corpus-test-support.h"
#include "ntfs-samples-support.h"
#include "optional-access.h"
#include "partition-disk-reader.h"

using NtfsBrowser::IndexEntry;
using NtfsBrowser::NtfsVolume;
using NtfsBrowser::Io::FileRecord;
namespace Cache = NtfsBrowser::Cache;

TEST_CASE("Reads /2.txt and finds the $Repair PTRN artifact (ntfs-ptrn.raw)",
          "[ntfs-samples][integration][slack]") {
  NtfsBrowserTests::RequireCorpusImage(ptrn_image);

  const NtfsVolume<Cache::Strategy::NoCache> volume(
      OpenWholeDiskImage(ptrn_image, small_image_partition_offset));
  REQUIRE(volume.IsVolumeOK());

  NtfsBrowser::Io::FileRecord<Cache::Strategy::NoCache> root(volume);
  OpenRootDir(root);
  const std::optional<IndexEntry> entry = root.FindSubEntry(L"2.txt");
  REQUIRE(entry.has_value());
  CHECK(NtfsBrowserTests::Unwrap(entry).GetFileSize() > 0);

  // /2.txt's own cluster slack (past its real size, still inside its last
  // allocated cluster) isn't checked: ReadData() clamps every read to
  // real_size (AttrNonResident::ReadData), and the ReadMe gives no absolute
  // offset for it here (unlike ntfs-ramslack.raw, below) to read around that.
  CheckRepairStreamsHoldPtrnPattern(volume);
}

TEST_CASE(
    "Finds the RAM slack and cluster slack PTRN pattern (ntfs-ramslack.raw)",
    "[ntfs-samples][integration][slack]") {
  NtfsBrowserTests::RequireCorpusImage(ramslack_image);

  const NtfsVolume<Cache::Strategy::NoCache> volume(
      OpenWholeDiskImage(ramslack_image, small_image_partition_offset));
  REQUIRE(volume.IsVolumeOK());

  NtfsBrowser::Io::FileRecord<Cache::Strategy::NoCache> root(volume);
  OpenRootDir(root);
  REQUIRE(root.FindSubEntry(L"1.txt").has_value());

  // RAM slack: the tail of /1.txt's last written sector, past its own real
  // size but still inside the sector Windows wrote, holding whatever was on
  // disk before (the "PTRN" pattern).
  LARGE_INTEGER ram_slack_addr{.QuadPart = ram_slack_address};
  const std::optional<std::span<const BYTE>> ram_slack =
      volume.Read(ram_slack_addr, 4);
  REQUIRE(ram_slack.has_value());
  CHECK(MatchesPtrnPattern(NtfsBrowserTests::Unwrap(ram_slack)));

  // Cluster slack: the rest of the cluster past that same sector.
  LARGE_INTEGER cluster_slack_addr{.QuadPart = cluster_slack_address};
  const std::optional<std::span<const BYTE>> cluster_slack =
      volume.Read(cluster_slack_addr, 4);
  REQUIRE(cluster_slack.has_value());
  CHECK(MatchesPtrnPattern(NtfsBrowserTests::Unwrap(cluster_slack)));

  CheckRepairStreamsHoldPtrnPattern(volume);
}
