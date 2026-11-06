// The subcommands that talk to the operating system: purge, wake, sample,
// disk-id, and the table of knobs.

#include <algorithm>
#include <chrono>
#include <compare>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <vector>

#include <sys/types.h>

#include <ntfs-browser/namespace.h>
#include <ntfs-browser/tuning/knobs.h>
#include <ntfs-browser/tuning/tuning.h>

#include "bench-tool.h"
#include "device-path.h"
#include "disk-identity.h"
#include "json.h"
#include "text-convert.h"

#ifdef _WIN32
  #include <windows.h>
  #include <winioctl.h>
#else
  #include <csignal>
  #include <sstream>

  #include <fcntl.h>
  #include <unistd.h>
#endif

namespace Tuning = NTFS_BROWSER_NAMESPACE::Tuning;

namespace NtfsBenchTool {

namespace {

// Version of the layout of the JSON these commands print.
constexpr std::uint64_t json_format = 1;

// Default time between two samples of the disk counters. 100 ms resolves a
// phase of a second and costs nothing on the disk.
constexpr std::uint64_t default_interval_ms = 100;

// Bytes read by wake: one sector of the largest size.
constexpr std::size_t wake_bytes = 4096;

// Where wake reads, as a share of the disk, in thousandths: past the metadata
// both volumes read, below their size.
constexpr std::uint64_t wake_position_per_mille = 850;

#ifdef _WIN32

// Nanoseconds from the Unix epoch to now, from the system clock.
std::uint64_t UnixNowNs() {
  FILETIME now{};
  GetSystemTimePreciseAsFileTime(&now);
  constexpr std::uint64_t ticks_to_unix_epoch = 116'444'736'000'000'000ULL;
  const std::uint64_t ticks =
      (static_cast<std::uint64_t>(now.dwHighDateTime) << 32U) |
      now.dwLowDateTime;
  return (ticks - ticks_to_unix_epoch) * 100;
}

bool EnablePrivilege(const wchar_t* name) {
  HANDLE token = nullptr;
  if (OpenProcessToken(GetCurrentProcess(),
                       TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY,
                       &token) == FALSE) {
    return false;
  }
  TOKEN_PRIVILEGES privileges{};
  privileges.PrivilegeCount = 1;
  privileges.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
  bool ok = LookupPrivilegeValueW(nullptr, name,
                                  &privileges.Privileges[0].Luid) != FALSE &&
            AdjustTokenPrivileges(token, FALSE, &privileges, 0, nullptr,
                                  nullptr) != FALSE &&
            GetLastError() == ERROR_SUCCESS;
  CloseHandle(token);
  return ok;
}

// The \\.\X: path of a drive target, the target itself when it names a device,
// or empty.
std::wstring VolumePath(std::wstring_view target) {
  if (NtfsDevice::ClassifyTarget(target) == NtfsDevice::TargetKind::Device) {
    return std::wstring(target);
  }
  if (const std::optional<wchar_t> letter = NtfsDevice::DriveLetterOf(target)) {
    return std::wstring(L"\\\\.\\") + *letter + L":";
  }
  return {};
}

#endif

// Says why target is no disk for command, and returns the exit code to stop
// with: 2 for a file, which names no disk, and 1 for a path that does not
// exist. Returns 0 for a drive or a device. A path is never taken for the drive
// its text starts with.
int RefuseNonDisk(std::string_view command, std::wstring_view target) {
  const std::string text = NtfsDevice::WideToUtf8(target);
  switch (NtfsDevice::ClassifyTarget(target)) {
    case NtfsDevice::TargetKind::Drive:
    case NtfsDevice::TargetKind::Device:
      return 0;
    case NtfsDevice::TargetKind::File:
      std::fprintf(stderr,
                   "%.*s needs a drive or a device: %s is a file, which "
                   "names no disk\n",
                   static_cast<int>(command.size()), command.data(),
                   text.c_str());
      return 2;
    case NtfsDevice::TargetKind::Missing:
      break;
  }
  std::fprintf(stderr, "Cannot open %s: no such file or device\n",
               text.c_str());
  return 1;
}

}  // namespace

bool PurgeSystemCache(std::string& message) {
#ifdef _WIN32
  using NtSetSystemInformation = LONG(NTAPI*)(INT, PVOID, ULONG);
  // SystemMemoryListInformation, and its commands: flush the modified list
  // to disk, then purge the standby list.
  constexpr INT system_memory_list_information = 80;
  constexpr INT flush_modified_list = 3;
  constexpr INT purge_standby_list = 4;

  if (!EnablePrivilege(L"SeProfileSingleProcessPrivilege") ||
      !EnablePrivilege(L"SeIncreaseQuotaPrivilege")) {
    message = "Cannot purge the file cache: run as administrator";
    return false;
  }
  const HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
  const auto set_information = reinterpret_cast<NtSetSystemInformation>(
      ntdll != nullptr ? GetProcAddress(ntdll, "NtSetSystemInformation")
                       : nullptr);
  if (set_information == nullptr) {
    message = "Cannot purge the file cache: NtSetSystemInformation is missing";
    return false;
  }
  for (INT command : {flush_modified_list, purge_standby_list}) {
    if (set_information(system_memory_list_information, &command,
                        sizeof(command)) < 0) {
      message = "Cannot purge the file cache: the system refused";
      return false;
    }
  }
  return true;
#else
  sync();
  std::ofstream out("/proc/sys/vm/drop_caches");
  out << "3\n";
  out.flush();
  if (!out) {
    message = "Cannot drop the page cache: run as root";
    return false;
  }
  return true;
#endif
}

int PurgeCommand(const Arguments& /*args*/) {
  std::string message;
  if (!PurgeSystemCache(message)) {
    std::fprintf(stderr, "%s\n", message.c_str());
    return 1;
  }
  return 0;
}

int WakeCommand(const Arguments& args) {
  if (args.words.size() != 1) {
    std::fprintf(stderr, "wake expects <target>\n");
    return 2;
  }
  const std::wstring& target = args.words.front();
  if (const int refused = RefuseNonDisk("wake", target); refused != 0) {
    return refused;
  }
#ifdef _WIN32
  const std::wstring path = VolumePath(target);
  // Unbuffered, so that the read reaches the disk and loads nothing the
  // benchmark reads into the cache.
  const HANDLE handle = CreateFileW(
      path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
      OPEN_EXISTING, FILE_FLAG_NO_BUFFERING, nullptr);
  if (handle == INVALID_HANDLE_VALUE) {
    std::fprintf(stderr, "Cannot open %s\n",
                 NtfsDevice::WideToUtf8(path).c_str());
    return 1;
  }
  GET_LENGTH_INFORMATION length{};
  DWORD returned = 0;
  if (DeviceIoControl(handle, IOCTL_DISK_GET_LENGTH_INFO, nullptr, 0, &length,
                      sizeof(length), &returned, nullptr) == FALSE) {
    CloseHandle(handle);
    return 1;
  }
  void* const buffer = VirtualAlloc(nullptr, wake_bytes,
                                    MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
  const std::uint64_t position =
      static_cast<std::uint64_t>(length.Length.QuadPart) / 1000 *
      wake_position_per_mille / wake_bytes * wake_bytes;
  OVERLAPPED overlapped{};
  overlapped.Offset = static_cast<DWORD>(position);
  overlapped.OffsetHigh = static_cast<DWORD>(position >> 32U);
  DWORD got = 0;
  const bool ok =
      ReadFile(handle, buffer, wake_bytes, &got, &overlapped) != FALSE;
  VirtualFree(buffer, 0, MEM_RELEASE);
  CloseHandle(handle);
  return ok ? 0 : 1;
#else
  const std::string path = NtfsDevice::WideToUtf8(target);
  const int descriptor = open(path.c_str(), O_RDONLY);
  if (descriptor < 0) {
    std::fprintf(stderr, "Cannot open %s\n", path.c_str());
    return 1;
  }
  const off_t size = lseek(descriptor, 0, SEEK_END);
  std::vector<char> buffer(wake_bytes);
  const off_t position =
      size / 1000 * static_cast<off_t>(wake_position_per_mille) /
      static_cast<off_t>(wake_bytes) * static_cast<off_t>(wake_bytes);
  const ssize_t got = pread(descriptor, buffer.data(), buffer.size(), position);
  close(descriptor);
  return got > 0 ? 0 : 1;
#endif
}

int DiskIdCommand(const Arguments& args) {
  if (args.words.size() != 1) {
    std::fprintf(stderr, "disk-id expects <target>\n");
    return 2;
  }
  const std::wstring& target = args.words.front();
  // A file names no disk, and the disk that holds it is not the one meant:
  // the script gives the drive or the device it reads. The answer is "none",
  // with the reason, so that an image still gets the "*" section of a profile.
  const NtfsDevice::TargetKind kind = NtfsDevice::ClassifyTarget(target);
  if (kind == NtfsDevice::TargetKind::Missing) {
    return RefuseNonDisk("disk-id", target);
  }
  std::optional<NtfsDevice::DiskIdentity> identity;
  if (kind != NtfsDevice::TargetKind::File) {
    identity = NtfsDevice::IdentifyDisk(target);
  }
  NtfsDevice::JsonWriter json;
  json.BeginObject();
  json.Field("format", json_format);
  json.Field("found", identity.has_value());
  if (kind == NtfsDevice::TargetKind::File) {
    json.Field("reason", std::string_view("a file names no disk: give the "
                                          "drive or the device that holds it"));
  }
  if (identity) {
    json.Field("key", identity->key);
    json.Field("vendor", identity->vendor);
    json.Field("product", identity->product);
    json.Field("serial", identity->serial);
    json.Key("solid_state");
    if (identity->solid_state) {
      json.Bool(*identity->solid_state);
    } else {
      json.Null();
    }
  }
  json.EndObject();
  return EmitJson(args, json);
}

int KnobsCommand(const Arguments& args) {
  NtfsDevice::JsonWriter json;
  json.BeginObject();
  json.Field("format", json_format);
  json.Key("knobs");
  json.BeginArray();
  for (const Tuning::Knob& knob : Tuning::Knobs()) {
    json.BeginObject();
    json.Field("name", knob.name);
    json.Field("family", Tuning::FamilyName(knob.family));
    json.Key("patterns");
    json.BeginArray();
    for (std::size_t index = 0; index < Tuning::pattern_count; index++) {
      if ((knob.patterns & (1U << index)) != 0) {
        json.String(Tuning::PatternName(static_cast<Tuning::Pattern>(index)));
      }
    }
    json.EndArray();
    json.Field("per_pattern", knob.per_pattern);
    json.Field("min", knob.min_value);
    json.Field("max", knob.max_value);
    json.Field("default", knob.default_value);
    json.Field("memory_unit_bytes",
               static_cast<std::uint64_t>(knob.memory_unit_bytes));
    json.EndObject();
  }
  json.EndArray();
  json.EndObject();
  return EmitJson(args, json);
}

namespace {

// One reading of the disk's counters.
struct DiskSample {
  std::uint64_t t_unix_ns{0};
  std::uint64_t bytes_read{0};
  std::uint64_t read_count{0};
  std::uint64_t read_time_ns{0};
  // Windows: the idle time since boot and the time of the query, in ns of
  // their own clocks. Linux: the time the disk was busy since boot.
  std::uint64_t idle_ns{0};
  std::uint64_t query_ns{0};
  std::uint64_t busy_ns{0};
  std::uint32_t queue_depth{0};
};

#ifdef _WIN32

// Reads the counters of a disk through IOCTL_DISK_PERFORMANCE.
class DiskCounters {
 public:
  explicit DiskCounters(std::wstring_view target) {
    std::wstring path = VolumePath(target);
    if (path.empty()) {
      return;
    }
    // A volume or partition maps to the physical disk that holds it.
    if (!path.starts_with(L"\\\\.\\PhysicalDrive")) {
      const HANDLE volume =
          CreateFileW(path.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE,
                      nullptr, OPEN_EXISTING, 0, nullptr);
      if (volume == INVALID_HANDLE_VALUE) {
        return;
      }
      STORAGE_DEVICE_NUMBER number{};
      DWORD returned = 0;
      const bool ok =
          DeviceIoControl(volume, IOCTL_STORAGE_GET_DEVICE_NUMBER, nullptr, 0,
                          &number, sizeof(number), &returned, nullptr) != FALSE;
      CloseHandle(volume);
      if (!ok) {
        return;
      }
      path = L"\\\\.\\PhysicalDrive" + std::to_wstring(number.DeviceNumber);
    }
    name_ = NtfsDevice::WideToUtf8(path);
    handle_ = CreateFileW(path.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE,
                          nullptr, OPEN_EXISTING, 0, nullptr);
  }

  DiskCounters(const DiskCounters&) = delete;
  DiskCounters& operator=(const DiskCounters&) = delete;
  DiskCounters(DiskCounters&&) = delete;
  DiskCounters& operator=(DiskCounters&&) = delete;

  ~DiskCounters() {
    if (handle_ != INVALID_HANDLE_VALUE) {
      CloseHandle(handle_);
    }
  }

  [[nodiscard]] bool Ok() const { return handle_ != INVALID_HANDLE_VALUE; }

  [[nodiscard]] const std::string& Name() const { return name_; }

  [[nodiscard]] static std::string_view Source() {
    return "ioctl-disk-performance";
  }

  std::optional<DiskSample> Read() const {
    DISK_PERFORMANCE performance{};
    DWORD returned = 0;
    if (DeviceIoControl(handle_, IOCTL_DISK_PERFORMANCE, nullptr, 0,
                        &performance, sizeof(performance), &returned,
                        nullptr) == FALSE) {
      return std::nullopt;
    }
    constexpr std::uint64_t ns_per_tick = 100;
    DiskSample sample;
    sample.t_unix_ns = UnixNowNs();
    sample.bytes_read =
        static_cast<std::uint64_t>(performance.BytesRead.QuadPart);
    sample.read_count = performance.ReadCount;
    sample.read_time_ns =
        static_cast<std::uint64_t>(performance.ReadTime.QuadPart) * ns_per_tick;
    sample.idle_ns =
        static_cast<std::uint64_t>(performance.IdleTime.QuadPart) * ns_per_tick;
    sample.query_ns =
        static_cast<std::uint64_t>(performance.QueryTime.QuadPart) *
        ns_per_tick;
    sample.queue_depth = performance.QueueDepth;
    return sample;
  }

 private:
  HANDLE handle_{INVALID_HANDLE_VALUE};
  std::string name_;
};

#else

// Reads the counters of a disk from /proc/diskstats.
class DiskCounters {
 public:
  explicit DiskCounters(std::wstring_view target) {
    namespace fs = std::filesystem;
    std::error_code error;
    const fs::path device(NtfsDevice::WideToUtf8(target));
    if (!fs::is_block_file(device, error)) {
      return;
    }
    name_ = device.filename().string();
    // A partition counts in its disk's line as well, but its own line is the
    // one that matches the volume.
    ok_ = true;
  }

  [[nodiscard]] bool Ok() const { return ok_; }

  [[nodiscard]] const std::string& Name() const { return name_; }

  [[nodiscard]] static std::string_view Source() { return "proc-diskstats"; }

  std::optional<DiskSample> Read() const {
    constexpr std::uint64_t sector_bytes = 512;
    constexpr std::uint64_t ns_per_ms = 1000000;
    std::ifstream in("/proc/diskstats");
    std::string line;
    while (std::getline(in, line)) {
      std::istringstream fields(line);
      unsigned major = 0;
      unsigned minor = 0;
      std::string name;
      fields >> major >> minor >> name;
      if (name != name_) {
        continue;
      }
      std::uint64_t reads = 0;
      std::uint64_t merged = 0;
      std::uint64_t sectors = 0;
      std::uint64_t read_ms = 0;
      std::uint64_t skip = 0;
      std::uint64_t in_flight = 0;
      std::uint64_t io_ms = 0;
      fields >> reads >> merged >> sectors >> read_ms;
      // Writes completed, merged, sectors written, ms writing.
      fields >> skip >> skip >> skip >> skip >> in_flight >> io_ms;
      DiskSample sample;
      sample.t_unix_ns = static_cast<std::uint64_t>(
          std::chrono::duration_cast<std::chrono::nanoseconds>(
              std::chrono::system_clock::now().time_since_epoch())
              .count());
      sample.bytes_read = sectors * sector_bytes;
      sample.read_count = reads;
      sample.read_time_ns = read_ms * ns_per_ms;
      sample.busy_ns = io_ms * ns_per_ms;
      sample.queue_depth = static_cast<std::uint32_t>(in_flight);
      return sample;
    }
    return std::nullopt;
  }

 private:
  bool ok_{false};
  std::string name_;
};

#endif

// Whether the process pid has ended, waiting up to wait_ms for it. A pid of 0
// never ends.
class ProcessWatch {
 public:
  explicit ProcessWatch(std::uint64_t pid) : pid_(pid) {
#ifdef _WIN32
    if (pid != 0) {
      handle_ = OpenProcess(SYNCHRONIZE, FALSE, static_cast<DWORD>(pid));
      missing_ = handle_ == nullptr;
    }
#endif
  }

  ProcessWatch(const ProcessWatch&) = delete;
  ProcessWatch& operator=(const ProcessWatch&) = delete;
  ProcessWatch(ProcessWatch&&) = delete;
  ProcessWatch& operator=(ProcessWatch&&) = delete;

  ~ProcessWatch() {
#ifdef _WIN32
    if (handle_ != nullptr) {
      CloseHandle(handle_);
    }
#endif
  }

  bool Ended(std::uint64_t wait_ms) const {
    if (pid_ == 0) {
      std::this_thread::sleep_for(std::chrono::milliseconds(wait_ms));
      return false;
    }
#ifdef _WIN32
    if (missing_) {
      return true;
    }
    return WaitForSingleObject(handle_, static_cast<DWORD>(wait_ms)) !=
           WAIT_TIMEOUT;
#else
    std::this_thread::sleep_for(std::chrono::milliseconds(wait_ms));
    return kill(static_cast<pid_t>(pid_), 0) != 0;
#endif
  }

 private:
  std::uint64_t pid_;
#ifdef _WIN32
  HANDLE handle_{nullptr};
  bool missing_{false};
#endif
};

}  // namespace

int SampleCommand(const Arguments& args) {
  if (args.words.size() != 1 || !args.Has("out")) {
    std::fprintf(
        stderr,
        "sample expects <target> --out=<file> (--pid=N | --seconds=N | "
        "--until-file=<path>)\n");
    return 2;
  }
  bool malformed = false;
  const std::uint64_t pid = args.Unsigned("pid", 0, malformed);
  const std::uint64_t seconds = args.Unsigned("seconds", 0, malformed);
  const std::uint64_t interval_ms =
      (std::max<std::uint64_t>)(1,
                                args.Unsigned("interval-ms",
                                              default_interval_ms, malformed));
  // The file whose appearance ends the sampling: the script creates it when
  // the process it watches is over, so that sampling starts before the process.
  const std::optional<std::string> until_file = args.Value("until-file");
  if (malformed || (pid == 0 && seconds == 0 && !until_file)) {
    std::fprintf(stderr, "sample needs --pid, --seconds or --until-file\n");
    return 2;
  }
  if (const int refused = RefuseNonDisk("sample", args.words.front());
      refused != 0) {
    return refused;
  }
  const DiskCounters counters(args.words.front());
  if (!counters.Ok()) {
    std::fprintf(stderr, "Cannot read the counters of the disk behind %s\n",
                 NtfsDevice::WideToUtf8(args.words.front()).c_str());
    return 1;
  }

  std::vector<DiskSample> samples;
  const ProcessWatch watch(pid);
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
  for (;;) {
    if (const std::optional<DiskSample> sample = counters.Read()) {
      samples.push_back(*sample);
    }
    const bool ended = watch.Ended(interval_ms);
    std::error_code ignored;
    const bool stopped =
        until_file &&
        std::filesystem::exists(
            std::filesystem::path(std::u8string(
                reinterpret_cast<const char8_t*>(until_file->data()),
                until_file->size())),
            ignored);
    const bool timed_out = pid == 0 && seconds != 0 &&
                           std::chrono::steady_clock::now() >= deadline;
    if (ended || stopped || timed_out) {
      if (const std::optional<DiskSample> last = counters.Read()) {
        samples.push_back(*last);
      }
      break;
    }
  }

  NtfsDevice::JsonWriter json;
  json.BeginObject();
  json.Field("format", json_format);
  json.Field("source", DiskCounters::Source());
  json.Field("device", counters.Name());
  json.Field("interval_ms", interval_ms);
  json.Key("samples");
  json.BeginArray();
  for (const DiskSample& sample : samples) {
    json.BeginObject();
    json.Field("t_unix_ns", sample.t_unix_ns);
    json.Field("bytes_read", sample.bytes_read);
    json.Field("read_count", sample.read_count);
    json.Field("read_time_ns", sample.read_time_ns);
    json.Field("idle_ns", sample.idle_ns);
    json.Field("query_ns", sample.query_ns);
    json.Field("busy_ns", sample.busy_ns);
    json.Field("queue_depth", static_cast<std::uint64_t>(sample.queue_depth));
    json.EndObject();
  }
  json.EndArray();
  json.EndObject();
  return EmitJson(args, json);
}

}  // namespace NtfsBenchTool
