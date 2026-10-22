#include <ntfs-browser/win-types.h>

#include <cstddef>
#include <memory>
#include <span>
#include <utility>

#include <ntfs-browser/cache/view.h>
#include <ntfs-browser/namespace.h>

#include "cache/block-cache.h"

namespace NTFS_BROWSER_NAMESPACE {
namespace Cache {

class ViewKey;

}  // namespace Cache
}  // namespace NTFS_BROWSER_NAMESPACE

namespace NTFS_BROWSER_NAMESPACE::Cache {

View::View() noexcept = default;

View::View(const ViewKey& /*key*/, std::span<const BYTE> bytes,
           std::shared_ptr<const void> owner,
           std::weak_ptr<void> cache) noexcept
    : bytes_(bytes), owner_(std::move(owner)), cache_(std::move(cache)) {}

View::View(const View& other) noexcept = default;

View::View(View&& other) noexcept
    : bytes_(std::exchange(other.bytes_, {})),
      owner_(std::move(other.owner_)),
      cache_(std::move(other.cache_)) {}

View& View::operator=(const View& other) noexcept {
  View copy(other);
  Swap(copy);
  return *this;
}

View& View::operator=(View&& other) noexcept {
  View moved(std::move(other));
  Swap(moved);
  return *this;
}

// Lets the previous holder's destructor do the trimming.
void View::Swap(View& other) noexcept {
  std::swap(bytes_, other.bytes_);
  owner_.swap(other.owner_);
  cache_.swap(other.cache_);
}

View::~View() {
  // A default-constructed or moved-from view holds nothing to give back.
  if (!owner_) {
    return;
  }
  owner_.reset();
  if (const std::shared_ptr<void> cache = cache_.lock()) {
    static_cast<BlockCache*>(cache.get())->Trim();
  }
}

View::operator std::span<const BYTE>() const noexcept { return bytes_; }

const BYTE* View::data() const noexcept { return bytes_.data(); }

std::size_t View::size() const noexcept { return bytes_.size(); }

bool View::empty() const noexcept { return bytes_.empty(); }

}  // namespace NTFS_BROWSER_NAMESPACE::Cache
