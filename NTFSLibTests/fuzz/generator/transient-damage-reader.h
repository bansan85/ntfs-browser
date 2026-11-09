#pragma once

#include <cstddef>
#include <mutex>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

#include <ntfs-browser/namespace.h>
#include <ntfs-browser/test/disk-readers/memory-disk-reader.h>

#include "disk-reader.h"
#include "win-types.h"

namespace NTFS_BROWSER_NAMESPACE::Tool::FuzzGen {

// Serves an image whose damage is seen by the first reads of the damaged bytes
// only. Every later read of them gets the intact bytes, as after a transient
// fault of the disk. A volume then rejects the damaged item once and reads the
// same item whole on its next visit, so the saved input holds both copies.
class TransientDamageReader : public NTFS_BROWSER_NAMESPACE::IDiskReader {
 public:
  // intact and damaged MUST have the same size. A read that overlaps a byte
  // where they differ is served from damaged while fewer than damaged_reads
  // such reads came before it, and from intact after.
  TransientDamageReader(std::vector<BYTE> intact, std::vector<BYTE> damaged,
                        std::size_t damaged_reads);

  bool Open(std::wstring_view path) override;

  [[nodiscard]] bool ReadInto(LARGE_INTEGER& addr,
                              std::span<BYTE> dest) const override;

  // The size of the image.
  [[nodiscard]] std::optional<ULONGLONG> Size() const override;

 private:
  // The bytes from the first to the last one that differ, as a half-open
  // range. Empty when the images are the same. It comes first: the readers
  // take the images, and the range is computed from them before.
  struct Range {
    ULONGLONG begin = 0;
    ULONGLONG end = 0;
  };

  [[nodiscard]] static Range Differing(const std::vector<BYTE>& intact,
                                       const std::vector<BYTE>& damaged);

  Range damage_;
  Test::DiskReaders::MemoryDiskReader intact_;
  Test::DiskReaders::MemoryDiskReader damaged_;
  std::size_t damaged_reads_;
  mutable std::size_t reads_so_far_ = 0;
  mutable std::mutex mutex_;
};

}  // namespace NTFS_BROWSER_NAMESPACE::Tool::FuzzGen
