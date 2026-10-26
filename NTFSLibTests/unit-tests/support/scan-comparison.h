#pragma once

#include <ntfs-browser/win-types.h>

#include <cstddef>
#include <string>
#include <vector>

#include <ntfs-browser/cache/strategy.h>
#include <ntfs-browser/mft/tree.h>
#include <ntfs-browser/namespace.h>
#include <ntfs-browser/ntfs-volume.h>

#include "counting-disk-reader.h"

namespace NTFS_BROWSER_NAMESPACE::Tests {

// Everything a scan leaves to look at: what the tree says, what the library
// logged, and what it asked of the reader. Two scans of one volume that differ
// only in their thread count MUST leave the same.
struct ScanResult {
  std::string tree;
  std::string log;
  std::vector<CountingDiskReader::Call> calls;
  // Slots the scan took from the threads that parsed them.
  size_t parsed_by_threads{0};
  // Waiting base records of the second pass, taken from the threads too.
  size_t waiting_by_threads{0};
};

// Writes every field of every entry, and of the statistics, one entry per
// line: two trees are the same when their descriptions are.
[[nodiscard]] std::string DescribeTree(const Mft::MftTree& tree);

// Where two texts part: a few lines of each from the line before the first
// that differs, and nothing when they are the same.
[[nodiscard]] std::string Divergence(const std::string& left,
                                     const std::string& right);

// Whether two scans read the same, in the same order, hints included. Two
// scans without a counting reader both have no call, and read alike.
[[nodiscard]] bool SameCalls(const ScanResult& left, const ScanResult& right);

// Checks that many, a scan with several threads, is one, the same scan with
// one: the same tree, log and reads. A difference shows where the texts part,
// not the texts. Both MUST come from ScanWithThreads().
void CheckSameScan(const ScanResult& one, const ScanResult& many);

// Scans volume with threads threads, and gathers what it did. reader, when not
// null, is the CountingDiskReader the volume was built over, for the reads. The
// log is what the capturing sink holds since the last time anyone took it: the
// caller takes it before it opens the volume, so that the opening counts.
// progress_stops_at, when not zero, makes the progress callback of the scan
// stop it at that call.
[[nodiscard]] ScanResult
    CaptureScan(const NTFS_BROWSER_NAMESPACE::NtfsVolume<
                    NTFS_BROWSER_NAMESPACE::Cache::Strategy::NoCache>& volume,
                const CountingDiskReader* reader, unsigned threads,
                size_t progress_stops_at = 0);

// Same for a FullCache volume.
[[nodiscard]] ScanResult
    CaptureScan(const NTFS_BROWSER_NAMESPACE::NtfsVolume<
                    NTFS_BROWSER_NAMESPACE::Cache::Strategy::FullCache>& volume,
                const CountingDiskReader* reader, unsigned threads,
                size_t progress_stops_at = 0);

}  // namespace NTFS_BROWSER_NAMESPACE::Tests
