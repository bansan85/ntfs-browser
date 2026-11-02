#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <ios>
#include <optional>
#include <string>
#include <vector>

#include <ntfs-browser/bench/bench.h>
#include <ntfs-browser/io/extended-path.h>
#include <ntfs-browser/namespace.h>

namespace NTFS_BROWSER_NAMESPACE::Bench {

namespace {

static_assert(std::endian::native == std::endian::little,
              "the read log is little endian");

// First bytes of a read log file.
constexpr std::array<char, 8> log_magic{'N', 'T', 'F', 'S', 'R', 'D', 'L', 'G'};

// Version of the layout below. A change of the layout MUST bump it. Version 2
// adds the event types 'S' and 'X' (the standing lane) and keeps the layout, so
// a version 1 file still loads.
constexpr std::uint32_t log_version = 2;

// The oldest version LoadReadLog() accepts.
constexpr std::uint32_t oldest_readable_version = 1;

// Size of the header, so that the events start on a multiple of 16.
constexpr std::size_t header_bytes = 64;

// The 64 bytes that open a read log file.
struct ReadLogHeader {
  std::array<char, 8> magic{};
  std::uint32_t version{0};
  std::uint32_t event_bytes{0};
  std::uint64_t event_count{0};
  std::uint64_t volume_bytes{0};
  std::uint64_t start_unix_ns{0};
  std::array<std::uint8_t, 24> reserved{};
};

static_assert(sizeof(ReadLogHeader) == header_bytes);

// Events read from or written to a stream in one call.
constexpr std::size_t events_per_chunk = 1U << 14U;

}  // namespace

bool WriteReadLog(const std::filesystem::path& path, const ReadLog& log,
                  std::string* error) {
  const auto fail = [error](const std::string& message) {
    if (error != nullptr) {
      *error = message;
    }
    return false;
  };

  std::ofstream out(Io::ToExtendedLengthFsPath(path),
                    std::ios::binary | std::ios::trunc);
  if (!out) {
    return fail("cannot create " + path.string());
  }
  ReadLogHeader header;
  header.magic = log_magic;
  header.version = log_version;
  header.event_bytes = static_cast<std::uint32_t>(sizeof(LogEvent));
  header.event_count = log.events.size();
  header.volume_bytes = log.volume_bytes;
  header.start_unix_ns = log.start_unix_ns;
  out.write(reinterpret_cast<const char*>(&header),
            static_cast<std::streamsize>(sizeof(header)));
  for (std::size_t done = 0; done < log.events.size();) {
    const std::size_t count =
        (std::min)(events_per_chunk, log.events.size() - done);
    out.write(reinterpret_cast<const char*>(log.events.data() + done),
              static_cast<std::streamsize>(count * sizeof(LogEvent)));
    done += count;
  }
  out.flush();
  if (!out) {
    return fail("cannot write " + path.string());
  }
  return true;
}

std::optional<ReadLog> LoadReadLog(const std::filesystem::path& path,
                                   std::string* error) {
  const auto fail = [error](const std::string& message) {
    if (error != nullptr) {
      *error = message;
    }
    return std::optional<ReadLog>{};
  };

  std::ifstream in(Io::ToExtendedLengthFsPath(path), std::ios::binary);
  if (!in) {
    return fail("cannot open " + path.string());
  }
  ReadLogHeader header;
  in.read(reinterpret_cast<char*>(&header),
          static_cast<std::streamsize>(sizeof(header)));
  if (!in || header.magic != log_magic) {
    return fail(path.string() + " is not a read log");
  }
  if (header.version < oldest_readable_version ||
      header.version > log_version || header.event_bytes != sizeof(LogEvent)) {
    return fail(path.string() + " has an unknown read log version (" +
                std::to_string(header.version) + ", this build reads " +
                std::to_string(oldest_readable_version) + " to " +
                std::to_string(log_version) + ")");
  }

  ReadLog log;
  log.volume_bytes = header.volume_bytes;
  log.start_unix_ns = header.start_unix_ns;
  // The count comes from the file: a forged one MUST NOT reserve memory the
  // file does not back, so the events are read by chunks as they exist.
  for (std::uint64_t done = 0; done < header.event_count;) {
    const std::size_t count = static_cast<std::size_t>(
        (std::min<std::uint64_t>)(events_per_chunk, header.event_count - done));
    const std::size_t first = log.events.size();
    log.events.resize(first + count);
    in.read(reinterpret_cast<char*>(log.events.data() + first),
            static_cast<std::streamsize>(count * sizeof(LogEvent)));
    if (!in) {
      return fail(path.string() + " is cut: fewer events than its header says");
    }
    done += count;
  }
  return log;
}

}  // namespace NTFS_BROWSER_NAMESPACE::Bench
