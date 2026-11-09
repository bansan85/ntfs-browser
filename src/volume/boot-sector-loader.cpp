#include "volume/boot-sector-loader.h"

#include <array>
#include <optional>
#include <span>
#include <utility>

#include <ntfs-browser/namespace.h>

#include "bench/bench.h"
#include "bench/recorder.h"
#include "cache/strategy.h"
#include "cache/view.h"
#include "io/file-reader.h"
#include "log/ntfs-common.h"
#include "volume/volume-geometry.h"
#include "win-types.h"

namespace NTFS_BROWSER_NAMESPACE::Volume {

namespace {

// Sector sizes the backup boot sector is tried with, most common first. The
// copy sits in the volume's last sector, whose size is not known yet.
constexpr std::array<WORD, 4> backup_sector_sizes{512, 4096, 1024, 2048};

// Bytes of a boot sector, whatever the sector size. It is also the smallest
// candidate sector size, so every backup read holds a whole boot sector.
constexpr WORD boot_sector_bytes = 512;

// Largest backup candidate read: the biggest sector size tried.
constexpr WORD backup_read_max_bytes = 4096;

}  // namespace

template <Cache::Strategy S>
BootSectorLoader<S>::BootSectorLoader(const Io::FileReader<S>& reader,
                                      bool recover) noexcept
    : reader_(reader), recover_(recover) {}

template <Cache::Strategy S>
BootSectorParse BootSectorLoader<S>::Load() const {
  BootSectorParse first;
  {
    // Held until the parse is done: under FullCache it owns the bytes.
    const std::optional<Cache::ReadResult<S>> bpb_buffer = ReadBootSector();
    if (!bpb_buffer) {
      Log::Error("Read boot sector error");
    } else {
      first = ParseBootSector(*bpb_buffer, recover_);
    }
  }
  if (first.ok || !recover_) {
    return first;
  }
  if (std::optional<BootSectorParse> backup = ReadBackupBootSector()) {
    return *backup;
  }
  return first;
}

template <Cache::Strategy S>
std::optional<Cache::ReadResult<S>>
    BootSectorLoader<S>::ReadBootSector() const {
  const Bench::Detail::KindScope kind(Bench::ReadKind::Boot);
  // Smallest sector size NTFS uses. It is also the size of the BPB read.
  constexpr DWORD min_sector_size = 512;
  // Largest sector size NTFS supports (4Kn). Windows opens a volume handle
  // unbuffered, and then only accepts a read length that is a whole number
  // of the device's sectors.
  constexpr DWORD max_sector_size = 4096;
  LARGE_INTEGER fr_addr{.QuadPart = 0};
  std::optional<Cache::ReadResult<S>> bpb_buffer =
      reader_.Read(fr_addr, max_sector_size);
  if (!bpb_buffer) {
    // A backing file shorter than max_sector_size cannot serve that read.
    Log::Warn("Cannot read a {}-byte boot sector, retrying with {} bytes",
              max_sector_size, min_sector_size);
    fr_addr.QuadPart = 0;
    bpb_buffer = reader_.Read(fr_addr, min_sector_size);
  }
  return bpb_buffer;
}

template <Cache::Strategy S>
std::optional<BootSectorParse>
    BootSectorLoader<S>::ReadBackupBootSector() const {
  const std::optional<ULONGLONG> volume_bytes = reader_.Size();
  if (!volume_bytes || *volume_bytes < boot_sector_bytes) {
    Log::Info(
        "Cannot try the backup boot sector: the size of the volume is unknown "
        "or too small");
    return std::nullopt;
  }

  for (const WORD sector_size : backup_sector_sizes) {
    if (std::optional<BootSectorParse> parse =
            TryBackupSectorSize(*volume_bytes, sector_size)) {
      return parse;
    }
  }

  Log::Info("No usable backup boot sector");
  return std::nullopt;
}

template <Cache::Strategy S>
std::optional<BootSectorParse>
    BootSectorLoader<S>::TryBackupSectorSize(ULONGLONG volume_bytes,
                                             WORD sector_size) const {
  // The store MUST hold whole sectors, and the read MUST fit in it. The read
  // then starts on a sector boundary, which a raw device requires.
  if (volume_bytes % sector_size != 0 || volume_bytes < sector_size ||
      !std::in_range<LONGLONG>(volume_bytes - sector_size)) {
    return std::nullopt;
  }

  LARGE_INTEGER addr{};
  addr.QuadPart = static_cast<LONGLONG>(volume_bytes - sector_size);
  std::array<BYTE, backup_read_max_bytes> copy{};
  const std::span<BYTE> buffer{copy.data(), sector_size};
  if (!reader_.ReadInto(addr, buffer)) {
    return std::nullopt;
  }

  BootSectorParse parse =
      ParseBootSector(buffer.first<boot_sector_bytes>(), /*recover=*/true);
  // A copy that declares another sector size was not read at its own
  // boundary: it is garbage for this candidate.
  if (!parse.ok || parse.geometry.sector_size != sector_size) {
    return std::nullopt;
  }
  Log::Info("Boot sector is unusable: using the backup boot sector at 0x{:X}",
            addr.QuadPart);
  return parse;
}

template class BootSectorLoader<Cache::Strategy::NoCache>;
template class BootSectorLoader<Cache::Strategy::FullCache>;

}  // namespace NTFS_BROWSER_NAMESPACE::Volume
