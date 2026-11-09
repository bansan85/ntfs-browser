#pragma once

#include <span>
#include <vector>

#include <ntfs-browser/namespace.h>

#include "layer-line.h"
#include "win-types.h"

namespace NTFS_BROWSER_NAMESPACE::Tool::FuzzGen {

// The boot area of a volume that Windows formatted: the BPB, then the boot
// code that every volume of that Windows version shares. The BPB comes from
// named fields of a layers line.
class BootSectorLayer {
 public:
  // The bytes of the boot area. The line gives the geometry by name: bps,
  // spc, reserved, media, spt, heads, hidden, total, mft, mirror, record,
  // index (all numbers) and serial (8 bytes of hexadecimal text). Fields that
  // the line leaves out keep the value that Windows 10 writes. code holds
  // every byte from the end of the volume serial number on: the boot code of
  // the first sector and the sectors after it.
  [[nodiscard]] static std::vector<BYTE> Render(const LayerLine& line,
                                                std::span<const BYTE> code);
};

}  // namespace NTFS_BROWSER_NAMESPACE::Tool::FuzzGen
