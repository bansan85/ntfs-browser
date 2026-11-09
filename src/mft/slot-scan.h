#pragma once

#include <optional>
#include <vector>

#include <ntfs-browser/namespace.h>

#include "alloc/claim-inventory.h"
#include "alloc/types.h"
#include "cache/strategy.h"
#include "mft/slot-entry.h"
#include "mft/tree.h"
#include "win-types.h"

namespace NTFS_BROWSER_NAMESPACE {

template <Cache::Strategy S>
class NtfsVolume;

namespace Mft {

// What a scan of the record slots of $MFT is asked to do.
struct SlotScanInput {
  const Mft::MftTree::ScanOptions& options;
  // The slots in use, when the bitmap of $MFT could be read: the scan leaves
  // the others alone.
  const std::optional<std::vector<BYTE>>& bitmap;
  SlotFeatures features;
};

// Reads the record slots of volume's $MFT into entries, in record order, in
// the two passes of a scan. The first pass reads each slot and keeps the entry
// of each base record. A base record with an $ATTRIBUTE_LIST waits for the
// second pass: by then the first one has held the extension records it asks
// for, so they come from memory instead of one disk read each. stats receives
// the counters. Returns the clusters of $J when the scan met the $UsnJrnl
// record. inventory receives the claims of the kept records when
// input.features.account is true.
template <Cache::Strategy S>
std::vector<Alloc::Extent>
    ScanSlots(const NtfsVolume<S>& volume, const SlotScanInput& input,
              std::vector<Mft::MftTree::Entry>& entries,
              Mft::MftTree::ScanStats& stats, Alloc::ClaimInventory& inventory);

}  // namespace Mft
}  // namespace NTFS_BROWSER_NAMESPACE
