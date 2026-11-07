#include <ntfs-browser/win-types.h>

#include <cstddef>
#include <cstring>
#include <vector>

#include <ntfs-browser/namespace.h>
#include <ntfs-browser/test/fake-image/fake-ntfs-image.h>
#include <ntfs-browser/tool/fuzzgen/bpb-edit.h>

#include "data/ntfs-bpb.h"

namespace NTFS_BROWSER_NAMESPACE::Tool::FuzzGen {

std::vector<BYTE> BpbEdit::BaseBootSector() {
  std::vector<BYTE> image = Test::FakeImage::BuildFakeNtfsImage();
  image.resize(boot_sector_bytes);
  return image;
}

void BpbEdit::Apply(std::vector<BYTE>& bytes, const Change& change) {
  static_assert(sizeof(Data::NtfsBpb) == boot_sector_bytes);
  // at() throws when the bytes are too short for a boot sector.
  static_cast<void>(bytes.at(boot_sector_bytes - 1));
  Data::NtfsBpb bpb{};
  std::memcpy(&bpb, bytes.data(), sizeof(bpb));
  change(bpb);
  std::memcpy(bytes.data(), &bpb, sizeof(bpb));
}

}  // namespace NTFS_BROWSER_NAMESPACE::Tool::FuzzGen
