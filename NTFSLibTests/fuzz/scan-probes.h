#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>

#include <ntfs-browser/namespace.h>

#include "cache/strategy.h"
#include "disk-reader.h"
#include "volume-options.h"
#include "win-types.h"

// The probes of FuzzTestcase() that scan the $MFT or list directories over
// readers of their own. They read in another order than the main probes, so
// FuzzOnce() runs them last, in the order it names them: a recorded input
// replays in step only while that order holds.
namespace NTFS_BROWSER_NAMESPACE::Test::Fuzz {

// Scans the volume over reader with threads threads, as the probe of
// ProbeScanThreads() does, and gives the digest of the tree. Nothing when the
// scan throws: only the fact that it did is worth comparing.
template <NTFS_BROWSER_NAMESPACE::Cache::Strategy S>
std::optional<std::uint64_t>
    ScanDigestOver(std::unique_ptr<NTFS_BROWSER_NAMESPACE::IDiskReader> reader,
                   const NTFS_BROWSER_NAMESPACE::VolumeOptions& options,
                   unsigned threads);

// Scans the volume over reader with the cluster account at its fullest: the
// map, and a content check of every cluster presumed intact. Gives the digest
// of the account. Nothing when the scan throws.
template <NTFS_BROWSER_NAMESPACE::Cache::Strategy S>
std::optional<std::uint64_t> AccountDigestOver(
    std::unique_ptr<NTFS_BROWSER_NAMESPACE::IDiskReader> reader,
    const NTFS_BROWSER_NAMESPACE::VolumeOptions& options, unsigned threads);

// Scans the input with one thread, then with several, each over a reader of its
// own: the two MUST give the same tree. The threads change when the records are
// parsed, never what the scan reads or finds, so a difference is a finding, and
// ends the process like a crash does.
template <NTFS_BROWSER_NAMESPACE::Cache::Strategy S>
void ProbeScanThreads(std::span<const BYTE> data,
                      const NTFS_BROWSER_NAMESPACE::VolumeOptions& options,
                      std::optional<size_t> failing_read);

// Walks the root with the batched walk, over a reader of its own.
template <NTFS_BROWSER_NAMESPACE::Cache::Strategy S>
void ProbeStandingWalk(std::span<const BYTE> data,
                       const NTFS_BROWSER_NAMESPACE::VolumeOptions& options,
                       std::optional<size_t> failing_read);

// Scans the input with the cluster account on, with one thread and then with
// several, each over a reader of its own: the two accounts MUST be the same,
// or the process aborts, as ProbeScanThreads() does. A scan that the progress
// callback stops has no account, so only an input whose $MFT is short reaches
// the sweep.
template <NTFS_BROWSER_NAMESPACE::Cache::Strategy S>
void ProbeClusterAccount(std::span<const BYTE> data,
                         const NTFS_BROWSER_NAMESPACE::VolumeOptions& options,
                         std::optional<size_t> failing_read);

// Lists the directories below the root as a recursive listing does, with the
// read ahead of the next directories on, over a reader of its own. The batches
// read at addresses that a stream reader ignores, so the bytes they hold are
// whatever comes next in the input: they MUST be parsed like any other.
template <NTFS_BROWSER_NAMESPACE::Cache::Strategy S>
void ProbeDirectoryReadAhead(
    std::span<const BYTE> data,
    const NTFS_BROWSER_NAMESPACE::VolumeOptions& options,
    std::optional<size_t> failing_read);

}  // namespace NTFS_BROWSER_NAMESPACE::Test::Fuzz
