#include "io/guarded-disk-reader.h"

#include <atomic>
#include <mutex>
#include <optional>
#include <span>
#include <string_view>

#include <ntfs-browser/namespace.h>

#include "disk-reader.h"
#include "tuning/tuning.h"
#include "win-types.h"

namespace NTFS_BROWSER_NAMESPACE::Io {

bool GuardedDiskReader::Open(std::wstring_view path) {
  const std::scoped_lock lock{mutex_};
  return inner_ != nullptr && inner_->Open(path);
}

bool GuardedDiskReader::Read(LONGLONG addr, std::span<BYTE> dest) {
  const std::scoped_lock lock{mutex_};
  if (inner_ == nullptr) {
    return false;
  }

  LARGE_INTEGER at{.QuadPart = addr};
  return inner_->ReadInto(at, dest);
}

bool GuardedDiskReader::ReadUnlessCancelled(const std::atomic<bool>& cancelled,
                                            LONGLONG addr,
                                            std::span<BYTE> dest) {
  const std::scoped_lock lock{mutex_};
  // Dropped while the worker waited for the wrapped reader.
  if (cancelled || inner_ == nullptr) {
    return false;
  }

  try {
    LARGE_INTEGER at{.QuadPart = addr};
    return inner_->ReadInto(at, dest);
  } catch (...) {
    // An exception must not end the process from a thread of its own.
    return false;
  }
}

std::optional<ULONGLONG> GuardedDiskReader::Size() {
  const std::scoped_lock lock{mutex_};
  return inner_ != nullptr ? inner_->Size() : std::nullopt;
}

Tuning::MediaKind GuardedDiskReader::GetMediaKind() {
  const std::scoped_lock lock{mutex_};
  return inner_ != nullptr ? inner_->GetMediaKind()
                           : Tuning::MediaKind::Unknown;
}

void GuardedDiskReader::Prefetch(ULONGLONG offset, ULONGLONG length) {
  const std::scoped_lock lock{mutex_};
  if (inner_ != nullptr) {
    inner_->Prefetch(offset, length);
  }
}

void GuardedDiskReader::CancelPrefetch() {
  const std::scoped_lock lock{mutex_};
  if (inner_ != nullptr) {
    inner_->CancelPrefetch();
  }
}

void GuardedDiskReader::PrefetchStanding(ULONGLONG offset, ULONGLONG length) {
  const std::scoped_lock lock{mutex_};
  if (inner_ != nullptr) {
    inner_->PrefetchStanding(offset, length);
  }
}

void GuardedDiskReader::CancelStanding(ULONGLONG offset, ULONGLONG length) {
  const std::scoped_lock lock{mutex_};
  if (inner_ != nullptr) {
    inner_->CancelStanding(offset, length);
  }
}

void GuardedDiskReader::SetReadPattern(Tuning::Pattern pattern) {
  const std::scoped_lock lock{mutex_};
  if (inner_ != nullptr) {
    inner_->SetReadPattern(pattern);
  }
}

}  // namespace NTFS_BROWSER_NAMESPACE::Io
