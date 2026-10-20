#include <algorithm>
#include <cstddef>
#include <cstring>
#include <vector>

#include <ntfs-browser/namespace.h>

#include "data/ntfs-bpb.h"
#include "fake-ntfs-image-internal.h"

namespace NTFS_BROWSER_NAMESPACE::Tests {

namespace {

// A boot sector spans 512 bytes, whatever the sector size.
constexpr size_t boot_sector_bytes = 512;

// Smallest cluster the builder uses. A record and an index block are one
// cluster, as in BuildFakeNtfsImage().
constexpr DWORD min_cluster_bytes = 1024;

// A sector size that is not a power of two.
constexpr WORD odd_sector_size = 3000;

// Breaks the boot sector at offset in image.
void Damage(std::vector<BYTE>& image, size_t offset, BootSectorDamage kind) {
  switch (kind) {
    case BootSectorDamage::None:
      break;
    case BootSectorDamage::Zeroed:
      std::memset(&image.at(offset), 0, boot_sector_bytes);
      break;
    case BootSectorDamage::Signature:
      std::memset(&image.at(offset + offsetof(Data::NtfsBpb, signature)), 0,
                  sizeof(Data::NtfsBpb::signature));
      break;
    case BootSectorDamage::SectorSize: {
      const WORD odd = odd_sector_size;
      std::memcpy(&image.at(offset + offsetof(Data::NtfsBpb, bytes_per_sector)),
                  &odd, sizeof(odd));
      break;
    }
  }
}

}  // namespace

std::vector<BYTE>
    BuildFakeNtfsImageWithBackupBootSector(const BackupBootSector& spec) {
  std::vector<BYTE> image = BuildFakeNtfsImage();
  auto bpb = *reinterpret_cast<Data::NtfsBpb*>(image.data());

  const DWORD cluster = std::max<DWORD>(min_cluster_bytes, spec.sector_size);
  bpb.bytes_per_sector = spec.sector_size;
  bpb.sectors_per_cluster = static_cast<BYTE>(cluster / spec.sector_size);

  image.resize(spec.image_bytes, 0);
  bpb.total_sectors = spec.image_bytes / spec.sector_size;
  std::memcpy(image.data(), &bpb, sizeof(bpb));

  const size_t placed_bytes = std::max<size_t>(
      spec.placed_for.value_or(spec.sector_size), boot_sector_bytes);
  const size_t backup_offset = image.size() - placed_bytes;
  std::memcpy(&image.at(backup_offset), &bpb, sizeof(bpb));

  Damage(image, 0, spec.sector0);
  Damage(image, backup_offset, spec.backup);
  return image;
}

}  // namespace NTFS_BROWSER_NAMESPACE::Tests
