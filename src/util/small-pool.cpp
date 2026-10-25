#include "util/small-pool.h"

#include <array>
#include <cstddef>
#include <new>

#include <ntfs-browser/namespace.h>

namespace NTFS_BROWSER_NAMESPACE::Util {

namespace {

// Sizes of the blocks of one list are one multiple of this: the alignment the
// heap gives, so a block fits any object.
constexpr size_t block_granule = 16;

// Lists a thread keeps: one for each multiple of block_granule up to
// max_pooled_size.
constexpr size_t list_count =
    (SmallBlockPool::max_pooled_size / block_granule) + 1;

// Blocks a list holds at most. A scan frees a handful of blocks of each size
// before it asks for them again, so a few are enough. The cap bounds what a
// thread keeps when it frees many, as the thread that drops what another one
// made does.
constexpr size_t max_cached_blocks = 1024;

// A free block holds the link to the next one in its first bytes.
struct FreeBlock {
  FreeBlock* next;
};

// Set once the lists of the thread are gone. An object of static storage
// duration may free a block after that: it goes to the heap.
thread_local bool lists_gone = false;

// The free blocks of a thread. They go back to the heap when the thread ends.
struct ThreadLists {
  std::array<FreeBlock*, list_count> heads{};
  std::array<size_t, list_count> counts{};

  ThreadLists() = default;
  ThreadLists(const ThreadLists&) = delete;
  ThreadLists& operator=(const ThreadLists&) = delete;
  ThreadLists(ThreadLists&&) = delete;
  ThreadLists& operator=(ThreadLists&&) = delete;

  ~ThreadLists() {
    for (FreeBlock* head : heads) {
      while (head != nullptr) {
        FreeBlock* const next = head->next;
        ::operator delete(head);
        head = next;
      }
    }
    lists_gone = true;
  }
};

// The lists of the calling thread, or nothing once they are gone.
ThreadLists* Lists() noexcept {
  if (lists_gone) {
    return nullptr;
  }
  thread_local ThreadLists lists;
  return &lists;
}

// The list that serves size bytes.
size_t ListOf(size_t size) noexcept {
  return (size + block_granule - 1) / block_granule;
}

}  // namespace

void* SmallBlockPool::Allocate(size_t size) {
  if (!SmallBlockPool::enabled || size == 0 || size > max_pooled_size) {
    return ::operator new(size);
  }
  const size_t list = ListOf(size);
  ThreadLists* const lists = Lists();
  if (lists != nullptr) {
    // list < list_count: size <= max_pooled_size.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    FreeBlock*& head = lists->heads[list];
    if (head != nullptr) {
      FreeBlock* const block = head;
      head = block->next;
      // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
      lists->counts[list]--;
      return block;
    }
  }
  // Every block of a list has the size of the list, whatever size asked for.
  return ::operator new(list * block_granule);
}

void SmallBlockPool::Deallocate(void* block, size_t size) noexcept {
  if (block == nullptr) {
    return;
  }
  if (!SmallBlockPool::enabled || size == 0 || size > max_pooled_size) {
    ::operator delete(block);
    return;
  }
  const size_t list = ListOf(size);
  ThreadLists* const lists = Lists();
  // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
  if (lists == nullptr || lists->counts[list] >= max_cached_blocks) {
    ::operator delete(block);
    return;
  }
  auto* const freed = static_cast<FreeBlock*>(block);
  // list < list_count: size <= max_pooled_size.
  // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
  freed->next = lists->heads[list];
  // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
  lists->heads[list] = freed;
  // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
  lists->counts[list]++;
}

}  // namespace NTFS_BROWSER_NAMESPACE::Util
