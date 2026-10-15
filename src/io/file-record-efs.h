#pragma once

#include <ntfs-browser/cache/strategy.h>
#include <ntfs-browser/io/file-record.h>
#include <ntfs-browser/namespace.h>

namespace NTFS_BROWSER_NAMESPACE::Io {

// Gives record's encrypted $DATA streams the context that decrypts them.
template <Cache::Strategy S>
[[nodiscard]] bool AttachEfsContext(FileRecord<S>& record);

}  // namespace NTFS_BROWSER_NAMESPACE::Io
