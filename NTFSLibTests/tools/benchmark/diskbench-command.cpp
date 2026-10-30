// The disk's sheet: what the disk gives, in the zone where the $MFT sits, for
// the patterns the library reads with. The handle is opened as the reader's
// is: buffered, because an unbuffered read of a mounted volume returns what is
// on the disk, which can lag what NTFS caches by hours.

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <ntfs-browser/cache/strategy.h>
#include <ntfs-browser/ntfs-volume.h>

#include "bench-tool.h"
#include "device-path.h"
#include "json.h"
#include "text-convert.h"

#ifdef _WIN32
  #include <windows.h>
  #include <winioctl.h>
#else
  #include <atomic>
  #include <thread>

  #include <fcntl.h>
  #include <unistd.h>

  #include "posix-device-disk-reader.h"
#endif

namespace NtfsBenchTool {

namespace {

using Clock = std::chrono::steady_clock;

// A byte range of the target.
struct Zone {
  std::uint64_t offset{0};
  std::uint64_t length{0};
};

// Version of the layout of the JSON this prints.
constexpr std::uint64_t json_format = 1;

// Seconds each measurement runs at most. Short, so that a sheet of 20 lines
// takes under a minute on a hard disk.
constexpr std::uint64_t default_seconds = 2;

// Share of the zone one measurement reads at most: a measurement moves on to
// fresh bytes, so that a wrap does not read from the file cache.
constexpr std::uint64_t zone_share = 8;

// Bytes of the sequential and random reads.
constexpr std::uint64_t sequential_bytes = std::uint64_t{4} << 20U;
constexpr std::uint64_t small_bytes = std::uint64_t{4} << 10U;
constexpr std::uint64_t medium_bytes = std::uint64_t{64} << 10U;

// Alignment of every read, in offset and length.
constexpr std::uint64_t alignment = 4096;

// Queue depths measured.
constexpr std::array<std::uint32_t, 6> queue_depths{1, 2, 4, 8, 16, 32};

// One line of the sheet.
struct Result {
  std::string pattern;
  std::uint32_t queue_depth{0};
  std::uint64_t reads{0};
  std::uint64_t bytes{0};
  double seconds{0};
  double mean_latency_us{0};
  double max_latency_us{0};
};

// A small, fast, repeatable generator for the random offsets.
class XorShift {
 public:
  explicit XorShift(std::uint64_t seed) : state_(seed != 0 ? seed : 1) {}

  std::uint64_t Next() {
    state_ ^= state_ << 13U;
    state_ ^= state_ >> 7U;
    state_ ^= state_ << 17U;
    return state_;
  }

 private:
  std::uint64_t state_;
};

// Where the next read of a measurement goes.
class Cursor {
 public:
  Cursor(const Zone& zone, std::uint64_t window, std::uint64_t bytes,
         bool random, std::uint64_t seed)
      : zone_(zone), bytes_(bytes), random_(random), random_source_(seed) {
    // Each measurement works in its own window of the zone.
    window_start_ =
        zone.offset + (seed % std::max<std::uint64_t>(
                                  1, (zone.length - window) / alignment + 1)) *
                          alignment;
    window_ = window;
  }

  std::uint64_t Next() {
    if (random_) {
      const std::uint64_t slots =
          (std::max<std::uint64_t>)(1, (window_ - bytes_) / alignment + 1);
      return window_start_ + (random_source_.Next() % slots) * alignment;
    }
    const std::uint64_t at = window_start_ + position_;
    position_ += bytes_;
    if (position_ + bytes_ > window_) {
      position_ = 0;
    }
    return at;
  }

 private:
  Zone zone_;
  std::uint64_t bytes_;
  bool random_;
  XorShift random_source_;
  std::uint64_t window_start_{0};
  std::uint64_t window_{0};
  std::uint64_t position_{0};
};

#ifdef _WIN32

// A volume or device opened for overlapped, buffered reads.
class Device {
 public:
  explicit Device(std::wstring_view target) {
    std::wstring path(target);
    if (const std::optional<wchar_t> letter =
            NtfsDevice::DriveLetterOf(target)) {
      path = std::wstring(L"\\\\.\\") + *letter + L":";
    }
    handle_ =
        CreateFileW(path.c_str(), GENERIC_READ,
                    FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                    FILE_ATTRIBUTE_READONLY | FILE_FLAG_OVERLAPPED |
                        FILE_FLAG_SEQUENTIAL_SCAN,
                    nullptr);
  }

  Device(const Device&) = delete;
  Device& operator=(const Device&) = delete;
  Device(Device&&) = delete;
  Device& operator=(Device&&) = delete;

  ~Device() {
    if (handle_ != INVALID_HANDLE_VALUE) {
      CloseHandle(handle_);
    }
  }

  [[nodiscard]] bool Ok() const { return handle_ != INVALID_HANDLE_VALUE; }

  [[nodiscard]] std::optional<std::uint64_t> Size() const {
    GET_LENGTH_INFORMATION length{};
    DWORD returned = 0;
    if (DeviceIoControl(handle_, IOCTL_DISK_GET_LENGTH_INFO, nullptr, 0,
                        &length, sizeof(length), &returned, nullptr) != FALSE) {
      return static_cast<std::uint64_t>(length.Length.QuadPart);
    }
    LARGE_INTEGER size{};
    if (GetFileSizeEx(handle_, &size) != FALSE) {
      return static_cast<std::uint64_t>(size.QuadPart);
    }
    return std::nullopt;
  }

  // Runs reads of bytes at the offsets the cursor gives, queue_depth in flight
  // at once, for seconds or until max_bytes are read.
  Result Measure(std::string pattern, std::uint32_t queue_depth,
                 std::uint64_t bytes, Cursor cursor, std::uint64_t seconds,
                 std::uint64_t max_bytes) const {
    struct Slot {
      OVERLAPPED overlapped{};
      BYTE* buffer{nullptr};
      bool busy{false};
      Clock::time_point began;
    };

    std::vector<Slot> slots(queue_depth);
    std::vector<HANDLE> events;
    for (Slot& slot : slots) {
      slot.buffer = static_cast<BYTE*>(VirtualAlloc(
          nullptr, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
      slot.overlapped.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
      events.push_back(slot.overlapped.hEvent);
    }

    Result result;
    result.pattern = std::move(pattern);
    result.queue_depth = queue_depth;
    double latency_sum_us = 0;
    const auto start = Clock::now();
    const auto deadline = start + std::chrono::seconds(seconds);
    bool issuing = true;
    std::size_t busy = 0;
    const auto issue = [&](Slot& slot) {
      const std::uint64_t at = cursor.Next();
      slot.overlapped.Offset = static_cast<DWORD>(at);
      slot.overlapped.OffsetHigh = static_cast<DWORD>(at >> 32U);
      ResetEvent(slot.overlapped.hEvent);
      slot.began = Clock::now();
      const BOOL ok = ReadFile(handle_, slot.buffer, static_cast<DWORD>(bytes),
                               nullptr, &slot.overlapped);
      if (ok == FALSE && GetLastError() != ERROR_IO_PENDING) {
        return false;
      }
      slot.busy = true;
      busy++;
      return true;
    };
    for (Slot& slot : slots) {
      issuing = issuing && issue(slot);
    }
    while (busy > 0) {
      const DWORD which = WaitForMultipleObjects(
          static_cast<DWORD>(events.size()), events.data(), FALSE, INFINITE);
      if (which >= events.size()) {
        break;
      }
      Slot& slot = slots.at(which);
      DWORD got = 0;
      const bool ok =
          GetOverlappedResult(handle_, &slot.overlapped, &got, TRUE) != FALSE;
      const double latency_us =
          std::chrono::duration<double, std::micro>(Clock::now() - slot.began)
              .count();
      slot.busy = false;
      busy--;
      if (ok) {
        result.reads++;
        result.bytes += got;
        latency_sum_us += latency_us;
        result.max_latency_us = (std::max)(result.max_latency_us, latency_us);
      } else {
        issuing = false;
      }
      // The event of a finished slot stays signalled until it is reissued.
      ResetEvent(slot.overlapped.hEvent);
      if (issuing && Clock::now() < deadline && result.bytes < max_bytes) {
        issuing = issue(slot);
      }
    }
    result.seconds =
        std::chrono::duration<double>(Clock::now() - start).count();
    result.mean_latency_us =
        result.reads == 0 ? 0
                          : latency_sum_us / static_cast<double>(result.reads);
    for (Slot& slot : slots) {
      CloseHandle(slot.overlapped.hEvent);
      VirtualFree(slot.buffer, 0, MEM_RELEASE);
    }
    return result;
  }

 private:
  HANDLE handle_{INVALID_HANDLE_VALUE};
};

#else

// A device or image opened for pread() from several threads.
class Device {
 public:
  explicit Device(std::wstring_view target) {
    const std::string path = NtfsDevice::WideToUtf8(target);
    descriptor_ = open(path.c_str(), O_RDONLY);
  }

  Device(const Device&) = delete;
  Device& operator=(const Device&) = delete;
  Device(Device&&) = delete;
  Device& operator=(Device&&) = delete;

  ~Device() {
    if (descriptor_ >= 0) {
      close(descriptor_);
    }
  }

  [[nodiscard]] bool Ok() const { return descriptor_ >= 0; }

  [[nodiscard]] std::optional<std::uint64_t> Size() const {
    const off_t end = lseek(descriptor_, 0, SEEK_END);
    return end < 0 ? std::nullopt : std::optional<std::uint64_t>(end);
  }

  Result Measure(std::string pattern, std::uint32_t queue_depth,
                 std::uint64_t bytes, Cursor cursor, std::uint64_t seconds,
                 std::uint64_t max_bytes) const {
    Result result;
    result.pattern = std::move(pattern);
    result.queue_depth = queue_depth;
    std::atomic<std::uint64_t> reads{0};
    std::atomic<std::uint64_t> total{0};
    std::atomic<std::uint64_t> latency_ns{0};
    std::atomic<std::uint64_t> max_ns{0};
    std::mutex cursor_mutex;
    const auto start = Clock::now();
    const auto deadline = start + std::chrono::seconds(seconds);
    std::vector<std::thread> workers;
    for (std::uint32_t worker = 0; worker < queue_depth; worker++) {
      workers.emplace_back([&] {
        std::vector<char> buffer(bytes);
        while (Clock::now() < deadline && total.load() < max_bytes) {
          std::uint64_t at = 0;
          {
            const std::scoped_lock lock{cursor_mutex};
            at = cursor.Next();
          }
          const auto began = Clock::now();
          const ssize_t got = pread(descriptor_, buffer.data(), buffer.size(),
                                    static_cast<off_t>(at));
          const auto took = static_cast<std::uint64_t>(
              std::chrono::duration_cast<std::chrono::nanoseconds>(
                  Clock::now() - began)
                  .count());
          if (got <= 0) {
            return;
          }
          reads++;
          total += static_cast<std::uint64_t>(got);
          latency_ns += took;
          std::uint64_t seen = max_ns.load();
          while (took > seen && !max_ns.compare_exchange_weak(seen, took)) {
          }
        }
      });
    }
    for (std::thread& worker : workers) {
      worker.join();
    }
    result.seconds =
        std::chrono::duration<double>(Clock::now() - start).count();
    result.reads = reads;
    result.bytes = total;
    result.mean_latency_us = reads == 0
                                 ? 0
                                 : static_cast<double>(latency_ns) / 1000.0 /
                                       static_cast<double>(reads.load());
    result.max_latency_us = static_cast<double>(max_ns) / 1000.0;
    return result;
  }

 private:
  int descriptor_{-1};
};

#endif

// The zone of $MFT in target, from the boot sector and the $MFT record.
std::optional<Zone> MftZone(const std::wstring& target) {
  using Volume = NTFS_BROWSER_NAMESPACE::NtfsVolume<
      NTFS_BROWSER_NAMESPACE::Cache::Strategy::NoCache>;
#ifdef _WIN32
  const std::optional<wchar_t> letter = NtfsDevice::DriveLetterOf(target);
  const std::unique_ptr<Volume> volume =
      letter ? std::make_unique<Volume>(*letter)
             : std::make_unique<Volume>(std::wstring_view(target));
#else
  auto reader = std::make_unique<NtfsDevice::PosixDeviceDiskReader>();
  if (!reader->Open(target)) {
    return std::nullopt;
  }
  const std::unique_ptr<Volume> volume =
      std::make_unique<Volume>(std::move(reader));
#endif
  if (!volume->IsVolumeOK()) {
    return std::nullopt;
  }
  return Zone{.offset = volume->GetMFTAddr(),
              .length =
                  volume->GetRecordsCount() * volume->GetFileRecordSize()};
}

// Parses "offset:length".
std::optional<Zone> ParseZone(std::string_view text) {
  const std::size_t colon = text.find(':');
  if (colon == std::string_view::npos) {
    return std::nullopt;
  }
  Zone zone;
  try {
    zone.offset = std::stoull(std::string(text.substr(0, colon)));
    zone.length = std::stoull(std::string(text.substr(colon + 1)));
  } catch (...) {
    return std::nullopt;
  }
  return zone;
}

}  // namespace

int DiskBenchCommand(const Arguments& args) {
  if (args.words.size() != 1) {
    std::fprintf(stderr, "diskbench expects <target>\n");
    return 2;
  }
  const std::wstring& target = args.words.front();
  bool malformed = false;
  const std::uint64_t seconds =
      args.Unsigned("seconds", default_seconds, malformed);
  if (malformed) {
    return 2;
  }
  const Device device(target);
  if (!device.Ok()) {
    std::fprintf(stderr, "Cannot open %s\n",
                 NtfsDevice::WideToUtf8(target).c_str());
    return 1;
  }

  const std::string zone_text = args.Value("zone").value_or("mft");
  std::optional<Zone> zone;
  if (zone_text == "mft") {
    zone = MftZone(target);
    if (!zone) {
      std::fprintf(stderr,
                   "Not an NTFS volume: the zone is the start of the disk\n");
    }
  } else {
    zone = ParseZone(zone_text);
    if (!zone) {
      std::fprintf(stderr, "--zone is mft or offset:length\n");
      return 2;
    }
  }
  const std::optional<std::uint64_t> size = device.Size();
  if (!zone) {
    zone = Zone{.offset = 0, .length = size.value_or(std::uint64_t{1} << 30U)};
  }
  if (size) {
    zone->length =
        std::min(zone->length, *size > zone->offset ? *size - zone->offset : 0);
  }
  if (zone->length < sequential_bytes * 2) {
    std::fprintf(stderr, "The zone is too small to measure\n");
    return 1;
  }
  const std::uint64_t window =
      std::max(zone->length / zone_share, sequential_bytes * 2);

  std::vector<Result> results;
  std::uint64_t seed = 1;
  const auto run = [&](const char* name, std::uint64_t bytes, bool random) {
    for (const std::uint32_t depth : queue_depths) {
      seed++;
      results.push_back(device.Measure(
          name, depth, bytes, Cursor(*zone, window, bytes, random, seed * 7919),
          seconds, window));
      const Result& last = results.back();
      std::fprintf(stderr, "%-8s QD%-2u %9.1f MB/s %9.0f IOPS\n", name, depth,
                   static_cast<double>(last.bytes) / 1048576.0 / last.seconds,
                   static_cast<double>(last.reads) / last.seconds);
    }
  };
  run("seq4m", sequential_bytes, false);
  run("rand4k", small_bytes, true);
  run("rand64k", medium_bytes, true);

  NtfsDevice::JsonWriter json;
  json.BeginObject();
  json.Field("format", json_format);
  json.Field("target", NtfsDevice::WideToUtf8(target));
  json.Key("zone");
  json.BeginObject();
  json.Field("offset", zone->offset);
  json.Field("length", zone->length);
  json.Field("window", window);
  json.EndObject();
  json.Key("results");
  json.BeginArray();
  for (const Result& result : results) {
    json.BeginObject();
    json.Field("pattern", result.pattern);
    json.Field("queue_depth", static_cast<std::uint64_t>(result.queue_depth));
    json.Field("reads", result.reads);
    json.Field("bytes", result.bytes);
    json.Key("seconds");
    json.Number(result.seconds);
    json.Key("mb_per_s");
    json.Number(result.seconds == 0 ? 0.0
                                    : static_cast<double>(result.bytes) /
                                          1048576.0 / result.seconds);
    json.Key("iops");
    json.Number(result.seconds == 0
                    ? 0.0
                    : static_cast<double>(result.reads) / result.seconds);
    json.Key("mean_latency_us");
    json.Number(result.mean_latency_us);
    json.Key("max_latency_us");
    json.Number(result.max_latency_us);
    json.EndObject();
  }
  json.EndArray();
  json.EndObject();
  return EmitJson(args, json);
}

}  // namespace NtfsBenchTool
