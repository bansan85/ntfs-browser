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
// Threads. The caller drives the scan and owns every call: Start(), Poll(),
// Cancel() and the callback all run on the thread that created the Scan. The
// library and its log are not thread safe, and the Scan keeps to that.
//
// Errors. Nothing is thrown and the callback has no error result. A target
// that does not open, a damaged folder or an unreadable record goes to the
// library log (Log::Error, Log::Warn). GetState() tells that the scan failed.
//
// State of the implementation. Today Start() runs the whole scan before it
// returns and Poll() delivers what it found in batches. The interface is made
// for a scan on a worker thread: no caller needs a change when it moves.

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
  // Cancels the scan if it still runs.
  ~Scan();

 private:
  // Every member and private method, kept out of this header.
  class Impl;
  std::unique_ptr<Impl> impl_;

 public:
  // Sets the function Poll() calls. Set it before Start(): a batch found with
  // no callback is dropped.
  void OnBatch(BatchCallback callback);

  // Begins the scan. Does nothing when the scan was started already. A target
  // that cannot be opened makes the state Failed, with the reason in the log.
  void Start();

  // Delivers to the callback what the scan found since the last call, as one
  // batch, and returns the state. Never blocks on the scan. Calls the callback
  // only when there is something new: entries, a change of progress, or the
  // end of the scan, which always gets a batch with last set.
  State Poll();

  // Asks the scan to stop. The next Poll() delivers what was found and ends
  // with the state Cancelled. Does nothing once the scan is over.
  void Cancel() noexcept;

  [[nodiscard]] State GetState() const noexcept;

  // Whether the scan is over: Finished, Cancelled or Failed, and the last
  // batch was delivered by Poll().
  [[nodiscard]] bool Finished() const noexcept;

  // Where the scan stands, as of the last Poll().
  [[nodiscard]] Progress GetProgress() const noexcept;
};  // Scan

}  // namespace NTFS_BROWSER_NAMESPACE::Search
