#include "tiny-stream-scenarios.h"

#include <ntfs-browser/win-types.h>

#include <string>
#include <utility>
#include <vector>

#include <ntfs-browser/namespace.h>
#include <ntfs-browser/tool/fuzzgen/scenario-registry.h>
#include <ntfs-browser/tool/fuzzgen/scenario.h>

#include "bpb-edit.h"
#include "data/ntfs-bpb.h"
#include "hex-bytes.h"
#include "resource-locator.h"

namespace NTFS_BROWSER_NAMESPACE::Tool::FuzzGen {

namespace {

using Data::NtfsBpb;

// The geometry of the hand made boot sectors: 1024-byte sectors, one per
// cluster, so a cluster count of 1 means one sector. The sector count is zero,
// and the end marker has its two bytes swapped. The fuzzer repairs both before
// it replays the input.
void HandMadeGeometry(NtfsBpb& bpb) {
  constexpr WORD sector_bytes = 1024;
  bpb.bytes_per_sector = sector_bytes;
  bpb.sectors_per_cluster = 1;
  bpb.total_sectors = 0;
  bpb.x_aa = 0xAA;
  bpb.x_55 = 0x55;
}

// A cluster count byte that decodes as a shift: negative, so the size is
// 2^(256 - value). 0xFF is 2^1 bytes, too small for a record.
constexpr DWORD shift_too_small = 0xFF;

// A shift of 31 (0xE1 is 256 - 31): the size overflows a 32-bit value.
constexpr DWORD shift_overflow = 0xE1;

// A count of 8 clusters of 1024 bytes: a file record larger than the library
// accepts.
constexpr DWORD too_many_clusters = 8;

// A sectors per cluster byte that decodes as a shift of 13 (0xF3 is 256 - 13).
constexpr BYTE sectors_per_cluster_shift = 0xF3;

// The high byte of the $MFT address that no sector count can hold.
constexpr ULONGLONG huge_mft_lcn = ULONGLONG{0x20} << 48U;

// One boot sector input: its file name, and its fields.
struct BootSectorInput {
  std::string name;
  bool hand_made_geometry;
  BpbEdit::Change change;
};

std::vector<BootSectorInput> BootSectorInputs() {
  return {
      {"boot_sectors_per_cluster_shift_out_of_range", false,
       [](NtfsBpb& bpb) {
         bpb.total_sectors = 0;
         bpb.x_aa = 0xAA;
         bpb.x_55 = 0x55;
         bpb.sectors_per_cluster = sectors_per_cluster_shift;
       }},
      {"file_record_size_invalid", true,
       [](NtfsBpb& bpb) { bpb.clusters_per_file_record = shift_too_small; }},
      {"file_record_size_shift_overflow", true,
       [](NtfsBpb& bpb) { bpb.clusters_per_file_record = shift_overflow; }},
      {"file_record_size_too_big", true,
       [](NtfsBpb& bpb) { bpb.clusters_per_file_record = too_many_clusters; }},
      {"index_block_size_invalid", true,
       [](NtfsBpb& bpb) { bpb.clusters_per_index_block = shift_too_small; }},
      {"index_block_size_shift_overflow", true,
       [](NtfsBpb& bpb) { bpb.clusters_per_index_block = shift_overflow; }},
      {"mft_addr_narrowing_error", true,
       [](NtfsBpb& bpb) { bpb.lcn_mft = huge_mft_lcn; }},
  };
}

// Crash input of 74 bytes that AFL found, kept as it was found. The bytes have
// no structure to name. The fix it covers: attributes whose types alias.
constexpr const char* attr_type_slot_aliasing_hex =
    "80004000 44444444 44444400 10010001 ffffffff 7f202020 204e4ef8 e5ffffff "
    "a2000000 000000ff ffff7100 0000fe00 000064e5 ff1f0000 00000000 00000000 "
    "f6f52000 46494c45 0000";

// A byte to change in a crash input.
struct BytePatch {
  std::size_t offset;
  BYTE value;
};

// invalid_header_common is attr_type_slot_aliasing with these seven bytes
// changed.
const std::vector<BytePatch> invalid_header_patches{
    {42, 0x60}, {47, 0xFF}, {48, 0xFF}, {49, 0xFF},
    {50, 0xFF}, {51, 0xFF}, {65, 0x53}};

// Crash input of 74 bytes that AFL found, kept as it was found. The fix it
// covers: a sector size too small for the update sequence math.
constexpr const char* sector_size_too_small_hex =
    "80004000 44444444 44010001 0001ffff 7f202020 204e4ef8 e5000000 00000000 "
    "00004444 ffff7f20 20200d4e 4ef8e5ff ffff5d00 00000000 007fffff ff190000 "
    "f6532000 46494c45 0002";

// Crash input of 137 bytes that AFL found, kept as it was found. The fix it
// covers: a cluster size of zero.
constexpr const char* cluster_size_null_hex =
    "4e4e0020 20200a20 ed202000 0200fa46 46464600 00538f8f 8f202000 8100ffff "
    "ffffffff 22007f46 46000053 20208f8f 8f262620 8fb6b626 262085b6 b6b6b6b6 "
    "b6b6b693 9300007f ff939393 c9104653 200a4e54 0000f601 20200b04 b6b6b6b6 "
    "b6797979 7946494c 45939393 93939393 93b68f46 0002124e 20072016 02000120 "
    "1666163c 470a2020 0a";

}  // namespace

void TinyStreamScenarios::Register(ScenarioRegistry& registry) {
  for (BootSectorInput& input : BootSectorInputs()) {
    registry.Add({.name = input.name,
                  .make = [input = std::move(input)](
                              const ResourceLocator& /*resources*/) {
                    std::vector<BYTE> bytes = BpbEdit::BaseBootSector();
                    if (input.hand_made_geometry) {
                      BpbEdit::Apply(bytes, HandMadeGeometry);
                    }
                    BpbEdit::Apply(bytes, input.change);
                    return bytes;
                  }});
  }

  registry.Add({.name = "attr_type_slot_aliasing",
                .make = [](const ResourceLocator& /*resources*/) {
                  return HexBytes::Decode(attr_type_slot_aliasing_hex);
                }});
  registry.Add({.name = "invalid_header_common",
                .make = [](const ResourceLocator& /*resources*/) {
                  std::vector<BYTE> bytes =
                      HexBytes::Decode(attr_type_slot_aliasing_hex);
                  for (const BytePatch& patch : invalid_header_patches) {
                    bytes.at(patch.offset) = patch.value;
                  }
                  return bytes;
                }});
  registry.Add({.name = "sector_size_too_small",
                .make = [](const ResourceLocator& /*resources*/) {
                  return HexBytes::Decode(sector_size_too_small_hex);
                }});
  registry.Add({.name = "cluster_size_null",
                .make = [](const ResourceLocator& /*resources*/) {
                  return HexBytes::Decode(cluster_size_null_hex);
                }});
}

}  // namespace NTFS_BROWSER_NAMESPACE::Tool::FuzzGen
