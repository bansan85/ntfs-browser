#pragma once

#include <ntfs-browser/win-types.h>

#include <cstddef>
#include <memory>
#include <span>
#include <type_traits>

#include <ntfs-browser/cache/strategy.h>
#include <ntfs-browser/export.h>
#include <ntfs-browser/namespace.h>

namespace NTFS_BROWSER_NAMESPACE::Cache {

// Declared only: its definition is internal to the library, so a consumer
// cannot build a View from raw bytes.
class ViewKey;

// Bytes read from a FullCache volume, plus what keeps them alive.
//
// The bytes stay valid and unchanged while any copy of the view exists and the
// volume is open. Closing the volume makes them unusable. Converting a view to
// a bare std::span gives up that guarantee.
//
// Not thread safe: a view MUST be dropped on the thread that uses the volume,
// since dropping the last one of a block may let the cache free memory.
class NTFS_BROWSER_EXPORT View {
 public:
  // An empty view. It holds nothing.
  View() noexcept;
  // A copy shares the memory of the original, so both keep the bytes valid.
  View(const View& other) noexcept;
  View(View&& other) noexcept;
  View& operator=(const View& other) noexcept;
  View& operator=(View&& other) noexcept;
  // May let the cache free memory it was holding for this view. Never throws.
  ~View();

  // The viewed bytes, without their owner.
  // NOLINTNEXTLINE(google-explicit-constructor): implicit on purpose.
  operator std::span<const BYTE>() const noexcept;
  [[nodiscard]] const BYTE* data() const noexcept;
  [[nodiscard]] std::size_t size() const noexcept;
  [[nodiscard]] bool empty() const noexcept;

  // Builds a populated view. owner keeps the bytes alive. cache is the cache
  // to trim once the last holder of owner goes away. Only the library can
  // name key.
  View(const ViewKey& key, std::span<const BYTE> bytes,
       std::shared_ptr<const void> owner, std::weak_ptr<void> cache) noexcept;

 private:
  void Swap(View& other) noexcept;

  std::span<const BYTE> bytes_;
  std::shared_ptr<const void> owner_;
  std::weak_ptr<void> cache_;
};

// What Read() returns for a strategy: a View under FullCache, and the bare
// span under NoCache, which holds its memory in the reader itself.
template <Strategy S>
using ReadResult =
    std::conditional_t<S == Strategy::FullCache, View, std::span<const BYTE>>;

}  // namespace NTFS_BROWSER_NAMESPACE::Cache
