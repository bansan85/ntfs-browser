#include "update-sequence.h"

#include <cstddef>
#include <cstring>
#include <vector>

#include <ntfs-browser/namespace.h>

#include "win-types.h"

namespace NTFS_BROWSER_NAMESPACE::Tool::FuzzGen {

void UpdateSequence::Protect(std::vector<BYTE>& bytes, std::size_t array_offset,
                             WORD number) {
  std::memcpy(&bytes.at(array_offset), &number, sizeof(WORD));
  for (std::size_t sector = 0; sector < bytes.size() / sector_bytes; ++sector) {
    const std::size_t end = ((sector + 1) * sector_bytes) - sizeof(WORD);
    const std::size_t slot = array_offset + ((sector + 1) * sizeof(WORD));
    std::memcpy(&bytes.at(slot), &bytes.at(end), sizeof(WORD));
    std::memcpy(&bytes.at(end), &number, sizeof(WORD));
  }
}

}  // namespace NTFS_BROWSER_NAMESPACE::Tool::FuzzGen
