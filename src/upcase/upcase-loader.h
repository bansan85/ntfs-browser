#pragma once

#include <cstdint>
#include <memory>

#include <ntfs-browser/namespace.h>

#include "mft/metadata-opener.h"

namespace NTFS_BROWSER_NAMESPACE {

namespace Cache {

enum class Strategy : std::uint8_t;

}  // namespace Cache

namespace UpCase {

class Table;

// Reads the volume's own $UpCase from its MFT record.
template <Cache::Strategy S>
class TableLoader {
 public:
  explicit TableLoader(const Mft::MetadataOpener<S>& opener) noexcept;

  // Reads $UpCase (MFT record 10). Null when it is missing or unusable.
  [[nodiscard]] std::unique_ptr<const Table> Load() const;

 private:
  Mft::MetadataOpener<S> opener_;
};

}  // namespace UpCase
}  // namespace NTFS_BROWSER_NAMESPACE
