#include "bench/recorder.h"

#include <ntfs-browser/win-types.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

#include <ntfs-browser/bench/bench.h>
#include <ntfs-browser/namespace.h>
#include <ntfs-browser/tuning/tuning.h>

#ifdef _WIN32
  #include <windows.h>
#else
  #include <sys/resource.h>
  #include <time.h>
#endif

namespace NTFS_BROWSER_NAMESPACE::Bench {

namespace Detail {

std::atomic<unsigned> g_flags{0};

}  // namespace Detail

namespace {

using Clock = std::chrono::steady_clock;

// Nanoseconds of a duration, as an unsigned count.
std::uint64_t Nanoseconds(Clock::duration duration) noexcept {
  return static_cast<std::uint64_t>(
      (std::max)(std::chrono::nanoseconds::rep{0},
                 std::chrono::duration_cast<std::chrono::nanoseconds>(duration)
                     .count()));
}

#ifdef _WIN32
// Ticks of a FILETIME are 100 nanoseconds.
constexpr std::uint64_t ns_per_filetime_tick = 100;

std::uint64_t FileTimeNs(const FILETIME& time) noexcept {
  return ((static_cast<std::uint64_t>(time.dwHighDateTime) << 32U) |
          time.dwLowDateTime) *
         ns_per_filetime_tick;
}
#else
constexpr std::uint64_t ns_per_second = 1'000'000'000;

std::uint64_t TimespecNs(const timespec& time) noexcept {
  return (static_cast<std::uint64_t>(time.tv_sec) * ns_per_second) +
         static_cast<std::uint64_t>(time.tv_nsec);
}

std::uint64_t TimevalNs(const timeval& time) noexcept {
  constexpr std::uint64_t ns_per_microsecond = 1000;
  return (static_cast<std::uint64_t>(time.tv_sec) * ns_per_second) +
         (static_cast<std::uint64_t>(time.tv_usec) * ns_per_microsecond);
}
#endif

// User and kernel CPU time of the calling thread.
std::pair<std::uint64_t, std::uint64_t> ThreadTimes() noexcept {
#ifdef _WIN32
  FILETIME created{};
  FILETIME exited{};
  FILETIME kernel{};
  FILETIME user{};
  if (GetThreadTimes(GetCurrentThread(), &created, &exited, &kernel, &user) ==
      FALSE) {
    return {0, 0};
  }
  return {FileTimeNs(user), FileTimeNs(kernel)};
#else
  timespec time{};
  if (clock_gettime(CLOCK_THREAD_CPUTIME_ID, &time) != 0) {
    return {0, 0};
  }
  return {TimespecNs(time), 0};
#endif
}

// User and kernel CPU time of the whole process.
std::pair<std::uint64_t, std::uint64_t> ProcessTimes() noexcept {
#ifdef _WIN32
  FILETIME created{};
  FILETIME exited{};
  FILETIME kernel{};
  FILETIME user{};
  if (GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user) ==
      FALSE) {
    return {0, 0};
  }
  return {FileTimeNs(user), FileTimeNs(kernel)};
#else
  rusage usage{};
  if (getrusage(RUSAGE_SELF, &usage) != 0) {
    return {0, 0};
  }
  return {TimevalNs(usage.ru_utime), TimevalNs(usage.ru_stime)};
#endif
}

// Adds value to the power of two bucket of histogram.
void Add(Histogram& histogram, std::uint64_t value) noexcept {
  const std::size_t bucket =
      value == 0
          ? 0
          : (std::min)(static_cast<std::size_t>(std::bit_width(value) - 1),
                       histogram.buckets.size() - 1);
  histogram.buckets.at(bucket)++;
  histogram.count++;
  histogram.sum += value;
  histogram.max = (std::max)(histogram.max, value);
}

// Everything a session gathers. One instance, guarded where several threads
// may reach it (the thread reports); the rest belongs to the main thread.
struct State {
  Clock::time_point start;
  std::uint64_t start_unix_ns{0};
  std::uint64_t main_user_begin{0};
  std::uint64_t main_kernel_begin{0};
  SessionOptions options;
  Report report;
  ReadLog log;
  std::array<PhaseStat, phase_count> phases{};
  // Phases that began and did not end, innermost last.
  std::vector<Phase> stack;
  std::uint64_t last_read_end{0};
  std::optional<Phase> stop_after;
  std::atomic<bool> stop_requested{false};
  std::shared_ptr<ICaptureSink> capture;
  std::mutex thread_mutex;
  std::vector<ThreadStat> threads;
  // The last position the disk was asked for, to count the seeks.
  std::optional<std::uint64_t> disk_end;
  bool started{false};
};

State& Get() {
  static State state;
  return state;
}

// The Phase that is innermost, or 0 when none runs.
std::uint8_t CurrentPhaseIndex(const State& state) noexcept {
  return state.stack.empty() ? std::uint8_t{0}
                             : static_cast<std::uint8_t>(state.stack.back());
}

// Pushes event on the log unless the cap is reached.
void Push(State& state, const LogEvent& event) {
  if (!state.options.keep_log) {
    return;
  }
  if (state.log.events.size() >= state.options.max_events) {
    state.report.events_dropped++;
    return;
  }
  state.log.events.push_back(event);
}

thread_local Detail::ReadNote t_note;
// What the slices nested in the running one gathered: their time, and the time
// the thread waited for the disk inside them.
thread_local std::uint64_t t_child_ns = 0;
thread_local std::uint64_t t_child_wait = 0;
thread_local ReadKind t_kind = ReadKind::Other;

void RefreshFlags(const State& state) noexcept {
  unsigned flags = 0;
  if (state.started) {
    flags |= Detail::flag_session;
  }
  if (state.capture) {
    flags |= Detail::flag_capture;
  }
  if (state.stop_after) {
    flags |= Detail::flag_stop;
  }
  Detail::g_flags.store(flags, std::memory_order_relaxed);
}

}  // namespace

namespace Detail {

std::uint64_t NowNs() noexcept {
  return Nanoseconds(Clock::now() - Get().start);
}

std::uint64_t ThreadCpuNs() noexcept {
  const auto [user, kernel] = ThreadTimes();
  return user + kernel;
}

void ResetReadNote() noexcept { t_note = ReadNote{}; }

const ReadNote& LastReadNote() noexcept { return t_note; }

void NoteRead(ReadClass cls, std::uint64_t wait_ns,
              std::uint32_t queue_depth) noexcept {
  t_note = {.cls = cls,
            .wait_ns = wait_ns,
            .queue_depth = queue_depth,
            .valid = true};
}

void NoteDiskRead(std::uint64_t offset, std::uint64_t bytes,
                  DiskReadOrigin origin, std::uint32_t queue_depth) noexcept {
  if (!SessionOn()) {
    return;
  }
  DiskStat& disk = Get().report.disk;
  State& state = Get();
  disk.reads++;
  disk.bytes += bytes;
  if (origin == DiskReadOrigin::ReadAhead) {
    disk.prefetch_reads++;
    disk.prefetch_bytes += bytes;
  } else if (origin == DiskReadOrigin::Standing) {
    disk.standing_reads++;
    disk.standing_bytes += bytes;
  }
  Add(disk.sizes, bytes);
  disk.queue_depth.at((std::min)(static_cast<std::size_t>(queue_depth),
                                 disk.queue_depth.size() - 1))++;
  if (state.disk_end && *state.disk_end != offset) {
    disk.seeks++;
    disk.seek_bytes += offset > *state.disk_end ? offset - *state.disk_end
                                                : *state.disk_end - offset;
  }
  state.disk_end = offset + bytes;
}

void NoteDiskWaste(std::uint64_t bytes, bool standing) noexcept {
  if (SessionOn()) {
    DiskStat& disk = Get().report.disk;
    (standing ? disk.standing_wasted_bytes : disk.wasted_bytes) += bytes;
  }
}

void RecordRead(std::uint64_t offset, std::uint64_t length,
                std::uint64_t start_ns, std::uint64_t end_ns) noexcept {
  if (!SessionOn()) {
    return;
  }
  State& state = Get();
  Report& report = state.report;
  // A reader that notes nothing made the caller wait for the whole call.
  const ReadNote& note = t_note;
  const std::uint64_t wait_ns = note.valid ? note.wait_ns : end_ns - start_ns;
  const ReadClass cls = note.valid ? note.cls : ReadClass::Unclassified;
  const std::uint8_t phase = CurrentPhaseIndex(state);

  report.requested_reads++;
  report.requested_bytes += length;
  report.read_call_ns += end_ns - start_ns;
  report.read_wait_ns += wait_ns;
  state.last_read_end = (std::max)(state.last_read_end, end_ns);

  KindStat& kind = report.kinds.at(static_cast<std::size_t>(t_kind));
  kind.reads++;
  kind.bytes += length;
  kind.wait_ns += wait_ns;
  Add(kind.sizes, length);
  kind.by_class.at(static_cast<std::size_t>(cls))++;
  report.class_reads.at(static_cast<std::size_t>(cls))++;
  report.class_wait_ns.at(static_cast<std::size_t>(cls)) += wait_ns;

  if (!state.stack.empty()) {
    PhaseStat& stat = state.phases.at(phase);
    stat.reads++;
    stat.read_bytes += length;
    stat.read_wait_ns += wait_ns;
  }

  Push(state, {.start_ns = start_ns,
               .end_ns = end_ns,
               .wait_ns = wait_ns,
               .offset = offset,
               .length = length,
               .type = EventType::Read,
               .kind = t_kind,
               .cls = cls,
               .phase = phase,
               .queue_depth = note.queue_depth});
}

void RecordPrefetch(std::uint64_t offset, std::uint64_t length) noexcept {
  if (!SessionOn()) {
    return;
  }
  State& state = Get();
  const std::uint64_t now = NowNs();
  state.report.prefetch_calls++;
  state.report.prefetch_bytes += length;
  Push(state, {.start_ns = now,
               .end_ns = now,
               .offset = offset,
               .length = length,
               .type = EventType::Prefetch,
               .kind = t_kind,
               .phase = CurrentPhaseIndex(state)});
}

void RecordCancel() noexcept {
  if (!SessionOn()) {
    return;
  }
  State& state = Get();
  const std::uint64_t now = NowNs();
  state.report.cancel_calls++;
  Push(state, {.start_ns = now,
               .end_ns = now,
               .type = EventType::Cancel,
               .phase = CurrentPhaseIndex(state)});
}

void RecordStandingPrefetch(std::uint64_t offset,
                            std::uint64_t length) noexcept {
  if (!SessionOn()) {
    return;
  }
  State& state = Get();
  const std::uint64_t now = NowNs();
  state.report.standing_prefetch_calls++;
  state.report.standing_prefetch_bytes += length;
  Push(state, {.start_ns = now,
               .end_ns = now,
               .offset = offset,
               .length = length,
               .type = EventType::StandingPrefetch,
               .kind = t_kind,
               .phase = CurrentPhaseIndex(state)});
}

void RecordStandingCancel(std::uint64_t offset, std::uint64_t length) noexcept {
  if (!SessionOn()) {
    return;
  }
  State& state = Get();
  const std::uint64_t now = NowNs();
  state.report.standing_cancel_calls++;
  Push(state, {.start_ns = now,
               .end_ns = now,
               .offset = offset,
               .length = length,
               .type = EventType::StandingCancel,
               .phase = CurrentPhaseIndex(state)});
}

void RecordPattern(Tuning::Pattern pattern) noexcept {
  if (!SessionOn()) {
    return;
  }
  State& state = Get();
  const std::uint64_t now = NowNs();
  Push(state, {.start_ns = now,
               .end_ns = now,
               .length = static_cast<std::uint64_t>(pattern),
               .type = EventType::Pattern,
               .phase = CurrentPhaseIndex(state)});
}

void NoteVolumeBytes(std::uint64_t bytes) noexcept {
  if (SessionOn()) {
    Get().report.volume_bytes = bytes;
  }
}

void NoteMediaKind(Tuning::MediaKind kind) noexcept {
  if (SessionOn()) {
    Get().report.media_kind = kind;
  }
}

void CaptureRead(std::uint64_t offset, std::span<const BYTE> bytes) {
  if ((g_flags.load(std::memory_order_relaxed) & flag_capture) == 0) {
    return;
  }
  const std::shared_ptr<ICaptureSink>& sink = Get().capture;
  if (sink) {
    sink->Write(offset, bytes);
  }
}

SliceMark SliceBegin() noexcept {
  SliceMark mark{.begin_ns = NowNs(),
                 .wait_ns = Get().report.read_wait_ns,
                 .saved_child_ns = t_child_ns,
                 .saved_child_wait = t_child_wait};
  t_child_ns = 0;
  t_child_wait = 0;
  return mark;
}

void SliceEnd(Slice slice, const SliceMark& mark) noexcept {
  State& state = Get();
  const std::uint64_t total = NowNs() - mark.begin_ns;
  const std::uint64_t waited = state.report.read_wait_ns - mark.wait_ns;
  // What the slice spent itself: not in a slice inside it, not on the disk.
  const std::uint64_t taken = t_child_ns + (waited - t_child_wait);
  state.report.slices_ns.at(static_cast<std::size_t>(slice)) +=
      total > taken ? total - taken : 0;
  t_child_ns = mark.saved_child_ns + total;
  t_child_wait = mark.saved_child_wait + waited;
}

ReadKind CurrentKind() noexcept { return t_kind; }

void SetCurrentKind(ReadKind kind) noexcept { t_kind = kind; }

}  // namespace Detail

std::string_view PhaseName(Phase phase) noexcept {
  constexpr std::array<std::string_view, phase_count> names{
      "open", "pass1",   "pass2", "flush", "sweep",  "check",
      "link", "journal", "walk",  "print", "destroy"};
  return names.at(static_cast<std::size_t>(phase));
}

std::string_view ReadKindName(ReadKind kind) noexcept {
  constexpr std::array<std::string_view, read_kind_count> names{
      "other",     "boot",           "mft_window",  "record",
      "extension", "attribute_list", "index_block", "bitmap_window",
      "data",      "journal",        "probe_run",   "content_check"};
  return names.at(static_cast<std::size_t>(kind));
}

std::string_view ReadClassName(ReadClass cls) noexcept {
  constexpr std::array<std::string_view, read_class_count> names{
      "unclassified", "io", "window", "ready", "plan"};
  return names.at(static_cast<std::size_t>(cls));
}

std::string_view ThreadRoleName(ThreadRole role) noexcept {
  constexpr std::array<std::string_view, thread_role_count> names{
      "main", "pool_worker", "output_writer", "read_ahead", "other"};
  return names.at(static_cast<std::size_t>(role));
}

std::string_view SliceName(Slice slice) noexcept {
  constexpr std::array<std::string_view, slice_count> names{
      "parse", "merge", "pool_wait", "log_replay"};
  return names.at(static_cast<std::size_t>(slice));
}

std::optional<Phase> ParsePhase(std::string_view name) noexcept {
  for (std::size_t index = 0; index < phase_count; index++) {
    const auto phase = static_cast<Phase>(index);
    if (PhaseName(phase) == name) {
      return phase;
    }
  }
  return std::nullopt;
}

void Start(const SessionOptions& options) {
  State& state = Get();
  // A restart forgets the previous session, but keeps the capture and the
  // stop-after request: they belong to the command line, not to the session.
  const std::shared_ptr<ICaptureSink> capture = state.capture;
  const std::optional<Phase> stop_after = state.stop_after;
  const bool stop_requested = state.stop_requested.load();
  state.report = Report{};
  state.log = ReadLog{};
  state.phases = {};
  state.stack.clear();
  state.last_read_end = 0;
  state.disk_end.reset();
  {
    const std::scoped_lock lock{state.thread_mutex};
    state.threads.clear();
  }
  state.options = options;
  state.capture = capture;
  state.stop_after = stop_after;
  state.stop_requested = stop_requested;
  state.start = Clock::now();
  state.start_unix_ns = static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(
          std::chrono::system_clock::now().time_since_epoch())
          .count());
  state.report.start_unix_ns = state.start_unix_ns;
  state.log.start_unix_ns = state.start_unix_ns;
  std::tie(state.main_user_begin, state.main_kernel_begin) = ThreadTimes();
  if (options.keep_log) {
    state.log.events.reserve(
        (std::min)(options.max_events, std::size_t{1} << 16U));
  }
  state.started = true;
  RefreshFlags(state);
}

bool Enabled() noexcept { return Detail::SessionOn(); }

Report Stop() {
  State& state = Get();
  if (!state.started) {
    return Report{};
  }
  Report report = std::move(state.report);
  const std::uint64_t wall = Detail::NowNs();
  report.wall_ns = wall;
  report.last_read_end_ns = state.last_read_end;
  report.tail_ns = wall > state.last_read_end ? wall - state.last_read_end : 0;
  report.events_logged = state.log.events.size();
  state.log.volume_bytes = report.volume_bytes;

  for (std::size_t index = 0; index < phase_count; index++) {
    PhaseStat stat = state.phases.at(index);
    if (stat.count != 0) {
      stat.phase = static_cast<Phase>(index);
      report.phases.push_back(stat);
    }
  }

  const auto [user, kernel] = ProcessTimes();
  report.process_user_ns = user;
  report.process_kernel_ns = kernel;
  const auto [thread_user, thread_kernel] = ThreadTimes();
  report.threads.push_back(
      {.role = ThreadRole::Main,
       .user_ns = thread_user - state.main_user_begin,
       .kernel_ns = thread_kernel - state.main_kernel_begin,
       .wall_ns = wall});
  {
    const std::scoped_lock lock{state.thread_mutex};
    report.threads.insert(report.threads.end(), state.threads.begin(),
                          state.threads.end());
  }

  for (std::size_t index = 0; index < read_kind_count; index++) {
    report.kinds.at(index).kind = static_cast<ReadKind>(index);
  }

  state.started = false;
  state.report = Report{};
  RefreshFlags(state);
  return report;
}

ReadLog TakeReadLog() {
  State& state = Get();
  ReadLog log = std::move(state.log);
  state.log = ReadLog{};
  return log;
}

void SetStopAfter(std::optional<Phase> phase) {
  State& state = Get();
  state.stop_after = phase;
  state.stop_requested = false;
  RefreshFlags(state);
}

bool StopRequested() noexcept {
  return Get().stop_requested.load(std::memory_order_relaxed);
}

void SetCaptureSink(std::shared_ptr<ICaptureSink> sink) {
  State& state = Get();
  state.capture = std::move(sink);
  RefreshFlags(state);
}

PhaseScope::PhaseScope(Phase phase) noexcept
    : phase_(phase),
      active_(Detail::g_flags.load(std::memory_order_relaxed) != 0) {
  if (!active_ || !Detail::SessionOn()) {
    return;
  }
  State& state = Get();
  begin_ns_ = Detail::NowNs();
  cpu_begin_ns_ = Detail::ThreadCpuNs();
  state.stack.push_back(phase);
  Push(state, {.start_ns = begin_ns_,
               .end_ns = begin_ns_,
               .length = static_cast<std::uint64_t>(phase),
               .type = EventType::PhaseBegin,
               .phase = static_cast<std::uint8_t>(phase)});
}

PhaseScope::~PhaseScope() {
  if (!active_) {
    return;
  }
  State& state = Get();
  if (Detail::SessionOn()) {
    const std::uint64_t end = Detail::NowNs();
    PhaseStat& stat = state.phases.at(static_cast<std::size_t>(phase_));
    if (stat.count == 0) {
      stat.first_begin_ns = begin_ns_;
    }
    stat.count++;
    stat.wall_ns += end - begin_ns_;
    stat.cpu_ns += Detail::ThreadCpuNs() - cpu_begin_ns_;
    stat.last_end_ns = end;
    if (!state.stack.empty()) {
      state.stack.pop_back();
    }
    Push(state, {.start_ns = end,
                 .end_ns = end,
                 .length = static_cast<std::uint64_t>(phase_),
                 .type = EventType::PhaseEnd,
                 .phase = static_cast<std::uint8_t>(phase_)});
  }
  if (state.stop_after && *state.stop_after == phase_) {
    state.stop_requested = true;
  }
}

ThreadScope::ThreadScope(ThreadRole role) noexcept
    : role_(role), active_(Detail::SessionOn()) {
  if (active_) {
    begin_ns_ = Detail::NowNs();
  }
}

ThreadScope::~ThreadScope() {
  if (!active_) {
    return;
  }
  try {
    State& state = Get();
    const auto [user, kernel] = ThreadTimes();
    const std::uint64_t end = Detail::NowNs();
    const std::scoped_lock lock{state.thread_mutex};
    state.threads.push_back({.role = role_,
                             .user_ns = user,
                             .kernel_ns = kernel,
                             .wall_ns = end - begin_ns_});
  } catch (...) {
    // A destructor cannot report; the thread is left out of the report.
  }
}

}  // namespace NTFS_BROWSER_NAMESPACE::Bench
