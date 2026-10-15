#include <ntfs-browser/win-types.h>

#include <filesystem>

#include <catch2/catch_test_macros.hpp>

#include <ntfs-browser/cache/strategy.h>
#include <ntfs-browser/io/file-record.h>
#include <ntfs-browser/namespace.h>
#include <ntfs-browser/ntfs-volume.h>

#include "catch2/catch_message.hpp"
#include "corpus-test-support.h"
#include "ntfs-samples-support.h"

using NTFS_BROWSER_NAMESPACE::NtfsVolume;
using NTFS_BROWSER_NAMESPACE::Io::FileRecord;
namespace Cache = NTFS_BROWSER_NAMESPACE::Cache;

TEST_CASE(
    "Resolves every file record of an extremely fragmented $MFT "
    "(ntfs_extremely_fragmented_mft.raw)",
    "[ntfs-samples][integration][fragmented-mft]") {
  const std::filesystem::path& image =
      NTFS_BROWSER_NAMESPACE::Tests::FragmentedMftImage();
  NTFS_BROWSER_NAMESPACE::Tests::RequireUnfetchedCorpusImage(
      image, "ntfs-fragmented-mft");

  const NtfsVolume<Cache::Strategy::NoCache> volume(
      NTFS_BROWSER_NAMESPACE::Tests::OpenCorpusImage(
          image, large_image_partition_offset, "ntfs-fragmented-mft"));
  REQUIRE(volume.IsVolumeOK());

  // ReadMe.md: these are the file records ($MFT's own base record, plus its
  // extension records) whose attributes make up $MFT's own, heavily
  // fragmented $DATA run list. Resolving each one exercises that fragmented
  // run list, through NtfsVolume::ReadMftData()/mft_extents_.
  for (const ULONGLONG record_num :
       {0ULL, 15ULL, 16ULL, 17ULL, 18ULL, 19ULL, 20ULL, 21ULL, 22ULL,
        34'799'617ULL, 34'799'618ULL, 34'799'619ULL}) {
    INFO("record " << record_num);
    NTFS_BROWSER_NAMESPACE::Io::FileRecord<Cache::Strategy::NoCache> record(
        volume);
    REQUIRE(record.ParseFileRecord(record_num));
    CHECK(record.GetBaseRecordReference() == 0);
  }
}
