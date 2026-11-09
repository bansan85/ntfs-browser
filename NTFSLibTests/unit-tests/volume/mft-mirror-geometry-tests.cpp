#include <array>
#include <limits>
#include <optional>
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
using Bpb = NTFS_BROWSER_NAMESPACE::Data::NtfsBpb;

namespace {

// The fake image's cluster size, as ParseBootSector() decodes it.
constexpr ULONGLONG cluster_size = TestFakeImage::fake_cluster_size;

// The fake image's $MFT address: one cluster in.
constexpr ULONGLONG fake_mft_addr = cluster_size;

// Bytes the 4 mirrored records span.
constexpr ULONGLONG copy_bytes = TestFakeImage::fake_mft_mirror_records *
                                 TestFakeImage::fake_file_record_size;

// The boot sector of the fake image with a mirror, edited by edit.
template <typename Edit>
std::vector<BYTE> BootSector(Edit edit) {
  std::vector<BYTE> image = TestFakeImage::BuildFakeNtfsImageWithMftMirror();
  edit(*reinterpret_cast<NTFS_BROWSER_NAMESPACE::Data::NtfsBpb*>(image.data()));
  image.resize(TestFakeImage::fake_bytes_per_sector);
  return image;
}

Volume::BootSectorParse Parse(const std::vector<BYTE>& boot_sector,
                              bool recover = false) {
  return Volume::ParseBootSector(boot_sector, recover);
}

// A boot sector edit, and what parsing the result gives for the mirror.
struct MirrorCase {
  const char* name;
  void (*edit)(Bpb&);
  bool ok;
  // The decoded address, when the case pins it.
  std::optional<ULONGLONG> addr;
  bool usable;
};

const std::array<MirrorCase, 7> unusable_mirrors{{
    {"a null lcn_mft_mirr has no mirror and no error",
     [](Bpb& bpb) { bpb.lcn_mft_mirr = 0; }, true, 0, false},
    {"a mirror at the address of $MFT is not usable",
     [](Bpb& bpb) { bpb.lcn_mft_mirr = bpb.lcn_mft; }, true, fake_mft_addr,
     false},
    {"an address that overflows saturates and is not usable",
     [](Bpb& bpb) { bpb.lcn_mft_mirr = std::numeric_limits<ULONGLONG>::max(); },
     true, std::numeric_limits<ULONGLONG>::max(), false},
    {"the largest product that does not overflow is not usable",
     [](Bpb& bpb) {
       bpb.lcn_mft_mirr = std::numeric_limits<ULONGLONG>::max() / cluster_size;
     },
     true, std::numeric_limits<ULONGLONG>::max() / cluster_size * cluster_size,
     false},
    {"an address too high for a LONGLONG is not usable",
     [](Bpb& bpb) {
       bpb.lcn_mft_mirr = (1ULL << 53U);
       bpb.total_sectors = std::numeric_limits<ULONGLONG>::max();
     },
     true, std::nullopt, false},
    {"an address past the volume is not usable",
     [](Bpb& bpb) { bpb.lcn_mft_mirr = 1000000; }, true, std::nullopt, false},
    {"a rejected boot sector decodes no mirror",
     [](Bpb& bpb) { bpb.sectors_per_cluster = 3; }, false, 0, false},
}};

}  // namespace

TEST_CASE("ParseBootSector decodes the $MFTMirr address", "[boot-sector]") {
  SECTION("a mirror inside the volume is usable") {
    const Volume::BootSectorParse parse = Parse(BootSector([](auto&) {}));

    CHECK(parse.ok);
    CHECK(parse.geometry.mft_mirr_addr ==
          TestFakeImage::fake_mft_mirror_lcn * cluster_size);
    CHECK(parse.geometry.mft_mirr_usable);
  }

  SECTION("the default fake image declares no mirror") {
    std::vector<BYTE> boot_sector = TestFakeImage::BuildFakeNtfsImage();
    boot_sector.resize(TestFakeImage::fake_bytes_per_sector);
    const Volume::BootSectorParse parse = Parse(boot_sector);

    CHECK(parse.ok);
    CHECK_FALSE(parse.geometry.mft_mirr_usable);
  }

  for (const MirrorCase& mirror : unusable_mirrors) {
    DYNAMIC_SECTION(mirror.name) {
      const Volume::BootSectorParse parse = Parse(BootSector(mirror.edit));

      CHECK(parse.ok == mirror.ok);
      if (mirror.addr) {
        CHECK(parse.geometry.mft_mirr_addr == *mirror.addr);
      }
      CHECK(parse.geometry.mft_mirr_usable == mirror.usable);
    }
  }

  SECTION("a mirror whose 4 records end exactly at the volume's end") {
    const ULONGLONG volume_bytes =
        ULONGLONG{TestFakeImage::fake_bytes_per_sector} * 128;
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
}
