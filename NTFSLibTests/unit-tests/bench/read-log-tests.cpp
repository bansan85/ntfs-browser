#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <optional>
#include <string>
#include <tuple>
#include <vector>

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <ntfs-browser/namespace.h>
#include <ntfs-browser/test/device-access/temp-dir.h>
#include <ntfs-browser/test/disk-readers/memory-disk-reader.h>
#include <ntfs-browser/test/fake-image/fake-ntfs-image.h>
#include <ntfs-browser/test/support/bench-session-guard.h>

#include "bench/bench.h"
#include "cache/strategy.h"
#include "disk-reader.h"
#include "mft/tree.h"
#include "ntfs-volume.h"
#include "volume-options.h"
#include "win-types.h"

namespace Bench = NTFS_BROWSER_NAMESPACE::Bench;
namespace Cache = NTFS_BROWSER_NAMESPACE::Cache;
namespace Mft = NTFS_BROWSER_NAMESPACE::Mft;
namespace TestDeviceAccess = NTFS_BROWSER_NAMESPACE::Test::DeviceAccess;
namespace TestSupport = NTFS_BROWSER_NAMESPACE::Test::Support;
namespace TestDiskReaders = NTFS_BROWSER_NAMESPACE::Test::DiskReaders;
namespace TestFakeImage = NTFS_BROWSER_NAMESPACE::Test::FakeImage;
using Catch::Matchers::ContainsSubstring;
using NTFS_BROWSER_NAMESPACE::NtfsVolume;
using NTFS_BROWSER_NAMESPACE::VolumeOptions;

namespace {

// Bytes of the header of a read log file, and of one event.
constexpr size_t header_bytes = 64;
constexpr size_t event_bytes = 48;

// Where the fields sit in the header: magic, version, event size, count.
constexpr size_t version_offset = 8;
constexpr size_t event_size_offset = 12;
constexpr size_t count_offset = 16;

Bench::LogEvent MakeEvent(Bench::EventType type, std::uint64_t seed) {
  Bench::LogEvent event;
  event.start_ns = seed * 1000 + 1;
  event.end_ns = seed * 1000 + 17;
  event.wait_ns = seed * 3;
  event.offset = (seed << 20U) + 4096;
  event.length = (seed * 512) + 1;
  event.type = type;
  event.kind = static_cast<Bench::ReadKind>(seed % Bench::read_kind_count);
  event.cls = static_cast<Bench::ReadClass>(seed % Bench::read_class_count);
  event.phase = static_cast<std::uint8_t>(seed % Bench::phase_count);
  event.queue_depth = static_cast<std::uint32_t>(seed % 70);
  return event;
}

// A log that holds one event of each type, and some more reads.
Bench::ReadLog SampleLog() {
  Bench::ReadLog log;
  log.volume_bytes = 0x123456789ABC;
  log.start_unix_ns = 1'700'000'000'123'456'789;
  const std::array types{Bench::EventType::Read,
                         Bench::EventType::Prefetch,
                         Bench::EventType::Cancel,
                         Bench::EventType::Pattern,
                         Bench::EventType::PhaseBegin,
                         Bench::EventType::PhaseEnd,
                         Bench::EventType::StandingPrefetch,
                         Bench::EventType::StandingCancel};
  for (std::uint64_t index = 0; index < 20; index++) {
    log.events.push_back(MakeEvent(types.at(index % types.size()), index));
  }
  return log;
}

// The fields of an event, to compare them in one expression.
auto FieldsOf(const Bench::LogEvent& event) {
  return std::tie(event.start_ns, event.end_ns, event.wait_ns, event.offset,
                  event.length, event.type, event.kind, event.cls, event.phase,
                  event.queue_depth);
}

bool SameEvent(const Bench::LogEvent& left, const Bench::LogEvent& right) {
  return FieldsOf(left) == FieldsOf(right);
}

void CheckSameLog(const Bench::ReadLog& left, const Bench::ReadLog& right) {
  CHECK(left.volume_bytes == right.volume_bytes);
  CHECK(left.start_unix_ns == right.start_unix_ns);
  REQUIRE(left.events.size() == right.events.size());
  for (size_t index = 0; index < left.events.size(); index++) {
    INFO("event " << index);
    CHECK(SameEvent(left.events.at(index), right.events.at(index)));
  }
}

std::vector<char> ReadFile(const std::filesystem::path& path) {
  std::ifstream in(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

void WriteFile(const std::filesystem::path& path,
               const std::vector<char>& bytes) {
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

template <typename T>
void Patch(std::vector<char>& bytes, size_t offset, T value) {
  std::memcpy(bytes.data() + offset, &value, sizeof(value));
}

// The file of log written under dir, as bytes.
std::vector<char> Serialized(const TestDeviceAccess::TempDir& dir,
                             const Bench::ReadLog& log) {
  const std::filesystem::path path = dir.Path() / "good.readlog";
  std::string error;
  REQUIRE(Bench::WriteReadLog(path, log, &error));
  return ReadFile(path);
}

// Loads a file that holds bytes, and returns the reason it is refused.
std::string RefusedBecause(const TestDeviceAccess::TempDir& dir,
                           const std::vector<char>& bytes) {
  const std::filesystem::path path = dir.Path() / "bad.readlog";
  WriteFile(path, bytes);
  std::string error;
  std::optional<Bench::ReadLog> loaded;
  REQUIRE_NOTHROW(loaded = Bench::LoadReadLog(path, &error));
  CHECK_FALSE(loaded.has_value());
  CHECK_FALSE(error.empty());
  return error;
}

}  // namespace

TEST_CASE("A read log written and loaded is the same log",
          "[bench][read-log]") {
  const TestDeviceAccess::TempDir dir;
  const Bench::ReadLog log = SampleLog();
  const std::filesystem::path path = dir.Path() / "round.readlog";

  std::string error;
  REQUIRE(Bench::WriteReadLog(path, log, &error));
  const std::optional<Bench::ReadLog> loaded = Bench::LoadReadLog(path, &error);

  REQUIRE(loaded.has_value());
  CheckSameLog(log, *loaded);
}

TEST_CASE("A read log file has the documented layout", "[bench][read-log]") {
  const TestDeviceAccess::TempDir dir;
  const Bench::ReadLog log = SampleLog();
  const std::vector<char> bytes = Serialized(dir, log);

  REQUIRE(bytes.size() == header_bytes + (log.events.size() * event_bytes));
  CHECK(std::string(bytes.data(), 8) == "NTFSRDLG");
  std::uint32_t version = 0;
  std::uint32_t size = 0;
  std::uint64_t count = 0;
  std::memcpy(&version, bytes.data() + version_offset, sizeof(version));
  std::memcpy(&size, bytes.data() + event_size_offset, sizeof(size));
  std::memcpy(&count, bytes.data() + count_offset, sizeof(count));
  CHECK(version == 2);
  CHECK(size == event_bytes);
  CHECK(count == log.events.size());
}

TEST_CASE("An empty read log round trips", "[bench][read-log]") {
  const TestDeviceAccess::TempDir dir;
  Bench::ReadLog log;
  log.volume_bytes = 42;
  log.start_unix_ns = 7;
  const std::filesystem::path path = dir.Path() / "empty.readlog";

  std::string error;
  REQUIRE(Bench::WriteReadLog(path, log, &error));
  CHECK(std::filesystem::file_size(path) == header_bytes);
  const std::optional<Bench::ReadLog> loaded = Bench::LoadReadLog(path, &error);

  REQUIRE(loaded.has_value());
  CheckSameLog(log, *loaded);
}

TEST_CASE("A log with more events than one chunk round trips",
          "[bench][read-log]") {
  const TestDeviceAccess::TempDir dir;
  Bench::ReadLog log;
  log.volume_bytes = 1;
  for (std::uint64_t index = 0; index < 40000; index++) {
    log.events.push_back(MakeEvent(Bench::EventType::Read, index));
  }
  const std::filesystem::path path = dir.Path() / "long.readlog";

  std::string error;
  REQUIRE(Bench::WriteReadLog(path, log, &error));
  const std::optional<Bench::ReadLog> loaded = Bench::LoadReadLog(path, &error);

  REQUIRE(loaded.has_value());
  CheckSameLog(log, *loaded);
}

TEST_CASE("The log of a real session round trips", "[bench][read-log]") {
  const TestDeviceAccess::TempDir dir;
  const std::vector<BYTE> image =
      TestFakeImage::BuildFakeNtfsImageWithListMftScan(
          3000, TestFakeImage::fragmented_mft_scan_fragment_clusters);

  TestSupport::BenchSessionGuard session;
  {
    const NtfsVolume<Cache::Strategy::NoCache> volume(
        std::make_unique<TestDiskReaders::MemoryDiskReader>(image),
        VolumeOptions{.include_deleted = true});
    REQUIRE(volume.IsVolumeOK());
    Mft::MftTree::ScanOptions scan;
    scan.threads = 1;
    const Mft::MftTree tree(volume, scan);
    REQUIRE(tree.Stats().complete);
  }
  const Bench::Report report = session.Finish();
  const Bench::ReadLog log = Bench::TakeReadLog();
  REQUIRE(log.events.size() == report.events_logged);
  REQUIRE(log.events.size() > 100);
  CHECK(log.volume_bytes == image.size());

  const std::filesystem::path path = dir.Path() / "scan.readlog";
  std::string error;
  REQUIRE(Bench::WriteReadLog(path, log, &error));
  const std::optional<Bench::ReadLog> loaded = Bench::LoadReadLog(path, &error);
  REQUIRE(loaded.has_value());
  CheckSameLog(log, *loaded);
}

TEST_CASE("A read log that is cut is refused", "[bench][read-log]") {
  const TestDeviceAccess::TempDir dir;
  const std::vector<char> good = Serialized(dir, SampleLog());
  // Inside the header, at the header, inside an event, one byte short.
  const size_t length =
      GENERATE_COPY(size_t{0}, size_t{7}, header_bytes - 1, header_bytes + 5,
                    header_bytes + event_bytes,
                    header_bytes + (10 * event_bytes) + 20, good.size() - 1);
  INFO("cut at " << length);

  const std::vector<char> cut(
      good.begin(), good.begin() + static_cast<std::ptrdiff_t>(length));
  const std::string error = RefusedBecause(dir, cut);
  if (length < header_bytes) {
    CHECK_THAT(error, ContainsSubstring("not a read log"));
  } else {
    CHECK_THAT(error, ContainsSubstring("cut"));
  }
}

TEST_CASE("A file with another magic is refused", "[bench][read-log]") {
  const TestDeviceAccess::TempDir dir;
  std::vector<char> bytes = Serialized(dir, SampleLog());
  bytes.at(0) = 'X';

  CHECK_THAT(RefusedBecause(dir, bytes), ContainsSubstring("not a read log"));
}

TEST_CASE("A file of another version is refused", "[bench][read-log]") {
  const TestDeviceAccess::TempDir dir;
  const std::uint32_t version =
      GENERATE(std::uint32_t{0}, std::uint32_t{3}, std::uint32_t{0xFFFFFFFF});
  std::vector<char> bytes = Serialized(dir, SampleLog());
  Patch(bytes, version_offset, version);

  const std::string error = RefusedBecause(dir, bytes);
  CHECK_THAT(error, ContainsSubstring("version"));
  // The reason names the version found and the ones this build reads.
  CHECK_THAT(error, ContainsSubstring(std::to_string(version)));
  CHECK_THAT(error, ContainsSubstring("1 to 2"));
}

TEST_CASE("A version 1 read log still loads", "[bench][read-log]") {
  const TestDeviceAccess::TempDir dir;
  const Bench::ReadLog log = SampleLog();
  std::vector<char> bytes = Serialized(dir, log);
  Patch(bytes, version_offset, std::uint32_t{1});
  const std::filesystem::path path = dir.Path() / "old.readlog";
  WriteFile(path, bytes);

  std::string error;
  const std::optional<Bench::ReadLog> loaded = Bench::LoadReadLog(path, &error);

  REQUIRE(loaded.has_value());
  CheckSameLog(log, *loaded);
}

TEST_CASE("The standing lane events keep their letters in the file",
          "[bench][read-log]") {
  const TestDeviceAccess::TempDir dir;
  Bench::ReadLog log;
  log.events.push_back(MakeEvent(Bench::EventType::StandingPrefetch, 1));
  log.events.push_back(MakeEvent(Bench::EventType::StandingCancel, 2));
  const std::vector<char> bytes = Serialized(dir, log);

  // The type is the byte after the five 64-bit fields of an event.
  constexpr size_t type_offset = 40;
  REQUIRE(bytes.size() == header_bytes + (2 * event_bytes));
  CHECK(bytes.at(header_bytes + type_offset) == 'S');
  CHECK(bytes.at(header_bytes + event_bytes + type_offset) == 'X');
  CHECK(static_cast<char>(Bench::EventType::Prefetch) == 'P');
  CHECK(static_cast<char>(Bench::EventType::Cancel) == 'C');
}

TEST_CASE("A file with another event size is refused", "[bench][read-log]") {
  const TestDeviceAccess::TempDir dir;
  const std::uint32_t size =
      GENERATE(std::uint32_t{0}, std::uint32_t{40}, std::uint32_t{56});
  std::vector<char> bytes = Serialized(dir, SampleLog());
  Patch(bytes, event_size_offset, size);

  CHECK_THAT(RefusedBecause(dir, bytes), ContainsSubstring("version"));
}

TEST_CASE("A forged event count is refused and reserves no memory",
          "[bench][read-log]") {
  const TestDeviceAccess::TempDir dir;
  const Bench::ReadLog log = SampleLog();
  // One more than the file holds, and counts whose events would take
  // terabytes, or that wrap a size computed from them.
  const std::uint64_t count = GENERATE_COPY(
      static_cast<std::uint64_t>(log.events.size() + 1),
      std::uint64_t{1} << 20U, std::uint64_t{1} << 40U, std::uint64_t{1} << 58U,
      ~std::uint64_t{0}, (~std::uint64_t{0} / event_bytes) + 1);
  INFO("count " << count);
  std::vector<char> bytes = Serialized(dir, log);
  Patch(bytes, count_offset, count);

  CHECK_THAT(RefusedBecause(dir, bytes), ContainsSubstring("cut"));
}

TEST_CASE("A forged count in a file with no event at all is refused",
          "[bench][read-log]") {
  const TestDeviceAccess::TempDir dir;
  std::vector<char> bytes = Serialized(dir, Bench::ReadLog{});
  REQUIRE(bytes.size() == header_bytes);
  Patch(bytes, count_offset, std::uint64_t{1} << 62U);

  CHECK_THAT(RefusedBecause(dir, bytes), ContainsSubstring("cut"));
}

TEST_CASE("A missing read log is refused with its path", "[bench][read-log]") {
  const TestDeviceAccess::TempDir dir;
  const std::filesystem::path path = dir.Path() / "no-such-file.readlog";

  std::string error;
  CHECK_FALSE(Bench::LoadReadLog(path, &error).has_value());
  CHECK_THAT(error, ContainsSubstring("cannot open"));
  // The error pointer is optional.
  CHECK_FALSE(Bench::LoadReadLog(path, nullptr).has_value());
}

TEST_CASE("A read log is not written where no file can be made",
          "[bench][read-log]") {
  const TestDeviceAccess::TempDir dir;
  const std::filesystem::path path = dir.Path() / "no-folder" / "x.readlog";

  std::string error;
  CHECK_FALSE(Bench::WriteReadLog(path, SampleLog(), &error));
  CHECK_THAT(error, ContainsSubstring("cannot create"));
  CHECK_FALSE(Bench::WriteReadLog(path, SampleLog(), nullptr));
}
