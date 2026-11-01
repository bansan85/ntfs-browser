#include "ordered-chunks.h"

#include <algorithm>
#include <condition_variable>
#include <cstddef>
#include <exception>
#include <mutex>
#include <system_error>
#include <thread>
#include <vector>

#include <ntfs-browser/bench/bench.h>

namespace NtfsDevice {

namespace {

// What the calling thread and its helpers share. Everything but steps is
// guarded by mutex.
struct Shared {
  Shared(const ChunkSteps& steps_in, size_t chunk_count)
      : steps(steps_in), done(chunk_count, 0) {}

  const ChunkSteps& steps;
  std::mutex mutex;
  // Signalled when a chunk is prepared, and when the batch stops.
  std::condition_variable work_ready;
  // Signalled when a helper has made a chunk, or has failed.
  std::condition_variable chunk_done;
  // Chunks below prepared are prepared, below started are started, below
  // consumed are consumed.
  size_t prepared = 0;
  size_t started = 0;
  size_t consumed = 0;
  // Whether make() of each chunk returned.
  std::vector<char> done;
  // The first exception a helper threw.
  std::exception_ptr error;
  bool stopping = false;
};

// Makes chunks until the batch stops. slot is the helper's own.
void RunHelper(Shared& shared, size_t slot) {
  const NTFS_BROWSER_NAMESPACE::Bench::ThreadScope cpu(
      NTFS_BROWSER_NAMESPACE::Bench::ThreadRole::Other);
  std::unique_lock lock{shared.mutex};
  for (;;) {
    shared.work_ready.wait(lock, [&shared] {
      return shared.stopping || shared.started < shared.prepared;
    });
    if (shared.stopping) {
      return;
    }
    const size_t chunk = shared.started++;
    lock.unlock();
    try {
      shared.steps.make(chunk, slot);
    } catch (...) {
      lock.lock();
      if (!shared.error) {
        shared.error = std::current_exception();
      }
      shared.stopping = true;
      shared.work_ready.notify_all();
      shared.chunk_done.notify_all();
      return;
    }
    lock.lock();
    // done is sized to the batch, and chunk is below it.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    shared.done[chunk] = 1;
    shared.chunk_done.notify_all();
  }
}

// The helper threads of a batch. Ends and joins them, on any way out.
class Helpers {
 public:
  Helpers(Shared& shared, size_t count) : shared_(shared) {
    threads_.reserve(count);
    for (size_t slot = 0; slot < count; slot++) {
      try {
        threads_.emplace_back(RunHelper, std::ref(shared_), slot);
      } catch (const std::system_error&) {
        // Fewer helpers: the others, and the calling thread, make the chunks.
        break;
      }
    }
  }

  Helpers(const Helpers&) = delete;
  Helpers& operator=(const Helpers&) = delete;
  Helpers(Helpers&&) = delete;
  Helpers& operator=(Helpers&&) = delete;

  ~Helpers() { Stop(); }

  // Tells the helpers to end, and waits for them.
  void Stop() {
    {
      const std::scoped_lock lock{shared_.mutex};
      shared_.stopping = true;
    }
    shared_.work_ready.notify_all();
    for (std::thread& thread : threads_) {
      if (thread.joinable()) {
        thread.join();
      }
    }
  }

 private:
  Shared& shared_;
  std::vector<std::thread> threads_;
};

}  // namespace

void RunOrderedChunks(const ChunkJob& job, const ChunkSteps& steps) {
  if (job.chunk_count == 0) {
    return;
  }
  const size_t threads = std::clamp<size_t>(job.threads, 1, job.chunk_count);
  const size_t window = (std::max<size_t>)(job.window, 1);
  // The calling thread has the last slot, the helpers the ones before it.
  const size_t own_slot = threads - 1;

  Shared shared(steps, job.chunk_count);
  Helpers helpers(shared, threads - 1);
  std::unique_lock lock{shared.mutex};
  while (shared.consumed < job.chunk_count && !shared.error) {
    // done is sized to the batch, and consumed is below it.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    if (shared.done[shared.consumed] != 0) {
      const size_t chunk = shared.consumed;
      lock.unlock();
      steps.consume(chunk);
      lock.lock();
      shared.consumed++;
      continue;
    }
    // Preparing comes before making: it is what keeps the helpers busy.
    if (shared.prepared < job.chunk_count &&
        shared.prepared < shared.consumed + window) {
      const size_t chunk = shared.prepared;
      lock.unlock();
      steps.prepare(chunk);
      lock.lock();
      shared.prepared++;
      shared.work_ready.notify_one();
      continue;
    }
    if (shared.started < shared.prepared) {
      const size_t chunk = shared.started++;
      lock.unlock();
      steps.make(chunk, own_slot);
      lock.lock();
      // done is sized to the batch, and chunk is below it.
      // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
      shared.done[chunk] = 1;
      continue;
    }
    shared.chunk_done.wait(lock);
  }
  lock.unlock();
  helpers.Stop();
  if (shared.error) {
    std::rethrow_exception(shared.error);
  }
}

}  // namespace NtfsDevice
