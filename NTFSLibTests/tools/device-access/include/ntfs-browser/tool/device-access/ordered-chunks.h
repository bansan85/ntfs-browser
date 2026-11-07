#pragma once

#include <cstddef>
#include <functional>

#include <ntfs-browser/namespace.h>

namespace NTFS_BROWSER_NAMESPACE::Tool::DeviceAccess {

// How a batch of chunks is cut and spread: see RunOrderedChunks().
struct ChunkJob {
  size_t chunk_count = 0;
  // Threads that make chunks, the calling one included. More than chunk_count
  // is the same as chunk_count.
  size_t threads = 1;
  // Chunks prepared and not yet consumed, at most. It bounds the memory the
  // results take. 0 counts as 1.
  size_t window = 1;
};

// What is done to each chunk of a batch.
struct ChunkSteps {
  // Runs on the calling thread, once per chunk and in order, before the chunk
  // can be made. It MAY change what make() of the chunks it prepared reads,
  // and MUST NOT change what make() of an earlier chunk reads.
  std::function<void(size_t chunk)> prepare;
  // Runs on any thread. Chunks start in increasing order. slot is below
  // ChunkJob::threads, and no two calls running at once share one, so a slot
  // can own scratch space. The chunk MAY finish after a later one.
  std::function<void(size_t chunk, size_t slot)> make;
  // Runs on the calling thread, once per chunk and in order, after make() of
  // the chunk returned. A chunk is held until then: at most
  // ChunkJob::window chunks are prepared and not yet consumed, so a result
  // MAY live in a slot of window entries, indexed by chunk % window.
  std::function<void(size_t chunk)> consume;
};

// Cuts the work of a listing into chunks, makes them on several threads, and
// hands the results back in order: the output is the one of a single thread.
// The calling thread prepares and consumes, and makes a chunk when it has
// nothing else to do. The helper threads live for the call. A thread that
// cannot be started is left out. An exception from any step stops the batch,
// and is rethrown by the call once its threads have ended.
void RunOrderedChunks(const ChunkJob& job, const ChunkSteps& steps);

}  // namespace NTFS_BROWSER_NAMESPACE::Tool::DeviceAccess
