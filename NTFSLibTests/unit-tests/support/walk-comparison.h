#pragma once

#include <ntfs-browser/win-types.h>

#include <compare>
#include <string>
#include <vector>

#include <ntfs-browser/cache/strategy.h>
#include <ntfs-browser/io/record-standing.h>
#include <ntfs-browser/namespace.h>
#include <ntfs-browser/ntfs-volume.h>

namespace NTFS_BROWSER_NAMESPACE::Tests {

// One entry a directory walk showed.
struct WalkedEntry {
  std::wstring name;
  ULONGLONG reference{0};
  WORD sequence{0};
  bool directory{false};
  // How the record the entry names stands.
  Io::RecordStanding standing{Io::RecordStanding::Live};

  auto operator<=>(const WalkedEntry&) const = default;
};

// What a walk of every directory of a volume showed: the entries in the order
// the walk gave them, each directory after the one it lies in, and the log the
// walk wrote.
struct WalkResult {
  std::vector<WalkedEntry> entries;
  // The log lines of the walk, but those about disk reads: the batched walk
  // reads the records of a group in one go, and those lines follow the reads.
  std::string log;
};

// Walks every directory of volume from the root, and tells the standing of
// each entry. With batched, through TraverseSubEntriesWithStanding(); without,
// through TraverseSubEntries(), and the standing comes from the probe that a
// caller of that walk has to make itself, once the walk is done. A directory
// that does not parse is left out.
template <NTFS_BROWSER_NAMESPACE::Cache::Strategy S>
[[nodiscard]] WalkResult WalkVolume(const NtfsVolume<S>& volume, bool batched);

}  // namespace NTFS_BROWSER_NAMESPACE::Tests
