#include "search/scan-exchange.h"

#include <algorithm>
#include <chrono>
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
  EntryBuffer& buffer = buffers_.at(fill_);
  buffer.Add(entry);
  // The batch cannot grow any more: the caller gains nothing by waiting.
  full_ = max_pending_ != 0 && buffer.Size() >= max_pending_;
  NoteNews();
}

void ScanExchange::SetPercent(double percent) {
  log_.Checkpoint();
  const std::scoped_lock lock(mutex_);
  percent_.store(percent, std::memory_order_relaxed);
  NoteNews();
}

void ScanExchange::EndWork(State result) noexcept {
  log_.End();
  {
    const std::scoped_lock lock(mutex_);
    done_ = true;
    result_ = result;
    ++news_;
  }
  filled_.notify_all();
  changed_.notify_all();
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
    taken_news_ = news_;
    full_ = false;
    taken.last = done_;
    if (done_) {
      taken.result = result_;
    }
  }
  filled_.notify_all();
  return taken;
}

void ScanExchange::WaitForNews(TimePoint news_not_before,
                               std::optional<TimePoint> deadline) {
  std::unique_lock lock(mutex_);
  waiting_ = true;
  const auto urgent = [this] { return done_ || wake_ || full_; };
  const auto news = [this] {
    return done_ || wake_ || full_ || news_ != taken_news_;
  };
  // First the part of the wait where only the strong reasons count.
  const TimePoint hold_until =
      deadline ? std::min(news_not_before, *deadline) : news_not_before;
  const bool strong = std::chrono::steady_clock::now() >= hold_until
                          ? urgent()
                          : changed_.wait_until(lock, hold_until, urgent);
  if (!strong) {
    if (deadline) {
      changed_.wait_until(lock, *deadline, news);
    } else {
      changed_.wait(lock, news);
    }
  }
  waiting_ = false;
  wake_ = false;
}

void ScanExchange::Wake() noexcept {
  {
    const std::scoped_lock lock(mutex_);
    wake_ = true;
  }
  changed_.notify_all();
}

// Counts one piece of news for WaitForNews(). The caller holds mutex_.
void ScanExchange::NoteNews() {
  ++news_;
  if (waiting_) {
    changed_.notify_all();
  }
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
