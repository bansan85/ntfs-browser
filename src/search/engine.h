#pragma once

// How Search::Scan is built, for whoever extends it.
//
//   Scan (public, scan.h)    Start/Poll/Cancel and the state. Today Start()
//                            runs the engine to its end into one EntryBuffer
//                            and Poll() hands it over in slices of at most
//                            4096 entries. The worker thread replaces this:
//                            it runs Engine::Run() on its own thread with a
//                            log capture, fills one of two EntryBuffers, and
//                            Poll() swaps them under a lock, replays the log
//                            and calls the callback.
//   Engine (this file)       One way to walk a volume. MakeEngine() picks it
//                            from Options::engine. ResolveEngine() holds the
//                            Auto rule: it returns DirectoryFullCache for
//                            every target until the MftList engine exists.
//   DirectoryEngine<S>       The NtfsDir walk, both caches. It fills every
//                            Entry field except link_count (0), clusters
//                            (null) and, for a live file, data_state.
//   EntryBuffer              Owns the text that the views of an Entry point at.
//   SplitTarget/OpenVolume   The target string, opened without test code.
//
// Entry conventions that every engine MUST keep, so that the engines agree:
// times are UTC ticks (FileTimesUtc), names are raw UTF-16 units, the path
// starts with '' and has no trailing one, the system files (record below 16)
// and the DOS alias names are hidden unless Options::include_system_files, a
// deleted entry has standing Deleted or Reused, and data_state is only set
// for a deleted entry.
//
// Left to do: the MftList engine (fills link_count and clusters, calls
// SetPercent()), the worker thread, the name matching and the filters on the
// fields (Options), and the Options of the regular expression.

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

// Makes the engine ResolveEngine() chooses. Null for an engine that does not
// exist yet, after a log line at Error level.
[[nodiscard]] NTFS_BROWSER_EXPORT_TESTS_ONLY std::unique_ptr<Engine>
    MakeEngine(const Options& options, const TargetParts& parts);

}  // namespace NTFS_BROWSER_NAMESPACE::Search
