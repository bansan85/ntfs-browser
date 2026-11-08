#include <ntfs-browser/win-types.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstring>
#include <span>
#include <stdexcept>
#include <vector>

#include <gsl/narrow>

#include <ntfs-browser/namespace.h>
#include <ntfs-browser/tool/fuzzgen/boot-sector-layer.h>
#include <ntfs-browser/tool/fuzzgen/layer-line.h>

#include "data/ntfs-bpb.h"

namespace NTFS_BROWSER_NAMESPACE::Tool::FuzzGen {

namespace {

namespace Data = NTFS_BROWSER_NAMESPACE::Data;

// The jump that opens every Windows boot sector: JMP +0x52, then a NOP.
constexpr std::array<BYTE, 3> windows_jump = {0xEB, 0x52, 0x90};

// Windows 10 formats a disk with 512-byte sectors, 4 KiB clusters, the fixed
// disk media byte and the geometry of a large disk, and it hides the sectors
// before the partition (1 MiB of alignment minus the 63 of the old tools).
constexpr ULONGLONG default_bytes_per_sector = 512;
constexpr ULONGLONG default_sectors_per_cluster = 8;
constexpr ULONGLONG default_media = 0xF8;
constexpr ULONGLONG default_sectors_per_track = 63;
constexpr ULONGLONG default_heads = 255;
constexpr ULONGLONG default_hidden = 128;

// Bytes 0x24 to 0x27, which NTFS does not use. Windows 10 fills them with the
// value of a FAT boot sector for a hard disk (80 00 80 00). Windows XP leaves
// the last two bytes zero.
constexpr DWORD windows_unused_marker = 0x00800080;

// The size of a file record, as the signed byte count of 1 KiB records: 2 to
// the power of 10, written as minus 10 (0xF6).
constexpr ULONGLONG default_record_size = 0xF6;

// One cluster per index block.
constexpr ULONGLONG default_index_size = 1;

}  // namespace

std::vector<BYTE> BootSectorLayer::Render(const LayerLine& line,
                                          std::span<const BYTE> code) {
  constexpr std::size_t code_offset = offsetof(Data::NtfsBpb, code);
  Data::NtfsBpb bpb{};
  std::ranges::copy(windows_jump, std::begin(bpb.jmp));
  std::memcpy(bpb.signature, Data::ntfs_signature.data(),
              Data::bpb_signature_size);
  bpb.bytes_per_sector =
      gsl::narrow<WORD>(line.Number("bps", default_bytes_per_sector));
  bpb.sectors_per_cluster =
      gsl::narrow<BYTE>(line.Number("spc", default_sectors_per_cluster));
  bpb.reserved_sectors = gsl::narrow<WORD>(line.Number("reserved", 0));
  bpb.media_descriptor = gsl::narrow<BYTE>(line.Number("media", default_media));
  bpb.sectors_per_track =
      gsl::narrow<WORD>(line.Number("spt", default_sectors_per_track));
  bpb.number_of_heads = gsl::narrow<WORD>(line.Number("heads", default_heads));
  bpb.hidden_sectors =
      gsl::narrow<DWORD>(line.Number("hidden", default_hidden));
  bpb.not_used3 =
      gsl::narrow<DWORD>(line.Number("unused", windows_unused_marker));
  bpb.total_sectors = line.Number("total");
  bpb.lcn_mft = line.Number("mft");
  bpb.lcn_mft_mirr = line.Number("mirror");
  bpb.clusters_per_file_record =
      gsl::narrow<DWORD>(line.Number("record", default_record_size));
  bpb.clusters_per_index_block =
      gsl::narrow<DWORD>(line.Number("index", default_index_size));
  const std::vector<BYTE> serial = line.Bytes("serial");
  if (serial.size() != Data::bpb_volume_serial_size) {
    throw std::runtime_error("the volume serial is not 8 bytes: " +
                             line.Source());
  }
  std::ranges::copy(serial, std::begin(bpb.volume_sn));

  std::vector<BYTE> bytes(code_offset + code.size());
  std::memcpy(bytes.data(), &bpb, code_offset);
  std::ranges::copy(code,
                    bytes.begin() + static_cast<std::ptrdiff_t>(code_offset));
  return bytes;
}

}  // namespace NTFS_BROWSER_NAMESPACE::Tool::FuzzGen
