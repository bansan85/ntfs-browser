#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include <ntfs-browser/namespace.h>

#include "bench/types.h"
#include "internal-export.h"

namespace NTFS_BROWSER_NAMESPACE::Bench {

// The event types of the read log.
enum class EventType : std::uint8_t {
  // A ReadInto() that reached the reader.
  Read = 'R',
  // A Prefetch().
  Prefetch = 'P',
  // A CancelPrefetch().
  Cancel = 'C',
  // A SetReadPattern(): length holds the Tuning::Pattern.
  Pattern = 'T',
  // A phase began ('B') or ended ('E'): length holds the Phase.
  PhaseBegin = 'B',
  PhaseEnd = 'E',
  // A PrefetchStanding(): a range announced on the standing lane. Version 2.
  StandingPrefetch = 'S',
  // A CancelStanding(): the interval withdrawn from the standing lane, where
  // offset 0 and length UINT64_MAX is the whole lane. Version 2.
  StandingCancel = 'X'
};

// One entry of the read log. The file holds them as they are, 48 bytes each,
// little endian, after a 64-byte header (see ReadLogHeader in the sources).
struct LogEvent {
  // Start and end of the call, in nanoseconds from the session start. A call
  // that takes no time, or a marker, has both equal.
  std::uint64_t start_ns{0};
  std::uint64_t end_ns{0};
  // Time the caller spent waiting for the disk inside the call.
  std::uint64_t wait_ns{0};
  std::uint64_t offset{0};
  std::uint64_t length{0};
  EventType type{EventType::Read};
  ReadKind kind{ReadKind::Other};
  ReadClass cls{ReadClass::Unclassified};
  // The Phase that was innermost when the call began.
  std::uint8_t phase{0};
  // Reads in flight when the call began, itself included, when known.
  std::uint32_t queue_depth{0};
};

static_assert(sizeof(LogEvent) == 48);

// The read log of a session, to write or to replay.
struct ReadLog {
  std::uint64_t volume_bytes{0};
  std::uint64_t start_unix_ns{0};
  std::vector<LogEvent> events;
};

// Writes log to path in the binary format of the read log. False, with the
// reason in error, when the file cannot be written.
[[nodiscard]] NTFS_BROWSER_EXPORT_TESTS_ONLY bool
    WriteReadLog(const std::filesystem::path& path, const ReadLog& log,
                 std::string* error);

// Reads a file WriteReadLog() wrote, in version 2 or in version 1 (which has
// no standing lane events). Nothing, with the reason in error, when the file is
// missing, cut or of another version.
[[nodiscard]] NTFS_BROWSER_EXPORT_TESTS_ONLY std::optional<ReadLog>
    LoadReadLog(const std::filesystem::path& path, std::string* error);

}  // namespace NTFS_BROWSER_NAMESPACE::Bench
