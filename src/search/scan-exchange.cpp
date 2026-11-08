#include "search/scan-exchange.h"

#include <cstddef>
#include <mutex>
#include <optional>
#include <stop_token>
#include <utility>

#include <ntfs-browser/namespace.h>
#include <ntfs-browser/search/types.h>

#include "log/ntfs-common.h"

namespace NTFS_BROWSER_NAMESPACE::Search {

ScanExchange::ScanExchange(std::size_t max_pending)
    : max_pending_(max_pending) {}

void ScanExchange::Prepare(Log::Detail::LevelMask levels,
                           std::stop_token stop) {
  stop_ = std::move(stop);
  log_.Prepare(levels);
}

void ScanExchange::BeginWork() noexcept { log_.Begin(); }

void ScanExchange::Add(const Entry& entry) {
  log_.Checkpoint();
  std::unique_lock lock(mutex_);
  if (max_pending_ != 0) {
    filled_.wait(lock, stop_,
                 [this] { return buffers_.at(fill_).Size() < max_pending_; });
  }
  buffers_.at(fill_).Add(entry);
}

void ScanExchange::SetPercent(double percent) {
  log_.Checkpoint();
  percent_.store(percent, std::memory_order_relaxed);
}

void ScanExchange::EndWork(State result) noexcept {
  log_.End();
  {
    const std::scoped_lock lock(mutex_);
    done_ = true;
    result_ = result;
  }
  filled_.notify_all();
}

ScanExchange::Taken ScanExchange::Take() {
  Taken taken;
  {
    const std::scoped_lock lock(mutex_);
    EntryBuffer& full = buffers_.at(fill_);
    if (full.Size() != 0) {
      reading_ = &full;
      taken.entries = full.Entries();
      fill_ = 1 - fill_;
    }
    taken.last = done_;
    if (done_) {
      taken.result = result_;
    }
  }
  filled_.notify_all();
  return taken;
}

void ScanExchange::ReplayLog() noexcept { log_.Replay(); }

void ScanExchange::Release() noexcept {
  if (reading_ != nullptr) {
    // The worker fills the other buffer, so no lock is needed.
    reading_->Clear();
    reading_ = nullptr;
  }
}

std::optional<double> ScanExchange::Percent() const noexcept {
  const double percent = percent_.load(std::memory_order_relaxed);
  if (percent < 0.0) {
    return std::nullopt;
  }
  return percent;
}

}  // namespace NTFS_BROWSER_NAMESPACE::Search
