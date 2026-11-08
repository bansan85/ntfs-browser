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
// Threads. Start() makes one worker thread, which opens the volume and runs the
// scan. Start() returns at once. The caller keeps every call: Start(), Poll(),
// Cancel(), the destructor and the callback all run on the thread that created
// the Scan. Poll() never waits for the worker: it takes the lock for a moment,
// swaps the buffer the worker filled for an empty one, and calls the callback
// with a view of the full one. The worker fills the other buffer meanwhile, so
// no name and no path is copied, and the worker never writes to what the
// callback reads. The batch is valid during the callback only: the buffer is
// recycled when it returns (a debug build overwrites the text).
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
// Order of delivery. The directory engines deliver while they walk. The MftList
// engine reads the whole $MFT first and delivers its entries when the read is
// over: the progress percent moves during the read, and Cancel() stops the read
// within a few thousand records, but no entry arrives before the end of the
// read.
//
// Search. Options::pattern selects the entries by name, or by full path, with
// an exact text or with wildcards, and with or without case. The worker tests
// each entry before it keeps it: an entry that does not match is never
// delivered and takes no memory. The progress counts the delivered entries
// only. A scan that matches nothing still ends with a last batch that holds no
// entry. See Pattern for the rules.
//
// Cancel() and the destructor stop the worker through a stop token that every
// engine looks at between entries. The destructor waits for the thread to end,
// so no thread outlives the Scan.
//
// Errors. Nothing is thrown and the callback has no error result. A target
// that does not open, a pattern that cannot be used (a regular expression, for
// now), a damaged folder or an unreadable record goes to the
// library log (Log::Error, Log::Warn). An exception in the worker is caught,
// logged, and ends the scan as Failed. GetState() tells that the scan failed.

#include <functional>
#include <memory>

#include <ntfs-browser/export.h>
#include <ntfs-browser/namespace.h>
#include <ntfs-browser/search/types.h>

namespace NTFS_BROWSER_NAMESPACE::Search {

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

  // Asks the worker to stop. Returns at once. The next Poll() calls deliver
  // what was found and the last of them ends with the state Cancelled. Does
  // nothing once the scan is over.
  void Cancel() noexcept;

  [[nodiscard]] State GetState() const noexcept;

  // Whether the scan is over: Finished, Cancelled or Failed, and the last
  // batch was delivered by Poll().
  [[nodiscard]] bool Finished() const noexcept;

  // Where the scan stands, as of the last Poll().
  [[nodiscard]] Progress GetProgress() const noexcept;
};  // Scan

}  // namespace NTFS_BROWSER_NAMESPACE::Search
