#pragma once

#include <cstdint>
#include <memory>

#include <ntfs-browser/namespace.h>
#include <ntfs-browser/usn/types.h>

#include "internal-export.h"

namespace NTFS_BROWSER_NAMESPACE {

namespace Cache {

enum class Strategy : std::uint8_t;

}  // namespace Cache

}  // namespace NTFS_BROWSER_NAMESPACE

namespace NTFS_BROWSER_NAMESPACE {}  // namespace NTFS_BROWSER_NAMESPACE

namespace NTFS_BROWSER_NAMESPACE {

template <Cache::Strategy S>
class NtfsVolume;

}  // namespace NTFS_BROWSER_NAMESPACE

namespace NTFS_BROWSER_NAMESPACE::Usn {

// Reads the $J stream of $Extend\$UsnJrnl of volume, and decodes its records.
// Only the real ranges of the stream are read, in order, so a sparse head costs
// nothing. Both $Extend and $UsnJrnl are read whatever the volume's
// include_deleted says: they are metadata. The newest
// VolumeOptions::max_usn_records records are kept.
//
// Null when the volume's recover_errors is off (nothing is read then), when
// there is no $Extend, $UsnJrnl or $J, or when the stream cannot be read at
// all. A stream read in part gives what was read. It never throws. The caller
// owns the result and decides what to log about a null one.
template <Cache::Strategy S>
[[nodiscard]] NTFS_BROWSER_EXPORT_TESTS_ONLY std::unique_ptr<Journal>
    LoadJournal(const NtfsVolume<S>& volume);

}  // namespace NTFS_BROWSER_NAMESPACE::Usn
