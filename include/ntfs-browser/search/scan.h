#pragma once

// Scan: lists the files of a folder or of a whole NTFS volume, as the NtfsDir
// and NtfsMftList samples do, and hands them to the caller in batches. The
// scan reads raw disk structures, not the file system API, so it also finds
// deleted files and works on an image file.
//
// Use:
//
//   Search::Options options;
//   options.target = u"c:\\Users";
//   options.recursive = true;
//   Search::Scan scan(options);
//   scan.OnBatch([](const Search::Batch& batch) {
//     for (const Search::Entry& entry : batch.entries) { ... }
//   });
//   scan.Start();
//   while (!scan.Finished()) {
//     scan.Poll();
//     // do other work, then wait about 100 ms
//   }
//
// A caller with nothing else to do can let Run() do the waiting. It starts the
// scan, polls about every 100 ms and returns when the scan is over:
//
//   const Search::State state = scan.Run();
//
// Threads. Start() makes one worker thread, which opens the volume and runs the
// scan. Start() returns at once. The caller keeps every call: Start(), Poll(),
// Run(), the destructor and the callback all run on the thread that created
// the Scan. Cancel() runs there too, from the callback or anywhere else, and
// it is the one call that another thread MAY make: see Cancel(). Poll() never
// waits for the worker: it takes the lock for a moment, swaps the buffer the
// worker filled for an empty one, and calls the callback with a view of the
// full one. The worker fills the other buffer meanwhile, so no name and no path
// is copied, and the worker never writes to what the callback reads. The batch
// is valid during the callback only: the buffer is recycled when it returns (a
// debug build overwrites the text).
//
// The library and its log are not thread safe, and the Scan keeps to that.
// Whatever the worker logs, the opening of the volume included, is captured in
// a buffer of the worker. Poll() replays it to the log sinks on the caller's
// thread, in order, as it hands over the batches. The sinks are never called
// from the worker thread. The log levels are the ones the sinks accept when
// Start() runs.
//
// Pace. A Poll() delivers everything found since the previous one, as one
// batch. Call it about every 100 ms. It calls the callback only when something
// is new: entries, a change of the percent, or the end. A caller that polls
// very often gets few batches, and none is empty. A worker faster than the
// caller waits when Options::max_pending_entries entries are pending, so the
// memory stays bounded. A caller that polls too rarely slows the scan down by
// that limit and gets large batches: the limit is also the largest batch.
//
// Run(). A loop of waits and Poll() calls, on the caller's thread. The wait
// ends when the interval is over, or earlier when the worker has news (entries,
// a change of the percent), when the scan ends, or when Cancel() is called.
// After a batch, news does not wake the loop before one interval has passed, so
// the batches stay large and the callback runs about once per interval. The end
// of the scan, Cancel() and a full pending buffer wake it at once. A condition
// variable in the exchange does the waiting: the worker notifies it, and Run()
// never spins.
//
// Order of delivery. The directory engines deliver while they walk. The MftList
// engine reads the whole $MFT first and delivers its entries when the read is
// over: the progress percent moves during the read, and Cancel() stops the read
// within a few thousand records, but no entry arrives before the end of the
// read.
//
// Search. Options::pattern selects the entries by name, or by full path, with
// an exact text, with wildcards or with a regular expression, and with or
// without case. The worker tests each entry before it keeps it: an entry that
// does not match is never delivered and takes no memory. The progress counts
// the delivered entries only. A scan that matches nothing still ends with a
// last batch that holds no entry. See Pattern for the rules.
//
// Filter. Options::filter adds tests on the other fields of an entry: the
// sizes, the four times, the attributes, the kind (file or folder), live or
// deleted, the record numbers, the number of names, and for a deleted file the
// state of its data and the counts of its clusters. The tests are combined with
// AND, with the pattern as one more. A field that is not set is not tested. The
// cheap tests run first and the pattern last. A filter on a field that the
// chosen engine does not give ends the scan as Failed, with a log line naming
// the field, and EngineKind::Auto picks an engine that gives it. See Filter for
// every test, its bounds and its engine.
//
// Cancel() and the destructor stop the worker through a stop token that every
// engine looks at between entries. The destructor waits for the thread to end,
// so no thread outlives the Scan.
//
// Errors. Nothing is thrown and the callback has no error result. A target
// that does not open, a pattern or a filter that cannot be used (a regular
// expression that does not compile, or one when the library has no regex
// support, a range with its minimum above its maximum, a field that the engine
// does not give), a damaged folder or an unreadable record goes to the library
// log (Log::Error, Log::Warn). An exception in the worker is caught, logged,
// and ends the scan as Failed. GetState() tells that the scan failed.

#include <chrono>
#include <functional>
#include <memory>

#include <ntfs-browser/export.h>
#include <ntfs-browser/namespace.h>
#include <ntfs-browser/search/types.h>

namespace NTFS_BROWSER_NAMESPACE::Search {

// Whether this build of the library has regular expressions
// (PatternKind::Regex), which the CMake option NTFS_BROWSER_ENABLE_REGEX turns
// on. A scan with a regular expression ends as Failed when it returns false.
[[nodiscard]] NTFS_BROWSER_EXPORT bool RegexAvailable() noexcept;

// MSVC warns (C4251, at /W4) that the exported class holds a std::unique_ptr
// of a type it does not export. The member is private and Impl is never seen
// by a client, so the warning has nothing to tell.
#ifdef _MSC_VER
  #pragma warning(push)
  #pragma warning(disable : 4251)
#endif

// A scan of one NTFS volume or folder, run on a worker thread of its own. One
// object scans once. See the top of this file for the use and the rules.
class NTFS_BROWSER_EXPORT Scan {
 public:
  // Receives the entries found since the previous call of Poll(). Called by
  // Poll(), on the caller's thread. The batch, and every view in it, is valid
  // only during the call.
  using BatchCallback = std::function<void(const Batch& batch)>;

  // Takes the options. Opens nothing: Start() does.
  explicit Scan(Options options);
  Scan(const Scan&) = delete;
  Scan& operator=(const Scan&) = delete;
  Scan(Scan&&) = delete;
  Scan& operator=(Scan&&) = delete;
  // Cancels the scan if it still runs, and waits for the worker thread to end.
  ~Scan();

 private:
  // Every member and private method, kept out of this header.
  class Impl;
  std::unique_ptr<Impl> impl_;

 public:
  // Sets the function Poll() calls. Set it before Start(): a batch found with
  // no callback is dropped.
  void OnBatch(BatchCallback callback);

  // Begins the scan on a worker thread and returns at once. Does nothing when
  // the scan was started already. A target that cannot be opened makes the
  // state Failed, with the reason in the log, once the worker found out: it is
  // Running until a Poll() delivers the end.
  void Start();

  // Delivers to the callback what the scan found since the last call, as one
  // batch, and returns the state. Never blocks on the scan. Calls the callback
  // only when there is something new: entries, a change of progress, or the
  // end of the scan, which always gets a batch with last set.
  State Poll();

  // Starts the scan if it is not started yet, then waits and calls Poll() until
  // the scan is over. Returns the final state: Finished, Cancelled or Failed.
  // The callback and the replayed log lines run on the calling thread, as with
  // Poll().
  //
  // interval is the longest wait between two calls of Poll(). A Poll() in each
  // round replays the log lines of the worker even when nothing is new. The
  // wait ends early when the worker has news, but not before one interval has
  // passed since the last batch. It ends at once when the scan ends, when
  // Cancel() is called, and when Options::max_pending_entries entries wait: the
  // batch cannot grow, and the worker is held until the next Poll(). An
  // interval of 0 waits for the worker with no timeout and polls after each
  // wake with no delay: a log line then waits for the next news. A negative
  // interval counts as 0 and one above a day as a day.
  //
  // A scan that was started already is not started again. Run() on a scan that
  // is over returns its state at once. Run() from the callback does nothing and
  // returns the current state, as Poll() does. An exception of the callback
  // leaves Run() and the scan can be run again.
  State
      Run(std::chrono::milliseconds interval = std::chrono::milliseconds(100));

  // Asks the worker to stop. Returns at once. The next calls of Poll() deliver
  // what was found, and the last batch ends the scan with the state Cancelled.
  // Does nothing once the scan is over. It also wakes a Run() that waits.
  //
  // Thread rule. It is safe from the callback, and from any other thread while
  // another thread waits in Run() or polls, once the scan is started: Start()
  // has returned, or Run() has begun and the worker is seen at work. It only
  // reads atomic flags, asks the stop source and wakes the exchange. A call
  // before the scan is started makes the worker stop at its first look, and the
  // state ends Finished, not Cancelled. No other call is safe from a second
  // thread. The Scan MUST outlive a Cancel() from another thread: do not call
  // it as the destructor runs.
  void Cancel() noexcept;

  // The state of the scan. It stays Running until the Poll() that delivers the
  // last batch, then becomes Finished, Cancelled or Failed.
  [[nodiscard]] State GetState() const noexcept;

  // Whether the scan is over: Finished, Cancelled or Failed, and the last
  // batch was delivered by Poll().
  [[nodiscard]] bool Finished() const noexcept;

  // Where the scan stands, as of the last Poll().
  [[nodiscard]] Progress GetProgress() const noexcept;
};  // Scan

#ifdef _MSC_VER
  #pragma warning(pop)
#endif

}  // namespace NTFS_BROWSER_NAMESPACE::Search
