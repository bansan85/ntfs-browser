#include <ntfs-browser/win-types.h>

#include <limits>
#include <span>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include <ntfs-browser/namespace.h>

#include "data/ntfs-bpb.h"
#include "fake-ntfs-image.h"
#include "volume/volume-geometry.h"

namespace Tests = NTFS_BROWSER_NAMESPACE::Tests;
namespace Volume = NTFS_BROWSER_NAMESPACE::Volume;

namespace {

// The fake image's cluster size, as ParseBootSector() decodes it.
constexpr ULONGLONG cluster_size = Tests::fake_cluster_size;

// The fake image's $MFT address: one cluster in.
constexpr ULONGLONG fake_mft_addr = cluster_size;

// Bytes the 4 mirrored records span.
constexpr ULONGLONG copy_bytes =
    Tests::fake_mft_mirror_records * Tests::fake_file_record_size;

// The boot sector of the fake image with a mirror, edited by edit.
template <typename Edit>
std::vector<BYTE> BootSector(Edit edit) {
  std::vector<BYTE> image = Tests::BuildFakeNtfsImageWithMftMirror();
  edit(*reinterpret_cast<NTFS_BROWSER_NAMESPACE::Data::NtfsBpb*>(image.data()));
  image.resize(Tests::fake_bytes_per_sector);
  return image;
}

Volume::BootSectorParse Parse(const std::vector<BYTE>& boot_sector,
                              bool recover = false) {
  return Volume::ParseBootSector(boot_sector, recover);
}

}  // namespace

TEST_CASE("ParseBootSector decodes the $MFTMirr address", "[boot-sector]") {
  SECTION("a mirror inside the volume is usable") {
    const Volume::BootSectorParse parse = Parse(BootSector([](auto&) {}));

    CHECK(parse.ok);
    CHECK(parse.geometry.mft_mirr_addr ==
          Tests::fake_mft_mirror_lcn * cluster_size);
    CHECK(parse.geometry.mft_mirr_usable);
  }

  SECTION("a null lcn_mft_mirr has no mirror and no error") {
    const Volume::BootSectorParse parse =
        Parse(BootSector([](auto& bpb) { bpb.lcn_mft_mirr = 0; }));

    CHECK(parse.ok);
    CHECK(parse.geometry.mft_mirr_addr == 0);
    CHECK_FALSE(parse.geometry.mft_mirr_usable);
  }

  SECTION("the default fake image declares no mirror") {
    std::vector<BYTE> boot_sector = Tests::BuildFakeNtfsImage();
    boot_sector.resize(Tests::fake_bytes_per_sector);
    const Volume::BootSectorParse parse = Parse(boot_sector);

    CHECK(parse.ok);
    CHECK_FALSE(parse.geometry.mft_mirr_usable);
  }

  SECTION("a mirror at the address of $MFT is not usable") {
    const Volume::BootSectorParse parse =
        Parse(BootSector([](auto& bpb) { bpb.lcn_mft_mirr = bpb.lcn_mft; }));

    CHECK(parse.ok);
    CHECK(parse.geometry.mft_mirr_addr == fake_mft_addr);
    CHECK_FALSE(parse.geometry.mft_mirr_usable);
  }

  SECTION("an address that overflows saturates and is not usable") {
    const Volume::BootSectorParse parse = Parse(BootSector([](auto& bpb) {
      bpb.lcn_mft_mirr = std::numeric_limits<ULONGLONG>::max();
    }));

    CHECK(parse.ok);
    CHECK(parse.geometry.mft_mirr_addr ==
          std::numeric_limits<ULONGLONG>::max());
    CHECK_FALSE(parse.geometry.mft_mirr_usable);
  }

  SECTION("the largest product that does not overflow is not usable") {
    const Volume::BootSectorParse parse = Parse(BootSector([](auto& bpb) {
      bpb.lcn_mft_mirr = std::numeric_limits<ULONGLONG>::max() / cluster_size;
    }));

    CHECK(parse.ok);
    CHECK(parse.geometry.mft_mirr_addr ==
          std::numeric_limits<ULONGLONG>::max() / cluster_size * cluster_size);
    CHECK_FALSE(parse.geometry.mft_mirr_usable);
  }

  SECTION("an address too high for a LONGLONG is not usable") {
    const Volume::BootSectorParse parse = Parse(BootSector([](auto& bpb) {
      bpb.lcn_mft_mirr = (1ULL << 53U);
      bpb.total_sectors = std::numeric_limits<ULONGLONG>::max();
    }));

    CHECK(parse.ok);
    CHECK_FALSE(parse.geometry.mft_mirr_usable);
  }

  SECTION("a mirror whose 4 records end exactly at the volume's end") {
    const ULONGLONG volume_bytes =
        ULONGLONG{Tests::fake_bytes_per_sector} * 128;
    const ULONGLONG last_lcn = (volume_bytes - copy_bytes) / cluster_size;

    const Volume::BootSectorParse fits =
        Parse(BootSector([&](auto& bpb) { bpb.lcn_mft_mirr = last_lcn; }));
    CHECK(fits.ok);
    CHECK(fits.geometry.mft_mirr_usable);

    const Volume::BootSectorParse beyond =
        Parse(BootSector([&](auto& bpb) { bpb.lcn_mft_mirr = last_lcn + 1; }));
    CHECK(beyond.ok);
    CHECK(beyond.geometry.mft_mirr_addr == (last_lcn + 1) * cluster_size);
    CHECK_FALSE(beyond.geometry.mft_mirr_usable);
  }

  SECTION("an address past the volume is not usable") {
    const Volume::BootSectorParse parse =
        Parse(BootSector([](auto& bpb) { bpb.lcn_mft_mirr = 1000000; }));

    CHECK(parse.ok);
    CHECK_FALSE(parse.geometry.mft_mirr_usable);
  }

  SECTION("a null total_sectors leaves the volume's end unknown") {
    const auto edit = [](auto& bpb) {
      bpb.total_sectors = 0;
      bpb.lcn_mft_mirr = 1000000;
    };

    CHECK_FALSE(Parse(BootSector(edit)).ok);

    const Volume::BootSectorParse recovered = Parse(BootSector(edit), true);
    CHECK(recovered.ok);
    CHECK(recovered.geometry.mft_mirr_addr == 1000000 * cluster_size);
    CHECK(recovered.geometry.mft_mirr_usable);
  }

  SECTION("a rejected boot sector decodes no mirror") {
    const Volume::BootSectorParse parse =
        Parse(BootSector([](auto& bpb) { bpb.sectors_per_cluster = 3; }));

    CHECK_FALSE(parse.ok);
    CHECK(parse.geometry.mft_mirr_addr == 0);
    CHECK_FALSE(parse.geometry.mft_mirr_usable);
  }
}
