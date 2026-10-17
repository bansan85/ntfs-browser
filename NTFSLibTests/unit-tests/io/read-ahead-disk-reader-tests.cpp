#include <ntfs-browser/win-types.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <gsl/narrow>

#include <ntfs-browser/cache/strategy.h>
#include <ntfs-browser/disk-reader.h>
#include <ntfs-browser/io/read-ahead-disk-reader.h>
#include <ntfs-browser/mft/tree.h>
#include <ntfs-browser/namespace.h>
#include <ntfs-browser/ntfs-volume.h>
#include <ntfs-browser/volume-options.h>

#include "fake-ntfs-image.h"
#include "log/ntfs-common.h"
#include "memory-disk-reader.h"
#include "test-log-sink.h"

namespace Cache = NTFS_BROWSER_NAMESPACE::Cache;
namespace Io = NTFS_BROWSER_NAMESPACE::Io;
using NTFS_BROWSER_NAMESPACE::IDiskReader;
using NTFS_BROWSER_NAMESPACE::NtfsVolume;
using NTFS_BROWSER_NAMESPACE::VolumeOptions;
using NTFS_BROWSER_NAMESPACE::Mft::MftTree;
using NTFS_BROWSER_NAMESPACE::Tests::MemoryDiskReader;
using NTFS_BROWSER_NAMESPACE::Tests::TakeCapturedLog;

namespace {

// The chunk size of most tests: one 4096-byte unit, the smallest the decorator
// accepts, so a small buffer holds many chunks.
constexpr std::uint64_t test_chunk = 4096;

// Size of the buffer the decorator reads, a whole number of chunks.
constexpr size_t content_size = 16 * test_chunk;

// The reads that start a stream: two forward reads. The third one is the first
// that read ahead can serve.
constexpr size_t first_read_size = 1000;
constexpr size_t second_read_size = 1000;
constexpr LONGLONG stream_start = first_read_size + second_read_size;

// How long a test waits for the worker thread before it calls that a failure.
// Long on purpose: it is a bound on a bug, never a measure of speed.
constexpr std::chrono::seconds wait_limit{30};

// Content whose every offset differs from its neighbours, so a misplaced copy
// shows.
std::vector<BYTE> MakePattern(size_t size) {
  std::vector<BYTE> content(size);
  for (size_t i = 0; i < size; i++) {
    // i < size by the loop condition.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    content[i] = static_cast<BYTE>((i * 31 + (i >> 8) * 7 + 3) & 0xFF);
  }
  return content;
}

// What a ProbeDiskReader saw, shared with the test so that it survives the
// decorator that owns the reader.
struct ProbeState {
  struct Read {
    LONGLONG addr;
    size_t size;
    std::thread::id thread;
  };

  std::mutex mutex;
  std::condition_variable changed;
  std::vector<BYTE> data;

  // Every read the reader began, in order.
  std::vector<Read> started;
  size_t finished{0};

  // A read at or past this address waits until gate_open. Negative: no gate.
  LONGLONG gate_from{-1};
  bool gate_open{true};

  // Reads on any thread but this one fail. Default id: none do.
  std::thread::id only_this_thread_reads;

  // Reads that overlap [fail_from, fail_to) fail.
  LONGLONG fail_from{0};
  LONGLONG fail_to{0};

  // Reads that the reader began while it was being destroyed, or still
  // running when it was. Both MUST stay zero.
  int active{0};
  bool dead{false};
  int misuse{0};
  bool log_reads{false};
};

// A random-access reader that records its reads and can hold them back, so a
// test sets the interleaving instead of timing it.
class ProbeDiskReader : public IDiskReader {
 public:
  explicit ProbeDiskReader(std::shared_ptr<ProbeState> state)
      : state_(std::move(state)) {}

  ProbeDiskReader(const ProbeDiskReader&) = delete;
  ProbeDiskReader& operator=(const ProbeDiskReader&) = delete;
  ProbeDiskReader(ProbeDiskReader&&) = delete;
  ProbeDiskReader& operator=(ProbeDiskReader&&) = delete;

  ~ProbeDiskReader() override {
    const std::scoped_lock lock{state_->mutex};
    if (state_->active != 0) {
      state_->misuse++;
    }
    state_->dead = true;
  }

  bool Open(std::wstring_view /*path*/) override { return true; }

  [[nodiscard]] bool ReadInto(LARGE_INTEGER& addr,
                              std::span<BYTE> dest) const override {
    ProbeState& state = *state_;
    std::unique_lock lock{state.mutex};
    if (state.dead) {
      state.misuse++;
    }
    state.started.push_back(
        {addr.QuadPart, dest.size(), std::this_thread::get_id()});
    state.active++;
    state.changed.notify_all();
    if (state.gate_from >= 0 && addr.QuadPart >= state.gate_from) {
      state.changed.wait(lock, [&] { return state.gate_open; });
    }

    const bool wrong_thread =
        state.only_this_thread_reads != std::thread::id{} &&
        state.only_this_thread_reads != std::this_thread::get_id();
    const auto start = static_cast<ULONGLONG>(addr.QuadPart);
    const bool out_of_range =
        addr.QuadPart < 0 || start + dest.size() > state.data.size();
    const bool failing =
        addr.QuadPart < state.fail_to &&
        addr.QuadPart + gsl::narrow<LONGLONG>(dest.size()) > state.fail_from;
    const bool ok = !wrong_thread && !out_of_range && !failing;
    if (ok) {
      std::copy_n(state.data.begin() + gsl::narrow<std::ptrdiff_t>(start),
                  dest.size(), dest.begin());
    }

    const bool log_read = state.log_reads;
    state.active--;
    state.finished++;
    state.changed.notify_all();
    lock.unlock();
    if (log_read) {
      NTFS_BROWSER_NAMESPACE::Log::Error("probe read at {}", addr.QuadPart);
    }
    return ok;
  }

 private:
  std::shared_ptr<ProbeState> state_;
};

// A probe over MakePattern(size), plus the decorator wrapped around it.
struct Fixture {
  std::shared_ptr<ProbeState> state{std::make_shared<ProbeState>()};
  std::unique_ptr<Io::ReadAheadDiskReader> reader;
  std::thread::id caller{std::this_thread::get_id()};

  explicit Fixture(size_t size = content_size,
                   std::uint64_t chunk_bytes = test_chunk) {
    state->data = MakePattern(size);
    reader = std::make_unique<Io::ReadAheadDiskReader>(
        std::make_unique<ProbeDiskReader>(state),
        Io::ReadAheadOptions{.chunk_bytes = chunk_bytes});
  }

  // Reads size bytes at addr through the decorator and checks them against
  // the content. Returns the decorator's answer.
  bool Read(LONGLONG addr, size_t size) const {
    std::vector<BYTE> got(size, 0xEE);
    LARGE_INTEGER at{.QuadPart = addr};
    const bool ok = reader->ReadInto(at, got);
    if (ok) {
      const std::vector<BYTE>& data = state->data;
      REQUIRE(static_cast<size_t>(addr) + size <= data.size());
      CHECK(std::equal(got.begin(), got.end(),
                       data.begin() + gsl::narrow<std::ptrdiff_t>(addr)));
    }
    return ok;
  }

  // Blocks until pred holds on the probe state.
  template <class Pred>
  void WaitFor(Pred pred) const {
    std::unique_lock lock{state->mutex};
    REQUIRE(state->changed.wait_for(lock, wait_limit,
                                    [&] { return pred(*state); }));
  }

  // Blocks until the probe has begun a read at addr, on any thread.
  void WaitForReadAt(LONGLONG addr) const {
    WaitFor([&](const ProbeState& s) {
      return std::any_of(
          s.started.begin(), s.started.end(),
          [&](const ProbeState::Read& r) { return r.addr == addr; });
    });
  }

  void OpenGate() const {
    const std::scoped_lock lock{state->mutex};
    state->gate_open = true;
    state->changed.notify_all();
  }

  void CloseGateFrom(LONGLONG addr) const {
    const std::scoped_lock lock{state->mutex};
    state->gate_from = addr;
    state->gate_open = false;
  }

  // The reads the probe began so far, as a copy.
  std::vector<ProbeState::Read> Started() const {
    const std::scoped_lock lock{state->mutex};
    return state->started;
  }

  // How many reads began on the caller's thread.
  size_t CallerReads() const {
    const std::vector<ProbeState::Read> started = Started();
    return gsl::narrow<size_t>(std::count_if(
        started.begin(), started.end(),
        [&](const ProbeState::Read& r) { return r.thread == caller; }));
  }

  // Two forward reads, which make the decorator read ahead.
  void StartStream() const {
    REQUIRE(Read(0, first_read_size));
    REQUIRE(Read(first_read_size, second_read_size));
  }
};

}  // namespace

TEST_CASE("ReadAheadDiskReader returns the bytes of the wrapped reader",
          "[io][read-ahead]") {
  Fixture fixture;

  SECTION("forward reads of uneven sizes, to the very end") {
    constexpr std::array<size_t, 9> sizes{1000, 3000, 4096, 777, 10000,
                                          512,  8192, 5000, 4096};
    LONGLONG addr = 0;
    size_t total = 0;
    for (const size_t size : sizes) {
      if (static_cast<size_t>(addr) + size > content_size) {
        break;
      }
      REQUIRE(fixture.Read(addr, size));
      addr += gsl::narrow<LONGLONG>(size);
      total += size;
    }
    REQUIRE(total > 4 * test_chunk);
    const size_t rest = content_size - static_cast<size_t>(addr);
    REQUIRE(fixture.Read(addr, rest));
  }

  SECTION("one read as big as the whole buffer") {
    REQUIRE(fixture.Read(0, content_size));
  }

  SECTION("single bytes in a row") {
    for (LONGLONG addr = 0; addr < 3 * gsl::narrow<LONGLONG>(test_chunk);
         addr++) {
      REQUIRE(fixture.Read(addr, 1));
    }
  }

  SECTION("an empty read") { REQUIRE(fixture.Read(0, 0)); }
}

TEST_CASE("ReadAheadDiskReader reads ahead after two forward reads",
          "[io][read-ahead]") {
  Fixture fixture;
  fixture.StartStream();

  // Two chunks, both read by the worker, right behind the second read.
  fixture.WaitForReadAt(stream_start + gsl::narrow<LONGLONG>(test_chunk));
  const std::vector<ProbeState::Read> started = fixture.Started();
  REQUIRE(started.size() == 4);
  CHECK(started[0].thread == fixture.caller);
  CHECK(started[1].thread == fixture.caller);
  CHECK(started[2].addr == stream_start);
  CHECK(started[2].size == test_chunk);
  CHECK(started[2].thread != fixture.caller);
  CHECK(started[3].addr == stream_start + gsl::narrow<LONGLONG>(test_chunk));
  CHECK(started[3].size == test_chunk);
  CHECK(started[3].thread == started[2].thread);

  SECTION("a read inside the first chunk never reaches the wrapped reader") {
    REQUIRE(fixture.Read(stream_start, 500));
    REQUIRE(fixture.Read(stream_start + 500, 1500));
    CHECK(fixture.CallerReads() == 2);
  }

  SECTION("a read that spans two chunks is served from both") {
    REQUIRE(fixture.Read(stream_start, 2 * test_chunk));
    CHECK(fixture.CallerReads() == 2);
  }

  SECTION("a read that outruns the chunks is completed directly") {
    // Starts inside the second chunk and ends past it: the tail is read by
    // the caller.
    const LONGLONG addr = stream_start + 5000;
    REQUIRE(fixture.Read(addr, 5000));
    CHECK(fixture.CallerReads() == 3);
  }

  SECTION("the window moves on as the caller consumes it") {
    // The first chunk is behind the caller: a third one is queued after the
    // second.
    REQUIRE(fixture.Read(stream_start, test_chunk));
    fixture.WaitForReadAt(stream_start + 2 * gsl::narrow<LONGLONG>(test_chunk));
    REQUIRE(fixture.Read(stream_start + test_chunk, test_chunk));
    REQUIRE(fixture.Read(stream_start + 2 * test_chunk, test_chunk));
    CHECK(fixture.CallerReads() == 2);
  }
}

TEST_CASE("ReadAheadDiskReader does not read ahead for scattered reads",
          "[io][read-ahead]") {
  Fixture fixture;

  REQUIRE(fixture.Read(0, 100));
  REQUIRE(fixture.Read(20000, 100));
  REQUIRE(fixture.Read(8000, 100));
  REQUIRE(fixture.Read(30000, 100));
  REQUIRE(fixture.Read(30001, 99));

  // Joining the worker, if there is one, makes the list final.
  fixture.reader.reset();
  const std::vector<ProbeState::Read> started = fixture.Started();
  REQUIRE(started.size() == 5);
  for (const ProbeState::Read& read : started) {
    CHECK(read.thread == fixture.caller);
  }
  CHECK(started[0].size == 100);
  CHECK(started[1].addr == 20000);
}

TEST_CASE("ReadAheadDiskReader overlaps the next read with the caller's work",
          "[io][read-ahead]") {
  Fixture fixture;
  const LONGLONG second_chunk =
      stream_start + gsl::narrow<LONGLONG>(test_chunk);

  // The second chunk's read cannot finish until the test says so.
  fixture.CloseGateFrom(second_chunk);
  fixture.StartStream();
  fixture.WaitForReadAt(second_chunk);

  // The worker is inside the wrapped reader, on chunk two. The caller still
  // gets chunk one, instead of waiting for the reader to be free.
  {
    const std::scoped_lock lock{fixture.state->mutex};
    REQUIRE(fixture.state->finished == 3);
  }
  REQUIRE(fixture.Read(stream_start, 3000));
  REQUIRE(fixture.Read(stream_start + 3000, 1000));
  {
    const std::scoped_lock lock{fixture.state->mutex};
    CHECK(fixture.state->finished == 3);
    CHECK(fixture.state->active == 1);
  }

  fixture.OpenGate();
  REQUIRE(fixture.Read(second_chunk, test_chunk));
  CHECK(fixture.CallerReads() == 2);
}

TEST_CASE("ReadAheadDiskReader waits for a chunk that is still being read",
          "[io][read-ahead]") {
  Fixture fixture;

  fixture.CloseGateFrom(stream_start);
  fixture.StartStream();
  fixture.WaitForReadAt(stream_start);

  // Opens the gate once the worker is inside the first chunk, whether or not
  // the caller already waits for it: the answer is the same.
  std::thread releaser([&] { fixture.OpenGate(); });
  const bool ok = fixture.Read(stream_start, 2000);
  releaser.join();

  REQUIRE(ok);
  CHECK(fixture.CallerReads() == 2);
}

TEST_CASE("ReadAheadDiskReader drops the read ahead when the caller jumps",
          "[io][read-ahead]") {
  Fixture fixture;
  fixture.StartStream();
  fixture.WaitForReadAt(stream_start + gsl::narrow<LONGLONG>(test_chunk));

  SECTION("forward") {
    REQUIRE(fixture.Read(40000, 100));
    const std::vector<ProbeState::Read> started = fixture.Started();
    CHECK(started.back().thread == fixture.caller);
    CHECK(started.back().addr == 40000);
    CHECK(started.back().size == 100);
  }

  SECTION("backward") {
    REQUIRE(fixture.Read(0, 100));
    const std::vector<ProbeState::Read> started = fixture.Started();
    CHECK(started.back().thread == fixture.caller);
    CHECK(started.back().addr == 0);
    CHECK(started.back().size == 100);
  }

  SECTION("the stream starts again at the new place") {
    REQUIRE(fixture.Read(40000, 100));
    REQUIRE(fixture.Read(40100, 100));
    fixture.WaitForReadAt(40200);
    REQUIRE(fixture.Read(40200, 300));
    CHECK(fixture.CallerReads() == 4);
  }
}

TEST_CASE("ReadAheadDiskReader reports a failing read", "[io][read-ahead]") {
  Fixture fixture;

  SECTION("a scattered read") {
    {
      const std::scoped_lock lock{fixture.state->mutex};
      fixture.state->fail_from = 30000;
      fixture.state->fail_to = 31000;
    }
    CHECK_FALSE(fixture.Read(30500, 100));
    CHECK(fixture.Read(0, 100));
    CHECK_FALSE(fixture.Read(29950, 100));
    CHECK(fixture.Read(31000, 100));
  }

  SECTION("a forward read that reaches the bad place") {
    {
      const std::scoped_lock lock{fixture.state->mutex};
      fixture.state->fail_from = 20000;
      fixture.state->fail_to = 21000;
    }
    LONGLONG addr = 0;
    while (addr + 1000 <= 20000) {
      REQUIRE(fixture.Read(addr, 1000));
      addr += 1000;
    }
    CHECK_FALSE(fixture.Read(addr, 1000));
    // The failure is not stuck: once past it, reads work again.
    CHECK(fixture.Read(21000, 1000));
    CHECK(fixture.Read(22000, 1000));
    CHECK(fixture.Read(23000, 1000));
  }

  SECTION("a read past the end") {
    CHECK_FALSE(fixture.Read(content_size - 10, 20));
    CHECK(fixture.Read(content_size - 20, 20));
  }

  SECTION("a negative address") { CHECK_FALSE(fixture.Read(-1, 10)); }
}

TEST_CASE("ReadAheadDiskReader hides a read ahead that fails",
          "[io][read-ahead]") {
  Fixture fixture;

  // Only the caller's thread can read: every chunk the worker reads fails.
  {
    const std::scoped_lock lock{fixture.state->mutex};
    fixture.state->only_this_thread_reads = fixture.caller;
  }

  LONGLONG addr = 0;
  for (int i = 0; i < 40; i++) {
    REQUIRE(fixture.Read(addr, 1500));
    addr += 1500;
  }
  fixture.reader.reset();

  // The bad chunks did not stick: the caller read everything itself.
  CHECK(fixture.CallerReads() >= 40);
}

TEST_CASE("ReadAheadDiskReader copes with the end of the medium",
          "[io][read-ahead]") {
  // Not a whole number of chunks: the last chunk reads past the end and
  // fails whole, but the caller's own reads still find the bytes.
  constexpr size_t odd_size = 10 * test_chunk + 1234;
  Fixture fixture(odd_size);

  LONGLONG addr = 0;
  while (static_cast<size_t>(addr) + 700 <= odd_size) {
    REQUIRE(fixture.Read(addr, 700));
    addr += 700;
  }
  const size_t rest = odd_size - static_cast<size_t>(addr);
  REQUIRE(fixture.Read(addr, rest));
  CHECK_FALSE(fixture.Read(gsl::narrow<LONGLONG>(odd_size), 1));
  CHECK_FALSE(fixture.Read(gsl::narrow<LONGLONG>(odd_size) - 1, 2));
}

TEST_CASE("ReadAheadDiskReader sizes its chunks", "[io][read-ahead]") {
  SECTION("a small size is rounded up to 4096") {
    Fixture fixture(content_size, 1);
    fixture.StartStream();
    fixture.WaitForReadAt(stream_start + gsl::narrow<LONGLONG>(test_chunk));
    CHECK(fixture.Started()[2].size == test_chunk);
  }

  SECTION("a size is rounded up to a multiple of 4096") {
    Fixture fixture(content_size, 5000);
    fixture.StartStream();
    fixture.WaitForReadAt(stream_start + 2 * gsl::narrow<LONGLONG>(test_chunk));
    CHECK(fixture.Started()[2].size == 2 * test_chunk);
  }

  SECTION("a big read makes a chunk as big as the read") {
    Fixture fixture(64 * test_chunk, test_chunk);
    constexpr size_t big = 5 * test_chunk;
    REQUIRE(fixture.Read(0, big));
    REQUIRE(fixture.Read(big, big));
    fixture.WaitForReadAt(2 * gsl::narrow<LONGLONG>(big));
    CHECK(fixture.Started()[2].size == big);
    REQUIRE(fixture.Read(2 * big, big));
    REQUIRE(fixture.Read(3 * big, big));
    CHECK(fixture.CallerReads() == 2);
  }
}

TEST_CASE("ReadAheadDiskReader joins its worker when destroyed",
          "[io][read-ahead]") {
  SECTION("with no read ahead started") {
    Fixture fixture;
    REQUIRE(fixture.Read(0, 100));
    fixture.reader.reset();
    CHECK(fixture.state->dead);
    CHECK(fixture.state->misuse == 0);
  }

  SECTION("right after construction") {
    Fixture fixture;
    fixture.reader.reset();
    CHECK(fixture.state->dead);
    CHECK(fixture.Started().empty());
  }

  SECTION("while a chunk is being read") {
    Fixture fixture;
    fixture.CloseGateFrom(stream_start);
    fixture.StartStream();
    fixture.WaitForReadAt(stream_start);

    // The destructor has to wait for the read in flight: it is released only
    // once the destruction is under way.
    std::atomic<bool> destroying{false};
    std::thread releaser([&] {
      while (!destroying.load()) {
        std::this_thread::yield();
      }
      fixture.OpenGate();
    });
    destroying = true;
    fixture.reader.reset();
    releaser.join();

    const std::scoped_lock lock{fixture.state->mutex};
    CHECK(fixture.state->dead);
    CHECK(fixture.state->active == 0);
    CHECK(fixture.state->misuse == 0);
  }
}

TEST_CASE("ReadAheadDiskReader stays quiet on its worker thread",
          "[io][read-ahead]") {
  Fixture fixture;
  {
    const std::scoped_lock lock{fixture.state->mutex};
    fixture.state->log_reads = true;
  }
  static_cast<void>(TakeCapturedLog());

  fixture.StartStream();
  fixture.WaitForReadAt(stream_start + gsl::narrow<LONGLONG>(test_chunk));
  fixture.WaitFor([](const ProbeState& s) { return s.active == 0; });
  fixture.reader.reset();

  // Only the two reads of the caller's thread logged.
  const std::string log = TakeCapturedLog();
  size_t lines = 0;
  for (size_t pos = log.find("probe read"); pos != std::string::npos;
       pos = log.find("probe read", pos + 1)) {
    lines++;
  }
  CHECK(lines == 2);
}

TEST_CASE("ReadAheadDiskReader forwards Open", "[io][read-ahead]") {
  Fixture fixture;
  fixture.StartStream();
  CHECK(fixture.reader->Open(L"ignored"));
  // The stream is forgotten: a read that would have hit is a direct one.
  const size_t before = fixture.CallerReads();
  REQUIRE(fixture.Read(stream_start, 100));
  CHECK(fixture.CallerReads() == before + 1);
}

TEST_CASE("ReadAheadDiskReader without a wrapped reader fails every read",
          "[io][read-ahead]") {
  Io::ReadAheadDiskReader reader(nullptr);
  std::vector<BYTE> buffer(16);
  LARGE_INTEGER at{.QuadPart = 0};
  CHECK_FALSE(reader.Open(L"x"));
  CHECK_FALSE(reader.ReadInto(at, buffer));
}

TEST_CASE("ReadAheadDiskReader works over MemoryDiskReader",
          "[io][read-ahead]") {
  const std::vector<BYTE> content = MakePattern(50000);
  Io::ReadAheadDiskReader reader(std::make_unique<MemoryDiskReader>(content),
                                 {.chunk_bytes = test_chunk});

  std::vector<BYTE> got;
  std::vector<BYTE> piece(777);
  LARGE_INTEGER at{.QuadPart = 0};
  while (static_cast<size_t>(at.QuadPart) + piece.size() <= content.size()) {
    REQUIRE(reader.ReadInto(at, piece));
    got.insert(got.end(), piece.begin(), piece.end());
    at.QuadPart += gsl::narrow<LONGLONG>(piece.size());
  }

  REQUIRE(got.size() > 49000);
  CHECK(std::equal(got.begin(), got.end(), content.begin()));
}

TEST_CASE("NtfsVolume with read_ahead_bytes lists what it lists without",
          "[io][read-ahead][mft-tree]") {
  // One line per entry: enough to see a wrong byte anywhere in a record.
  const auto describe = [](const MftTree& tree) {
    std::vector<std::wstring> lines;
    for (const MftTree::Entry& entry : tree.Entries()) {
      lines.push_back(std::to_wstring(entry.record) + L"|" +
                      tree.GetPath(entry.record) + L"|" +
                      std::to_wstring(entry.size) + L"|" +
                      std::to_wstring(entry.allocated_size));
    }
    return lines;
  };

  const auto list = [&]<Cache::Strategy S>(std::uint64_t read_ahead_bytes) {
    NtfsVolume<S> volume(
        std::make_unique<MemoryDiskReader>(
            NTFS_BROWSER_NAMESPACE::Tests::BuildFakeNtfsImageWithMftTree()),
        VolumeOptions{.include_deleted = true,
                      .read_ahead_bytes = read_ahead_bytes});
    REQUIRE(volume.IsVolumeOK());
    CHECK(volume.GetOptions().read_ahead_bytes == read_ahead_bytes);
    return describe(MftTree(volume));
  };

  const std::vector<std::wstring> expected =
      list.template operator()<Cache::Strategy::NoCache>(0);
  REQUIRE(expected.size() > 1);

  CHECK(list.template operator()<Cache::Strategy::NoCache>(test_chunk) ==
        expected);
  CHECK(list.template operator()<Cache::Strategy::FullCache>(test_chunk) ==
        expected);
  CHECK(list.template operator()<Cache::Strategy::NoCache>(
            Io::default_read_ahead_bytes) == expected);
}
