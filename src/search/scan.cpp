#include <cstdint>
#include <exception>
#include <memory>
#include <optional>
#include <stop_token>
#include <thread>
#include <utility>

#include <ntfs-browser/namespace.h>
#include <ntfs-browser/search/scan.h>
#include <ntfs-browser/search/types.h>

#include "log/ntfs-common.h"
#include "search/engine.h"
#include "search/field-filter.h"
#include "search/scan-exchange.h"
#include "search/target.h"
#include "utf/utf.h"

namespace NTFS_BROWSER_NAMESPACE::Search {

// The state of a scan. A worker thread runs the engine into exchange_. The
// caller's thread owns every member that is not part of the exchange: Start(),
// Poll(), Cancel() and the callback run there. The worker touches the options
// (read only), the stop source (thread safe) and the exchange.
class Scan::Impl {
 public:
  explicit Impl(Options options)
      : options_(Normalized(std::move(options))),
        exchange_(options_.max_pending_entries) {}

  Impl(const Impl&) = delete;
  Impl& operator=(const Impl&) = delete;
  Impl(Impl&&) = delete;
  Impl& operator=(Impl&&) = delete;

  // Stops the worker and waits for it: no thread outlives the scan.
  ~Impl() {
    stop_.request_stop();
    Join();
  }

  void SetCallback(BatchCallback callback) { callback_ = std::move(callback); }

  void Start() {
    if (started_) {
      return;
    }
    started_ = true;
    exchange_.Prepare(Log::Detail::EnabledLevels(), stop_.get_token());
    try {
      worker_ = std::thread([this] { Work(); });
    } catch (const std::exception& error) {
      Log::Error("The scan thread did not start: {}", error.what());
      exchange_.BeginWork();
      exchange_.EndWork(State::Failed);
    }
  }

  State Poll() {
    if (!started_ || final_delivered_ || polling_) {
      return GetState();
    }
    const PollGuard guard(*this);
    const ScanExchange::Taken taken = exchange_.Take();
    // After Take(): a worker that ended has its log whole by then.
    exchange_.ReplayLog();

    const std::optional<double> percent = exchange_.Percent();
    const bool progress_changed = percent != reported_percent_;
    if (taken.entries.empty() && !taken.last && !progress_changed) {
      return GetState();
    }

    delivered_ += taken.entries.size();
    progress_.entries_found = delivered_;
    progress_.percent = percent;
    reported_percent_ = percent;
    if (taken.last) {
      final_delivered_ = true;
      result_ = taken.result;
    }
    if (callback_) {
      callback_(Batch{
          .entries = taken.entries, .progress = progress_, .last = taken.last});
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
  // options with what the scan needs of itself: a pattern on the full path
  // needs the paths, and a filter on the data of deleted files needs the data
  // state.
  static Options Normalized(Options options) {
    if (NeedsOf(options.filter).deleted_data_state) {
      options.deleted_data_state = true;
    }
    if (options.pattern.kind != PatternKind::None &&
        options.pattern.target == MatchTarget::FullPath) {
      options.full_path = true;
    }
    return options;
  }

  // Marks a Poll() as running, so that a Poll() from the callback does nothing,
  // and gives the entries back when the Poll() ends, even by an exception of
  // the callback.
  class PollGuard {
   public:
    explicit PollGuard(Impl& scan) noexcept : scan_(scan) {
      scan_.polling_ = true;
    }

    PollGuard(const PollGuard&) = delete;
    PollGuard& operator=(const PollGuard&) = delete;
    PollGuard(PollGuard&&) = delete;
    PollGuard& operator=(PollGuard&&) = delete;

    ~PollGuard() {
      scan_.exchange_.Release();
      scan_.polling_ = false;
    }

   private:
    Impl& scan_;
  };

  // The body of the worker thread. Nothing leaves it: an exception ends the
  // scan as Failed, and its text goes to the captured log.
  void Work() noexcept {
    exchange_.BeginWork();
    State result = State::Failed;
    try {
      result = Run();
    } catch (const std::exception& error) {
      Log::Error("The scan failed: {}", error.what());
    } catch (...) {
      Log::Error("The scan failed");
    }
    exchange_.EndWork(result);
  }

  // Splits the target, makes the engine and runs it into exchange_. Returns
  // the state the scan ends in when nobody cancels it. Runs on the worker.
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
    return engine->Run(exchange_, stop_.get_token()) ? State::Finished
                                                     : State::Failed;
  }

  // Waits for the worker. Never throws.
  void Join() noexcept {
    try {
      if (worker_.joinable()) {
        worker_.join();
      }
    } catch (...) {
      // A thread that cannot be joined cannot be helped here.
    }
  }

  Options options_;
  BatchCallback callback_;
  std::stop_source stop_;
  ScanExchange exchange_;

  bool started_{false};
  // How the engine ended, as the last batch reported it.
  State result_{State::NotStarted};
  bool cancelled_{false};
  // Whether Poll() has delivered the batch that has last set.
  bool final_delivered_{false};
  // A Poll() is running: it is not reentrant.
  bool polling_{false};
  // Entries handed to the callback so far.
  std::uint64_t delivered_{0};
  std::optional<double> reported_percent_;
  Progress progress_;
  // Declared last: the worker uses every member above.
  std::thread worker_;
};

Scan::Scan(Options options)
    : impl_(std::make_unique<Impl>(std::move(options))) {}

Scan::~Scan() = default;

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
