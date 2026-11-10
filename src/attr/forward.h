#pragma once

#include <cstdint>

#include <ntfs-browser/namespace.h>

// Forward declarations that the attribute headers share, so that each one does
// not repeat them.
namespace NTFS_BROWSER_NAMESPACE {

namespace Cache {

enum class Strategy : std::uint8_t;

}  // namespace Cache

namespace Io {

template <Cache::Strategy S>
class FileRecord;

}  // namespace Io

namespace Attr {

struct HeaderCommon;

}  // namespace Attr

}  // namespace NTFS_BROWSER_NAMESPACE
