#pragma once

// How Search::Scan is built, for whoever extends it.
//
//   Scan (public, scan.h)    Start/Poll/Cancel and the state. Start() makes
//                            a worker thread that runs Engine::Run() with the
//                            log captured. Poll() runs on the caller's thread.
//   ScanExchange             What passes from the worker to Poll(): two
//                            EntryBuffers that Poll() swaps under a short lock
//                            (the caller reads one, the worker fills the
//                            other), the percent, and a Log::Detail::WorkerLog
//                            that Poll() replays on its own thread. An engine
//                            sees it as its EntrySink. Add() may wait when
//                            Options::max_pending_entries is reached.
//   Engine (this file)       One way to walk a volume. MakeEngine() picks it
//                            from Options::engine. ResolveEngine() holds the
//                            Auto rule: MftList for the root of a volume (no
//                            inside path), DirectoryFullCache for a folder.
//   DirectoryEngine<S>       The NtfsDir walk, both caches. It fills every
//                            Entry field except link_count (0), clusters
//                            (null) and, for a live file, data_state.
//   MftListEngine            The NtfsMftList walk: every record in $MFT order,
//                            one Entry for each name of a record, orphans
//                            included, with link_count, the cluster counts of
//                            a deleted file and a percent. It builds an
//                            Mft::MftTree and lists it once the scan is done.
//   EntryBuffer              Owns the text that the views of an Entry point at.
//   SplitTarget/OpenVolume   The target string, opened without test code.
//
// Entry conventions that every engine MUST keep, so that the engines agree:
// times are UTC ticks (FileTimesUtc), names are raw UTF-16 units, the path
// starts with '\' and has no trailing one, the system files (record below 16)
// and the DOS alias names are hidden unless Options::include_system_files, a
// deleted entry has standing Deleted or Reused, and data_state is only set
// for a deleted entry.
//
// An engine runs on the worker thread. It MUST NOT touch the sinks of the log
// (the capture catches what it logs through Log::*), and it MUST call
// sink.Add() and sink.SetPercent() from that one thread only. It SHOULD look at
// the stop token often: the destructor of a Scan waits for the thread. The
// MftList engine gives its entries to the sink after it read the whole $MFT.
// The percent moves during the read.
//
// Left to do: the name matching and the filters on the fields (Options), and
// the Options of the regular expression.

#include <functional>
#include <memory>
#include <stop_token>

#include <ntfs-browser/namespace.h>
#include <ntfs-browser/search/types.h>

#include "internal-export.h"
#include "search/target.h"

namespace NTFS_BROWSER_NAMESPACE::Search {

// Where an engine puts what it finds.
class NTFS_BROWSER_EXPORT_TESTS_ONLY EntrySink {
 public:
  EntrySink() = default;
  EntrySink(const EntrySink&) = delete;
  EntrySink& operator=(const EntrySink&) = delete;
  EntrySink(EntrySink&&) = delete;
  EntrySink& operator=(EntrySink&&) = delete;
  virtual ~EntrySink() = default;

  // Takes a copy of entry: the engine MAY reuse its views and its cluster
  // summary as soon as the call returns.
  virtual void Add(const Entry& entry) = 0;

  // Tells how far the engine is, from 0 to 100. Only an engine that knows the
  // size of its work calls it.
  virtual void SetPercent(double percent) = 0;
};

// One way to walk a volume. An engine holds the options and the target it was
// made for, opens the volume itself and runs once.
class NTFS_BROWSER_EXPORT_TESTS_ONLY Engine {
 public:
  Engine() = default;
  Engine(const Engine&) = delete;
  Engine& operator=(const Engine&) = delete;
  Engine(Engine&&) = delete;
  Engine& operator=(Engine&&) = delete;
  virtual ~Engine() = default;

  // Walks the target and gives each entry to sink, in the order the engine
  // finds them. Looks at stop between entries and ends early when it is
  // set. Returns false when the scan could not run: the target did not open or
  // is no NTFS volume, or the folder to enter does not exist. The log says
  // why. Returns true when the walk ended, or was stopped on request.
  [[nodiscard]] virtual bool Run(EntrySink& sink, std::stop_token stop) = 0;
};

// The engine that a scan with options and target uses. Never Auto: the choice
// is made here, see ResolveEngine().
[[nodiscard]] NTFS_BROWSER_EXPORT_TESTS_ONLY EngineKind
    ResolveEngine(const Options& options, const TargetParts& parts);

// Makes the engine ResolveEngine() chooses.
[[nodiscard]] NTFS_BROWSER_EXPORT_TESTS_ONLY std::unique_ptr<Engine>
    MakeEngine(const Options& options, const TargetParts& parts);

// A function that makes an engine in place of the ones above.
using EngineFactory = std::function<std::unique_ptr<Engine>(
    const Options& options, const TargetParts& parts)>;

// Makes MakeEngine() call factory from now on. An empty function restores the
// real engines. For tests: they hold the worker thread of a scan with an engine
// of their own. Call it while no scan runs: it is not thread safe.
NTFS_BROWSER_EXPORT_TESTS_ONLY void
    SetEngineFactoryForTests(EngineFactory factory);

}  // namespace NTFS_BROWSER_NAMESPACE::Search
