#include "util/chunk-pool.h"

#include <cstddef>
#include <exception>
#include <mutex>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include <ntfs-browser/namespace.h>

namespace NTFS_BROWSER_NAMESPACE::Util {

ChunkPool::ChunkPool(size_t helpers) {
  helpers_.reserve(helpers);
  for (size_t slot = 0; slot < helpers; slot++) {
    try {
      helpers_.emplace_back([this, slot] { RunHelper(slot); });
    } catch (const std::system_error&) {
      // The pool works with the threads it has.
      break;
    }
  }
  slot_count_ = helpers_.size() + 1;
}

ChunkPool::~ChunkPool() {
  try {
    Finish();
  } catch (...) {
    // A destructor cannot report a chunk's exception.
  }
  {
    const std::scoped_lock lock{mutex_};
    stopping_ = true;
  }
  work_ready_.notify_all();
  for (std::thread& helper : helpers_) {
    helper.join();
  }
}

void ChunkPool::Start(size_t chunk_count, Work work) {
  {
    const std::scoped_lock lock{mutex_};
    work_ = std::move(work);
    chunk_count_ = chunk_count;
    next_chunk_ = 0;
    done_.assign(chunk_count, 0);
    running_ = 0;
    error_ = nullptr;
  }
  work_ready_.notify_all();
}

void ChunkPool::WaitFor(size_t chunk) {
  std::unique_lock lock{mutex_};
  while (done_.at(chunk) == 0 && !error_) {
    if (next_chunk_ < chunk_count_) {
      const size_t next = next_chunk_++;
      RunChunk(lock, next, slot_count_ - 1);
    } else {
      chunk_done_.wait(lock);
    }
  }
  RethrowError();
}

void ChunkPool::Finish() {
  std::unique_lock lock{mutex_};
  // Chunks nobody started are not needed any more.
  next_chunk_ = chunk_count_;
  chunk_done_.wait(lock, [this] { return running_ == 0; });
  RethrowError();
}

void ChunkPool::RunHelper(size_t slot) {
  std::unique_lock lock{mutex_};
  for (;;) {
    work_ready_.wait(
        lock, [this] { return stopping_ || next_chunk_ < chunk_count_; });
    if (stopping_) {
      return;
    }
    RunChunk(lock, next_chunk_++, slot);
  }
}

void ChunkPool::RunChunk(std::unique_lock<std::mutex>& lock, size_t chunk,
                         size_t slot) {
  running_++;
  lock.unlock();
  std::exception_ptr failure;
  try {
    work_(chunk, slot);
  } catch (...) {
    failure = std::current_exception();
  }
  lock.lock();
  running_--;
  if (failure && !error_) {
    error_ = failure;
  }
  done_.at(chunk) = 1;
  chunk_done_.notify_all();
}

void ChunkPool::RethrowError() {
  if (error_) {
    const std::exception_ptr failure = std::exchange(error_, nullptr);
    std::rethrow_exception(failure);
  }
}

}  // namespace NTFS_BROWSER_NAMESPACE::Util
