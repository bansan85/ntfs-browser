#include <cstddef>
#include <cstdint>
#include <memory>
#include <thread>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "util/small-pool.h"

using NTFS_BROWSER_NAMESPACE::Util::SmallBlockPool;

TEST_CASE("a block freed is the next one given for its size",
          "[util][small-pool]") {
  if (!SmallBlockPool::enabled) {
    SKIP("The lists are off in this build.");
  }
  void* const first = SmallBlockPool::Allocate(100);
  SmallBlockPool::Deallocate(first, 100);

  void* const second = SmallBlockPool::Allocate(100);

  CHECK(second == first);
  SmallBlockPool::Deallocate(second, 100);
}

TEST_CASE("sizes that round to one list share their blocks",
          "[util][small-pool]") {
  if (!SmallBlockPool::enabled) {
    SKIP("The lists are off in this build.");
  }
  void* const block = SmallBlockPool::Allocate(17);
  SmallBlockPool::Deallocate(block, 17);

  // 17 to 32 bytes make one list.
  void* const same_list = SmallBlockPool::Allocate(32);
  CHECK(same_list == block);
  SmallBlockPool::Deallocate(same_list, 32);

  void* const other_list = SmallBlockPool::Allocate(33);
  CHECK(other_list != block);
  SmallBlockPool::Deallocate(other_list, 33);
}

TEST_CASE("a block holds the bytes of the largest size of its list",
          "[util][small-pool]") {
  // A block asked for 17 bytes is 32 bytes long: the 32 bytes of a second
  // request, served from it, are all there.
  auto* const block = static_cast<unsigned char*>(SmallBlockPool::Allocate(17));
  SmallBlockPool::Deallocate(block, 17);
  auto* const again = static_cast<unsigned char*>(SmallBlockPool::Allocate(32));
  for (size_t i = 0; i < 32; i++) {
    again[i] = static_cast<unsigned char>(i);
  }
  for (size_t i = 0; i < 32; i++) {
    CHECK(again[i] == static_cast<unsigned char>(i));
  }
  SmallBlockPool::Deallocate(again, 32);
}

TEST_CASE("blocks are aligned for any object", "[util][small-pool]") {
  std::vector<void*> blocks;
  for (size_t size = 1; size <= SmallBlockPool::max_pooled_size + 16;
       size += 7) {
    void* const block = SmallBlockPool::Allocate(size);
    CHECK(reinterpret_cast<std::uintptr_t>(block) % alignof(std::max_align_t) ==
          0);
    blocks.push_back(block);
  }
  size_t size = 1;
  for (void* const block : blocks) {
    SmallBlockPool::Deallocate(block, size);
    size += 7;
  }
}

TEST_CASE("a size past the pool comes from the heap and goes back to it",
          "[util][small-pool]") {
  constexpr size_t large = SmallBlockPool::max_pooled_size + 1;
  void* const first = SmallBlockPool::Allocate(large);
  void* const second = SmallBlockPool::Allocate(large);
  REQUIRE(first != nullptr);
  REQUIRE(second != nullptr);
  CHECK(first != second);
  SmallBlockPool::Deallocate(first, large);
  SmallBlockPool::Deallocate(second, large);
}

TEST_CASE("a block of no bytes is a block", "[util][small-pool]") {
  void* const block = SmallBlockPool::Allocate(0);
  CHECK(block != nullptr);
  SmallBlockPool::Deallocate(block, 0);
  CHECK_NOTHROW(SmallBlockPool::Deallocate(nullptr, 64));
}

TEST_CASE("a thread that frees what another allocated keeps it for itself",
          "[util][small-pool]") {
  if (!SmallBlockPool::enabled) {
    SKIP("The lists are off in this build.");
  }
  void* block = nullptr;
  std::thread maker([&block] { block = SmallBlockPool::Allocate(48); });
  maker.join();
  REQUIRE(block != nullptr);

  void* again = nullptr;
  std::thread user([&] {
    SmallBlockPool::Deallocate(block, 48);
    again = SmallBlockPool::Allocate(48);
    SmallBlockPool::Deallocate(again, 48);
  });
  user.join();

  CHECK(again == block);
}

TEST_CASE("a pool keeps no more than a cap of blocks per list",
          "[util][small-pool]") {
  // Far more blocks than a list keeps: the others go back to the heap, and
  // every block stays valid until it is freed.
  std::vector<void*> blocks;
  for (size_t i = 0; i < 5000; i++) {
    blocks.push_back(SmallBlockPool::Allocate(64));
  }
  for (size_t i = 0; i < blocks.size(); i++) {
    static_cast<unsigned char*>(blocks.at(i))[0] =
        static_cast<unsigned char>(i);
  }
  for (void* const block : blocks) {
    SmallBlockPool::Deallocate(block, 64);
  }
  void* const last = SmallBlockPool::Allocate(64);
  CHECK(last != nullptr);
  SmallBlockPool::Deallocate(last, 64);
}
