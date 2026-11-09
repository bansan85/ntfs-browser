#include "boot-sector-scenarios.h"

#include <cstddef>
#include <string>
#include <utility>
#include <vector>

#include <ntfs-browser/namespace.h>
#include <ntfs-browser/test/fake-image/fake-ntfs-image.h>
#include <ntfs-browser/tool/fuzzgen/recorded-stream.h>
#include <ntfs-browser/tool/fuzzgen/scenario-registry.h>
#include <ntfs-browser/tool/fuzzgen/scenario.h>

#include "bpb-edit.h"
#include "data/ntfs-bpb.h"
#include "resource-locator.h"
#include "win-types.h"

namespace NTFS_BROWSER_NAMESPACE::Tool::FuzzGen {

namespace {

namespace FakeImage = Test::FakeImage;
using Data::NtfsBpb;

// Bytes of the base volume that the inputs keep: the boot sector, $MFT record
// 0, the empty slots, record 3 ($Volume), and the first bytes of record 5. The
// saved inputs end there. The fuzzer replays them in a loop.
constexpr std::size_t volume_start_bytes = 6167;

// The flags word of $Volume with only the dirty bit set.
constexpr WORD dirty_flag = 0x0001;

// A sector size that is not a power of two, and two that no volume has: below
// and above the range of sizes the library accepts.
constexpr WORD odd_sector_size = 0x0180;
constexpr WORD tiny_sector_size = 0x0080;
constexpr WORD huge_sector_size = 0x2000;

// One saved input: its file name, and the field it damages.
struct Damage {
  std::string name;
  BpbEdit::Change change;
};

// The fields a real NTFS boot sector keeps at zero, the geometry it keeps in
// powers of two, and the marker that ends it.
std::vector<Damage> Damages() {
  return {
      {"boot_sector_fat_count_nonzero",
       [](NtfsBpb& bpb) { bpb.zeros1[0] = 2; }},
      {"boot_sector_reserved_sectors_nonzero",
       [](NtfsBpb& bpb) { bpb.reserved_sectors = 8; }},
      {"boot_sector_large_sectors_nonzero",
       [](NtfsBpb& bpb) { bpb.not_used2 = 1; }},
      {"boot_sector_total_sectors_zero",
       [](NtfsBpb& bpb) { bpb.total_sectors = 0; }},
      // The sector count is too small to hold the $MFT.
      {"boot_sector_mft_beyond_total_sectors",
       [](NtfsBpb& bpb) { bpb.total_sectors = 2; }},
      {"boot_sector_missing_aa55_marker",
       [](NtfsBpb& bpb) {
         bpb.total_sectors = 0;
         bpb.x_aa = 0;
         bpb.x_55 = 0;
       }},
      {"boot_sector_sector_size_above_range",
       [](NtfsBpb& bpb) { bpb.bytes_per_sector = huge_sector_size; }},
      {"boot_sector_sector_size_not_power_of_two",
       [](NtfsBpb& bpb) { bpb.bytes_per_sector = odd_sector_size; }},
      {"boot_sector_sector_size_out_of_range",
       [](NtfsBpb& bpb) { bpb.bytes_per_sector = tiny_sector_size; }},
      {"boot_sector_sectors_per_cluster_not_power_of_two",
       [](NtfsBpb& bpb) { bpb.sectors_per_cluster = 3; }},
  };
}

// The start of the base volume, with $Volume's flags word set to flags.
std::vector<BYTE> VolumeStart(WORD flags) {
  std::vector<BYTE> image = FakeImage::BuildFakeNtfsImageWithVolumeFlags(flags);
  image.resize(volume_start_bytes);
  return image;
}

}  // namespace

void BootSectorScenarios::Register(ScenarioRegistry& registry) {
  for (Damage& damage : Damages()) {
    registry.Add({.name = damage.name,
                  .make = [change = std::move(damage.change)](
                              const ResourceLocator& /*resources*/) {
                    std::vector<BYTE> bytes = VolumeStart(0);
                    BpbEdit::Apply(bytes, change);
                    return bytes;
                  }});
  }

  // The dirty bit is the only difference from a clean volume.
  registry.Add({.name = "volume_dirty_flag_set",
                .make = [](const ResourceLocator& /*resources*/) {
                  return VolumeStart(dirty_flag);
                }});

  // The first boot sector is damaged, the backup at the end of the volume is
  // not: the volume opens from the copy.
  registry.AddRecorded(
      "boot_sector_backup_fallback",
      [] {
        return FakeImage::BuildFakeNtfsImageWithBackupBootSector(
            {.sector0 = FakeImage::BootSectorDamage::SectorSize});
      },
      RecordedStream::Recovering());
}

}  // namespace NTFS_BROWSER_NAMESPACE::Tool::FuzzGen
