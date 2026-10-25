#pragma once

#include <cstddef>

#include <ntfs-browser/namespace.h>

#include "internal-export.h"

namespace NTFS_BROWSER_NAMESPACE::Util {

// Memory for small objects that come and go by the million, such as the
// attributes and the headers of a scan of $MFT. Each thread keeps the blocks
// it freed in lists, one list per size, and gives them out again: that costs
// less than asking the heap each time. A block MAY be freed by another thread
// than the one that got it: it joins the lists of the thread that frees it.
class NTFS_BROWSER_EXPORT_TESTS_ONLY SmallBlockPool {
 public:
  // Whether the lists serve blocks. An address sanitizer build turns them off:
  // a block that waits in a list is not poisoned, so a use after free would go
  // unseen. The heap then serves every block.
#if defined(__SANITIZE_ADDRESS__)
  static constexpr bool enabled = false;
#elif defined(__has_feature)
  #if __has_feature(address_sanitizer)
  static constexpr bool enabled = false;
  #else
  static constexpr bool enabled = true;
  #endif
#else
  static constexpr bool enabled = true;
#endif

  // Largest size, in bytes, the lists serve. A larger block comes from the
  // heap, and goes back to it.
  static constexpr size_t max_pooled_size = 512;

  // A block of at least size bytes, aligned for any object. Throws
  // std::bad_alloc when the heap has none.
  [[nodiscard]] static void* Allocate(size_t size);

  // Takes a block back. size MUST be the size it was allocated with, or one of
  // the same list: sizes round up to a multiple of 16.
  static void Deallocate(void* block, size_t size) noexcept;
};

}  // namespace NTFS_BROWSER_NAMESPACE::Util
