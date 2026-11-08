#include <algorithm>
#include <cstddef>
#include <exception>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <utility>

#include <ntfs-browser/namespace.h>
#include <ntfs-browser/search/scan.h>
#include <ntfs-browser/search/types.h>

#include "log/ntfs-common.h"
#include "search/engine.h"
#include "search/entry-buffer.h"
#include "search/target.h"
#include "utf/utf.h"

namespace NTFS_BROWSER_NAMESPACE::Search {

namespace {

// Entries in one batch at most. Poll() hands over this many and returns: a
// caller that polls every 100 ms is never held longer than a callback over this
// many entries takes. A run of entries a worker found in one slice is no
// larger than this either, once the scan has its own thread.
constexpr std::size_t max_batch_entries = 4096;

// Keeps what an engine reports: every entry in a buffer, and the percent.
class BufferSink final : public EntrySink {
 public:
  explicit BufferSink(EntryBuffer& buffer) : buffer_(buffer) {}

  void Add(const Entry& entry) override { buffer_.Add(entry); }

  void SetPercent(double percent) override { percent_ = percent; }

  [[nodiscard]] const std::optional<double>& Percent() const noexcept {
    return percent_;
  }

 private:
  EntryBuffer& buffer_;
  std::optional<double> percent_;
};

}  // namespace

// The state of a scan. Today the engine runs inside Start() and fills one
// buffer; Poll() hands that buffer over in slices. The worker thread of the
// next step replaces buffer_ with two buffers that it swaps under a lock and
// runs the engine from a thread, and keeps the rest.
class Scan::Impl {
 public:
  explicit Impl(Options options) : options_(std::move(options)) {}

  void SetCallback(BatchCallback callback) { callback_ = std::move(callback); }

  void Start() {
    if (started_) {
      return;
    }
    started_ = true;
    try {
      result_ = Run();
    } catch (const std::exception& error) {
      Log::Error("The scan failed: {}", error.what());
      result_ = State::Failed;
    } catch (...) {
      Log::Error("The scan failed");
      result_ = State::Failed;
    }
  }

  State Poll() {
    if (!started_ || final_delivered_) {
      return GetState();
    }
    const std::size_t found = buffer_.Size();
    const std::size_t count = std::min(found - delivered_, max_batch_entries);
    const bool last = delivered_ + count == found;
    const bool progress_changed = percent_ != reported_percent_;
    if (count == 0 && !last && !progress_changed) {
      return GetState();
    }

    progress_.entries_found = delivered_ + count;
    progress_.percent = percent_;
    reported_percent_ = percent_;
    const Batch batch{.entries = buffer_.Entries().subspan(delivered_, count),
                      .progress = progress_,
                      .last = last};
    delivered_ += count;
    if (last) {
      final_delivered_ = true;
    }
    if (callback_) {
      callback_(batch);
    }
    return GetState();
  }

  void Cancel() noexcept {
    if (started_ && !final_delivered_) {
      cancelled_ = true;
    }
    stop_.request_stop();
  }

  [[nodiscard]] State GetState() const noexcept {
    if (!started_) {
      return State::NotStarted;
    }
    if (!final_delivered_) {
      return State::Running;
    }
    if (result_ == State::Failed) {
      return State::Failed;
    }
    return cancelled_ ? State::Cancelled : State::Finished;
  }

  [[nodiscard]] Progress GetProgress() const noexcept { return progress_; }

 private:
  // Splits the target, makes the engine and runs it into buffer_. Returns the
  // state the scan ends in when nobody cancels it.
  State Run() {
    const std::optional<TargetParts> parts =
        SplitTarget(Utf::Utf16ToWide(options_.target));
    if (!parts) {
      Log::Error("Cannot find a device or image file in {}",
                 Utf::WideToUtf8(Utf::Utf16ToWide(options_.target)));
      return State::Failed;
    }
    const std::unique_ptr<Engine> engine = MakeEngine(options_, *parts);
    if (!engine) {
      return State::Failed;
    }
    BufferSink sink(buffer_);
    const bool ran = engine->Run(sink, stop_.get_token());
    percent_ = sink.Percent();
    return ran ? State::Finished : State::Failed;
  }

  Options options_;
  BatchCallback callback_;
  std::stop_source stop_;
  EntryBuffer buffer_;

  bool started_{false};
  // How the engine ended: Finished, or Failed. Set by Start().
  State result_{State::NotStarted};
  bool cancelled_{false};
  // Whether Poll() has delivered the batch that has last set.
  bool final_delivered_{false};
  // Entries of buffer_ that Poll() handed over.
  std::size_t delivered_{0};
  std::optional<double> percent_;
  std::optional<double> reported_percent_;
  Progress progress_;
};

Scan::Scan(Options options)
    : impl_(std::make_unique<Impl>(std::move(options))) {}

Scan::~Scan() {
  if (impl_) {
    impl_->Cancel();
  }
}

void Scan::OnBatch(BatchCallback callback) {
  impl_->SetCallback(std::move(callback));
}

void Scan::Start() { impl_->Start(); }

State Scan::Poll() { return impl_->Poll(); }

void Scan::Cancel() noexcept { impl_->Cancel(); }

State Scan::GetState() const noexcept { return impl_->GetState(); }

bool Scan::Finished() const noexcept {
  const State state = impl_->GetState();
  return state != State::NotStarted && state != State::Running;
}

Progress Scan::GetProgress() const noexcept { return impl_->GetProgress(); }

}  // namespace NTFS_BROWSER_NAMESPACE::Search
