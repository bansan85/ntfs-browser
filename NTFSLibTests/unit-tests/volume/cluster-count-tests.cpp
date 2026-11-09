#include <span>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include <ntfs-browser/namespace.h>
#include <ntfs-browser/test/fake-image/fake-ntfs-image.h>

#include "data/ntfs-bpb.h"
#include "volume/volume-geometry.h"
#include "win-types.h"

namespace TestFakeImage = NTFS_BROWSER_NAMESPACE::Test::FakeImage;
namespace Volume = NTFS_BROWSER_NAMESPACE::Volume;

namespace {

// Sectors in a fake cluster, as the fake image's BPB declares them.
constexpr ULONGLONG sectors_per_cluster =
    TestFakeImage::fake_sectors_per_cluster;

// Parses the fake image's boot sector with total_sectors set to the value.
Volume::BootSectorParse ParseWithTotalSectors(ULONGLONG total_sectors,
                                              bool recover) {
  std::vector<BYTE> boot_sector = TestFakeImage::BuildFakeNtfsImage();
  reinterpret_cast<NTFS_BROWSER_NAMESPACE::Data::NtfsBpb*>(boot_sector.data())
      ->total_sectors = total_sectors;
  boot_sector.resize(TestFakeImage::fake_bytes_per_sector);
  return Volume::ParseBootSector(boot_sector, recover);
}

}  // namespace

TEST_CASE("ParseBootSector decodes the cluster count", "[boot-sector]") {
  SECTION("a whole number of clusters") {
    const Volume::BootSectorParse parse =
        ParseWithTotalSectors(sectors_per_cluster * 2048, false);

    CHECK(parse.ok);
    CHECK(parse.geometry.cluster_count == 2048);
  }

  SECTION("a partial last cluster is not counted") {
    const Volume::BootSectorParse parse =
        ParseWithTotalSectors(sectors_per_cluster * 2048 + 1, false);

    CHECK(parse.ok);
    CHECK(parse.geometry.cluster_count == 2048);
  }

  SECTION("a null sector count leaves the count unknown when recovering") {
    const Volume::BootSectorParse parse = ParseWithTotalSectors(0, true);

    CHECK(parse.ok);
    CHECK(parse.geometry.cluster_count == 0);
  }

  SECTION("a rejected boot sector decodes no count") {
    const Volume::BootSectorParse parse = ParseWithTotalSectors(0, false);

    CHECK_FALSE(parse.ok);
    CHECK(parse.geometry.cluster_count == 0);
  }
}
