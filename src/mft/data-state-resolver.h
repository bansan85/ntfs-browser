#pragma once

#include <ntfs-browser/win-types.h>

#include <cstddef>
#include <vector>

#include <ntfs-browser/alloc/types.h>
#include <ntfs-browser/cache/strategy.h>
#include <ntfs-browser/mft/tree.h>
#include <ntfs-browser/namespace.h>

#include "mft/slot-entry.h"

namespace NTFS_BROWSER_NAMESPACE {

template <Cache::Strategy S>
class NtfsVolume;

namespace Mft {

// Gives the entries of one scan their data state. A deleted stream's extents
// are kept as queries, and resolved against the cluster bitmap in one sorted
// batch once enough of them wait, and at the end: deleted files sit at random
// clusters, so a query per file would reload a window of the bitmap for nearly
// each of them.
template <Cache::Strategy S>
class DataStateResolver {
 public:
  // keep_windows leaves the bitmap windows the batches kept to the caller,
  // who gives them back.
  DataStateResolver(const NtfsVolume<S>& volume,
                    std::vector<Mft::MftTree::Entry>& entries,
                    Mft::MftTree::ScanStats& stats, bool keep_windows = false);

  DataStateResolver(const DataStateResolver&) = delete;
  DataStateResolver& operator=(const DataStateResolver&) = delete;
  DataStateResolver(DataStateResolver&&) = delete;
  DataStateResolver& operator=(DataStateResolver&&) = delete;

  // Gives back the bitmap windows the batches kept.
  ~DataStateResolver();

  // Handles the entry at entry_index, whose record gave note.
  void Add(size_t entry_index, DataNote note);

  // Resolves the entries still waiting, the last ones of the scan, and gives
  // back the bitmap windows kept for the batches.
  void Finish();

  // Resolves every waiting entry. A bitmap that cannot answer leaves them
  // unknown: the scan goes on.
  void Flush();

 private:
  // An entry whose extents wait in queries_.
  struct Waiting {
    size_t entry_index;
    size_t first_query;
    size_t query_count;
    bool complete;
  };

  // Records state in entry_index and in the statistics.
  void Set(size_t entry_index, Alloc::DataState state);

  const NtfsVolume<S>* volume_;
  std::vector<Mft::MftTree::Entry>* entries_;
  Mft::MftTree::ScanStats* stats_;
  std::vector<Alloc::RangeQuery> queries_;
  std::vector<Waiting> waiting_;
  // Whether the volume keeps bitmap windows for this resolver.
  bool keeping_{false};
  // Whether the windows outlive the resolver, for the cluster sweep.
  bool keep_windows_{false};
  // Queries kept before a batch resolves them, and bytes of bitmap kept.
  size_t max_pending_;
  ULONGLONG kept_bitmap_bytes_;
};

}  // namespace Mft
}  // namespace NTFS_BROWSER_NAMESPACE
