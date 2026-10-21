#include <ntfs-browser/win-types.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <limits>
#include <memory>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/generators/catch_generators_range.hpp>

#include <ntfs-browser/cache/strategy.h>
#include <ntfs-browser/namespace.h>
#include <ntfs-browser/ntfs-volume.h>
#include <ntfs-browser/volume-options.h>

#include "corpus-test-support.h"
#include "data/ntfs-bpb.h"
#include "memory-disk-reader.h"
#include "volume/volume-geometry.h"

namespace {

namespace Cache = NTFS_BROWSER_NAMESPACE::Cache;
namespace Data = NTFS_BROWSER_NAMESPACE::Data;
namespace Volume = NTFS_BROWSER_NAMESPACE::Volume;
using NTFS_BROWSER_NAMESPACE::NtfsVolume;
using NTFS_BROWSER_NAMESPACE::VolumeOptions;
using NTFS_BROWSER_NAMESPACE::Tests::DfirMuseumDir;
using NTFS_BROWSER_NAMESPACE::Tests::DfirNtfsDir;
using NTFS_BROWSER_NAMESPACE::Tests::MemoryDiskReader;
using NTFS_BROWSER_NAMESPACE::Tests::ReadCorpusFile;
using NTFS_BROWSER_NAMESPACE::Tests::RequireCorpusFile;

// Bytes of a boot sector: the BPB the library parses.
constexpr std::size_t boot_sector_bytes = 512;

// Which kept project holds a fixture.
enum class Project : unsigned char { DfirNtfs, DfirMuseum };

// One real boot sector, with the geometry an independent reader (a small
// Python script, not this library) decoded from its bytes. The cluster size is
// the sector size times the sectors per cluster, where a raw byte above 0x80
// stands for 2^(256 - byte) sectors and 0x80 for itself, 128 sectors.
struct BootFixture {
  std::string_view label;
  Project project;
  std::string_view relative_path;
  std::uintmax_t file_size;
  WORD sector_size;
  DWORD cluster_size;
  DWORD file_record_size;
  DWORD index_block_size;
  ULONGLONG total_sectors;
  // $MFT and $MFTMirr in bytes: their LCN times the cluster size.
  ULONGLONG mft_addr;
  ULONGLONG mft_mirr_addr;
  // False for the one sector whose bytes 510 and 511 are zero.
  bool has_end_marker;
};

// A boot area the Windows formatter writes: 8 KiB, the BPB then boot code.
constexpr std::uintmax_t boot_area_file_size = 8192;

// The kept real boot sectors. The 64 KiB one has the raw byte 0x80, which
// means 128 sectors: its $MFT sits at LCN 49 152, the same 3 GiB as the 4 KiB
// and 128 KiB volumes of the same size, which only 128 sectors give.
constexpr std::array<BootFixture, 7> boot_fixtures = {{
    {"512-byte clusters", Project::DfirNtfs, "test_data/512.boot",
     boot_sector_bytes, 512, 512, 1024, 4096, 2091007, 356'865'024, 8192, true},
    {"4 KiB clusters", Project::DfirNtfs, "test_data/4k.boot",
     boot_sector_bytes, 512, 4096, 1024, 4096, 124'700'671, 3'221'225'472, 8192,
     true},
    {"64 KiB clusters", Project::DfirNtfs, "test_data/64k.boot",
     boot_sector_bytes, 512, 65536, 1024, 4096, 67'102'719, 3'221'225'472,
     65536, true},
    {"128 KiB clusters", Project::DfirNtfs, "test_data/128k.boot",
     boot_sector_bytes, 512, 131072, 1024, 4096, 67'102'719, 3'221'225'472,
     131072, true},
    {"4Kn sectors", Project::DfirNtfs, "test_data/4kn.boot", boot_sector_bytes,
     4096, 4096, 4096, 4096, 14335, 19'570'688, 8192, true},
    {"no boot code", Project::DfirNtfs, "test_data/nobootcode.boot",
     boot_sector_bytes, 512, 4096, 1024, 4096, 61386, 3158016, 15'712'256,
     false},
    {"Windows Server 2012 R2 $Boot", Project::DfirMuseum,
     "Windows/NTFSArtifacts/Boot/Win2012R2/StolenSzechuan/$Boot",
     boot_area_file_size, 512, 4096, 1024, 4096, 22'872'063, 3'221'225'472,
     8192, true},
}};

std::vector<BYTE> BootSectorOf(const BootFixture& fixture) {
  const std::filesystem::path& root =
      fixture.project == Project::DfirNtfs ? DfirNtfsDir() : DfirMuseumDir();
  const std::filesystem::path file =
      RequireCorpusFile(root / std::u8string(fixture.relative_path.begin(),
                                             fixture.relative_path.end()));
  std::vector<BYTE> bytes = ReadCorpusFile(file);
  REQUIRE(bytes.size() == fixture.file_size);
  bytes.resize(boot_sector_bytes);
  return bytes;
}

Data::NtfsBpb& BpbOf(std::vector<BYTE>& sector) {
  return *reinterpret_cast<Data::NtfsBpb*>(sector.data());
}

void CheckGeometry(const BootFixture& fixture,
                   const Volume::BootSectorParse& parse) {
  REQUIRE(parse.ok);
  CHECK(parse.geometry.sector_size == fixture.sector_size);
  CHECK(parse.geometry.cluster_size == fixture.cluster_size);
  CHECK(parse.geometry.file_record_size == fixture.file_record_size);
  CHECK(parse.geometry.index_block_size == fixture.index_block_size);
  CHECK(parse.geometry.mft_addr == fixture.mft_addr);
  CHECK(parse.geometry.mft_mirr_addr == fixture.mft_mirr_addr);
  CHECK(parse.geometry.mft_mirr_usable);
  CHECK(parse.geometry.cluster_count ==
        fixture.total_sectors * fixture.sector_size / fixture.cluster_size);
}

template <Cache::Strategy S>
void CheckVolumeGetters(const BootFixture& fixture,
                        const std::vector<BYTE>& sector) {
  // The sector alone holds no $MFT, so the volume does not come up OK. The
  // geometry it decoded stays readable.
  const NtfsVolume<S> volume(std::make_unique<MemoryDiskReader>(sector),
                             VolumeOptions{.recover_errors = true});

  CHECK(volume.GetSectorSize() == fixture.sector_size);
  CHECK(volume.GetClusterSize() == fixture.cluster_size);
  CHECK(volume.GetFileRecordSize() == fixture.file_record_size);
  CHECK(volume.GetIndexBlockSize() == fixture.index_block_size);
  CHECK(volume.GetMFTAddr() == fixture.mft_addr);
  CHECK(volume.GetClusterBuffer().size() == fixture.cluster_size);
}

}  // namespace

TEST_CASE("Real boot sectors decode to the independent geometry",
          "[dfir-corpora][integration][boot-sector]") {
  const BootFixture fixture = GENERATE(from_range(boot_fixtures));
  CAPTURE(fixture.label);
  const std::vector<BYTE> sector = BootSectorOf(fixture);

  const Volume::BootSectorParse recovering =
      Volume::ParseBootSector(sector, true);
  const Volume::BootSectorParse strict = Volume::ParseBootSector(sector, false);

  CheckGeometry(fixture, recovering);
  // Only the missing 0xAA55 marker makes a sector need recovery.
  CHECK(strict.ok == fixture.has_end_marker);
  if (fixture.has_end_marker) {
    CheckGeometry(fixture, strict);
  }
  CheckVolumeGetters<Cache::Strategy::NoCache>(fixture, sector);
  CheckVolumeGetters<Cache::Strategy::FullCache>(fixture, sector);
}

TEST_CASE("A real boot sector with one damaged field is rejected or recovered",
          "[dfir-corpora][integration][boot-sector]") {
  // A change to a copy of the 4 KiB volume's sector, and what each mode makes
  // of it.
  struct Change {
    std::string_view what;
    std::function<void(Data::NtfsBpb&)> edit;
    bool strict_ok;
    bool recover_ok;
  };

  const std::array<Change, 14> changes = {{
      {"the NTFS signature", [](auto& bpb) { bpb.signature[0] = 'X'; }, false,
       false},
      {"the 0xAA55 marker", [](auto& bpb) { bpb.x_aa = 0; }, false, true},
      {"a sector of 128 bytes", [](auto& bpb) { bpb.bytes_per_sector = 128; },
       false, false},
      {"a sector of 768 bytes", [](auto& bpb) { bpb.bytes_per_sector = 768; },
       false, false},
      {"no sector per cluster", [](auto& bpb) { bpb.sectors_per_cluster = 0; },
       false, false},
      {"3 sectors per cluster", [](auto& bpb) { bpb.sectors_per_cluster = 3; },
       false, false},
      {"sectors per cluster -13",
       [](auto& bpb) { bpb.sectors_per_cluster = 0xF3; }, false, false},
      {"no cluster per file record",
       [](auto& bpb) { bpb.clusters_per_file_record = 0; }, false, false},
      {"a file record of 127 clusters",
       [](auto& bpb) { bpb.clusters_per_file_record = 127; }, false, false},
      {"an index block of 2^16 bytes",
       [](auto& bpb) {
         bpb.clusters_per_index_block = static_cast<DWORD>(-16);
       },
       false, false},
      {"an $MFT address that overflows",
       [](auto& bpb) { bpb.lcn_mft = (std::numeric_limits<ULONGLONG>::max)(); },
       false, false},
      {"a volume of one sector", [](auto& bpb) { bpb.total_sectors = 1; },
       false, true},
      {"a reserved sector count", [](auto& bpb) { bpb.reserved_sectors = 1; },
       false, true},
      {"no total sectors", [](auto& bpb) { bpb.total_sectors = 0; }, false,
       true},
  }};
  const Change change = GENERATE_COPY(from_range(changes));
  CAPTURE(change.what);
  std::vector<BYTE> sector = BootSectorOf(boot_fixtures.at(1));
  change.edit(BpbOf(sector));

  CHECK(Volume::ParseBootSector(sector, false).ok == change.strict_ok);
  CHECK(Volume::ParseBootSector(sector, true).ok == change.recover_ok);
}

TEST_CASE("Every sectors_per_cluster between 0x81 and 0xF3 is rejected",
          "[dfir-corpora][integration][boot-sector]") {
  // Between the literal counts (up to 0x80) and the powers of two (-12 to -1)
  // there is no valid encoding.
  const int raw = GENERATE(range(0x81, 0xF4));
  CAPTURE(raw);
  std::vector<BYTE> sector = BootSectorOf(boot_fixtures.at(1));
  BpbOf(sector).sectors_per_cluster = static_cast<BYTE>(raw);

  CHECK_FALSE(Volume::ParseBootSector(sector, false).ok);
  CHECK_FALSE(Volume::ParseBootSector(sector, true).ok);
}

TEST_CASE("Real boot sectors keep their guards against overflow",
          "[dfir-corpora][integration][boot-sector]") {
  const BootFixture fixture = GENERATE(from_range(boot_fixtures));
  CAPTURE(fixture.label);
  const std::vector<BYTE> sector = BootSectorOf(fixture);
  constexpr ULONGLONG biggest = (std::numeric_limits<ULONGLONG>::max)();

  SECTION("an $MFT address one cluster past what a ULONGLONG holds") {
    std::vector<BYTE> forged = sector;
    BpbOf(forged).lcn_mft = biggest / fixture.cluster_size + 1;

    CHECK_FALSE(Volume::ParseBootSector(forged, false).ok);
    CHECK_FALSE(Volume::ParseBootSector(forged, true).ok);
  }

  SECTION("an $MFT address that fits but names no byte of a LONGLONG") {
    std::vector<BYTE> forged = sector;
    BpbOf(forged).lcn_mft = biggest / fixture.cluster_size;

    CHECK_FALSE(Volume::ParseBootSector(forged, false).ok);
    CHECK_FALSE(Volume::ParseBootSector(forged, true).ok);
  }

  SECTION("a $MFTMirr address that overflows only loses the mirror") {
    std::vector<BYTE> forged = sector;
    BpbOf(forged).lcn_mft_mirr = biggest;

    const Volume::BootSectorParse parse = Volume::ParseBootSector(forged, true);

    CHECK(parse.ok);
    CHECK_FALSE(parse.geometry.mft_mirr_usable);
  }
}
