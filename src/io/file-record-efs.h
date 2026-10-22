#pragma once

#include <cstdint>

#include <ntfs-browser/namespace.h>

namespace NTFS_BROWSER_NAMESPACE {

namespace Cache {

enum class Strategy : std::uint8_t;

}  // namespace Cache

}  // namespace NTFS_BROWSER_NAMESPACE

namespace NTFS_BROWSER_NAMESPACE {
namespace Io {

template <Cache::Strategy S>
class FileRecord;

}  // namespace Io
}  // namespace NTFS_BROWSER_NAMESPACE

namespace NTFS_BROWSER_NAMESPACE::Io {

// Gives record's encrypted $DATA streams the context that decrypts them.
template <Cache::Strategy S>
[[nodiscard]] bool AttachEfsContext(FileRecord<S>& record);

}  // namespace NTFS_BROWSER_NAMESPACE::Io
