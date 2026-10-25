#pragma once

#include <condition_variable>
#include <cstddef>
#include <exception>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

#include <ntfs-browser/namespace.h>

#include "internal-export.h"

namespace NTFS_BROWSER_NAMESPACE::Util {

// Runs the chunks of one batch of work on a few threads, and hands their
// results to the thread that asks for them in order. A batch is cut into
// numbered chunks. Chunks START in increasing order, on a pool thread or on the
// calling thread, which helps while it waits. The pool threads live as long as
// the pool, so a batch costs no thread start.
//
// One thread owns the pool: it calls Start(), WaitFor() and Finish(). The work
// function runs on any thread, and MUST NOT call the pool.
class NTFS_BROWSER_EXPORT_TESTS_ONLY ChunkPool {
 public:
  // The work function of a batch: chunk is the chunk number, slot says which
  // thread runs it. slot is below Slots(), and no two threads running at once
  // share one, so a slot can own state that a chunk uses alone.
  using Work = std::function<void(size_t chunk, size_t slot)>;

  // Starts helpers pool threads. 0 gives a pool that runs everything on the
  // calling thread. A thread that cannot be started is left out: Slots() says
  // how many slots there are.
  explicit ChunkPool(size_t helpers);
  ChunkPool(const ChunkPool&) = delete;
  ChunkPool& operator=(const ChunkPool&) = delete;
  ChunkPool(ChunkPool&&) = delete;
  ChunkPool& operator=(ChunkPool&&) = delete;
  // Ends the batch in progress, as Finish() does, without throwing, then joins
  // the threads.
  ~ChunkPool();

  // The number of threads that run chunks, the calling one included.
  [[nodiscard]] size_t Slots() const noexcept { return slot_count_; }

  // Begins a batch of chunk_count chunks. The previous batch MUST be over. work
  // MUST stay valid until Finish() returns.
  void Start(size_t chunk_count, Work work);

  // Returns once chunk is done. Until then the calling thread runs chunks that
  // no thread has taken. Rethrows the first exception a chunk of this batch
  // threw. chunk MUST be below the count of the batch.
  void WaitFor(size_t chunk);

  // Ends the batch: chunks no thread started are dropped, and the call returns
  // once the others are done. Rethrows the first exception a chunk threw, if
  // WaitFor() did not.
  void Finish();

 private:
  // The body of a pool thread.
  void RunHelper(size_t slot);

  // Runs one chunk on slot, and records that it is done. lock is held on
  // entry and on return, and released while the chunk runs.
  void RunChunk(std::unique_lock<std::mutex>& lock, size_t chunk, size_t slot);

  // Rethrows and forgets the first exception of the batch, if any.
  void RethrowError();

  // Guards everything below but the threads.
  std::mutex mutex_;
  // Signalled when chunks are available, and when one is done.
  std::condition_variable work_ready_;
  std::condition_variable chunk_done_;
  Work work_;
  // Chunks of the batch, the next one to start, and which are done.
  size_t chunk_count_{0};
  size_t next_chunk_{0};
  std::vector<char> done_;
  // Chunks started and not done.
  size_t running_{0};
  std::exception_ptr error_;
  bool stopping_{false};
  // Slots including the calling thread's, which is the last.
  size_t slot_count_{1};
  std::vector<std::thread> helpers_;
};

}  // namespace NTFS_BROWSER_NAMESPACE::Util
