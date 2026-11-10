#include "alloc/bitmap-windows.h"

#include <algorithm>
#include <cstddef>
#include <exception>
#include <optional>
#include <utility>

#include <ntfs-browser/namespace.h>

#include "log/ntfs-common.h"
#include "win-types.h"

namespace NTFS_BROWSER_NAMESPACE::Alloc {

namespace {

// Windows that may fail before the rest of the bitmap is given up on. A hostile
// volume can claim a bitmap of petabytes: reading it window by window, and
// remembering each failure, would take forever and all the memory. Past this
// many failures, a window that was not read is unknown without being tried.
constexpr size_t max_failed_windows = 4096;

// Most bytes any window has held since the peak was last reset.
size_t peak_bytes_held = 0;

}  // namespace

BitmapWindows::BitmapWindows(Reader reader, ULONGLONG data_size,
                             size_t window_bytes)
    : reader_(std::move(reader)),
      data_size_(data_size),
      window_bytes_(window_bytes) {}

size_t BitmapWindows::WindowBytes() const noexcept { return window_bytes_; }

size_t BitmapWindows::FailedCount() const noexcept { return failed_.size(); }

bool BitmapWindows::GaveUp() const noexcept {
  return failed_.size() >= max_failed_windows;
}

// Reads the window at index unless it is the one held. A failed or short read
// keeps what came back, and the window is remembered so it is not read again.
const BitmapWindows::Window* BitmapWindows::Load(ULONGLONG index) {
  // A kept window answers whatever happened when it was read: a short one
  // keeps the bytes it returned.
  if (const auto kept = retained_.find(index); kept != retained_.end()) {
    return &kept->second;
  }
  if (have_window_ && window_.index == index) {
    return &window_;
  }
  if (failed_.contains(index) || GaveUp()) {
    return nullptr;
  }
  return ReadWindow(index);
}

const BitmapWindows::Window* BitmapWindows::ReadWindow(ULONGLONG index) {
  const ULONGLONG offset = index * window_bytes_;
  const auto length = static_cast<size_t>(
      std::min<ULONGLONG>(window_bytes_, data_size_ - offset));

  window_.index = index;
  window_.bytes.assign(length, 0);
  window_.valid = 0;
  have_window_ = true;
  peak_bytes_held = std::max(peak_bytes_held, BytesHeld());

  std::optional<ULONGLONG> read;
  try {
    read = reader_(offset, window_.bytes);
  } catch (const std::exception& e) {
    Log::Exception(e);
  }
  window_.valid = read ? static_cast<size_t>(std::min<ULONGLONG>(*read, length))
                       : size_t{0};
  if (window_.valid < length) {
    failed_.insert(index);
    Log::Debug("Cluster bitmap window {} read {} of {} bytes", index,
               window_.valid, length);
  }

  if (retained_bytes_ + length > retain_bytes_) {
    return &window_;
  }
  retained_bytes_ += length;
  have_window_ = false;
  return &retained_.emplace(index, std::move(window_)).first->second;
}

void BitmapWindows::SetRetention(size_t max_bytes) {
  retain_bytes_ = max_bytes;
  if (max_bytes == 0) {
    retained_.clear();
    retained_bytes_ = 0;
  }
}

size_t BitmapWindows::BytesHeld() const noexcept {
  return retained_bytes_ + (have_window_ ? window_.bytes.size() : size_t{0});
}

void ResetPeakBytesHeld() noexcept { peak_bytes_held = 0; }

size_t PeakBytesHeld() noexcept { return peak_bytes_held; }

}  // namespace NTFS_BROWSER_NAMESPACE::Alloc
