#include "scan-comparison.h"

#include <ntfs-browser/win-types.h>

#include <algorithm>
#include <cstddef>
#include <format>
#include <string>
#include <string_view>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include <ntfs-browser/cache/strategy.h>
#include <ntfs-browser/mft/tree.h>
#include <ntfs-browser/namespace.h>
#include <ntfs-browser/ntfs-volume.h>

#include "counting-disk-reader.h"
#include "mft/tree-testing.h"
#include "test-log-sink.h"

namespace NTFS_BROWSER_NAMESPACE::Tests {

namespace {

// The code units of text, in hex: a name prints whatever it holds.
std::string Hex(std::wstring_view text) {
  std::string out;
  for (const wchar_t unit : text) {
    out += std::format("{:x}.", static_cast<unsigned>(unit));
  }
  return out;
}

ULONGLONG Ticks(const FILETIME& time) {
  return (static_cast<ULONGLONG>(time.dwHighDateTime) << 32U) |
         time.dwLowDateTime;
}

template <Cache::Strategy S>
ScanResult Capture(const NtfsVolume<S>& volume,
                   const CountingDiskReader* reader, unsigned threads,
                   size_t progress_stops_at) {
  Mft::MftTree::ScanOptions scan;
  scan.threads = threads;
  size_t progress_calls = 0;
  if (progress_stops_at != 0) {
    scan.progress = [&](ULONGLONG /*done*/, ULONGLONG /*total*/) {
      progress_calls++;
      return progress_calls < progress_stops_at;
    };
  }

  static_cast<void>(Mft::SlotsParsedByThreadsForTests(true));
  static_cast<void>(Mft::WaitingParsedByThreadsForTests(true));
  const Mft::MftTree tree(volume, scan);

  ScanResult result;
  result.parsed_by_threads = Mft::SlotsParsedByThreadsForTests(true);
  result.waiting_by_threads = Mft::WaitingParsedByThreadsForTests(true);
  result.tree = DescribeTree(tree);
  result.log = TakeCapturedLog();
  if (reader != nullptr) {
    result.calls = reader->Calls();
  }
  return result;
}

}  // namespace

std::string DescribeTree(const Mft::MftTree& tree) {
  using Tree = Mft::MftTree;
  std::string out;
  for (const Tree::Entry& entry : tree.Entries()) {
    out += std::format(
        "{} seq {} use {} dir {} size {} alloc {} times {:x} {:x} {:x} {:x} "
        "flags {}{}{}{}{}{}{} state {} usn {} reach {}\n",
        entry.record, entry.sequence, entry.in_use, entry.directory, entry.size,
        entry.allocated_size, Ticks(entry.create_time), Ticks(entry.write_time),
        Ticks(entry.change_time), Ticks(entry.access_time), entry.read_only,
        entry.hidden, entry.system, entry.archive, entry.compressed,
        entry.encrypted, entry.sparse,
        entry.data_state ? static_cast<int>(*entry.data_state) : -1,
        entry.usn.has_value(), tree.IsReachable(entry.record));
    for (const Tree::Name& name : entry.names) {
      out +=
          std::format("  name {} parent {} seq {} dos {} valid {}\n",
                      Hex(name.name), name.parent_record, name.parent_sequence,
                      name.dos_only, name.parent_valid);
    }
    for (const auto& carved : entry.carved_names) {
      out += std::format(
          "  carved {} parent {} {} known {} {} times {:x} {:x} {:x} {:x} "
          "ns {} partial {}\n",
          Hex(carved.name), carved.parent_record, carved.parent_sequence,
          carved.parent_known, carved.times_known, Ticks(carved.create_time),
          Ticks(carved.write_time), Ticks(carved.change_time),
          Ticks(carved.access_time), carved.name_space, carved.partial);
    }
  }
  const Tree::ScanStats& stats = tree.Stats();
  out += std::format(
      "slots {} use {} deleted {} ext {} unreadable {} skipped {} damaged {} "
      "reused {} unreachable {} complete {} carved {} data {} {} {} {}\n",
      stats.slots, stats.in_use, stats.deleted, stats.extensions,
      stats.unreadable, stats.skipped, stats.damaged, stats.reused_extensions,
      stats.unreachable, stats.complete, stats.carved_names, stats.data_intact,
      stats.data_reallocated, stats.data_unknown, stats.data_bad);
  for (const Tree::Entry& entry : tree.Entries()) {
    for (const ULONGLONG child : tree.Children(entry.record)) {
      out += std::format("child {} of {}\n", child, entry.record);
    }
  }
  return out;
}

std::string Divergence(const std::string& left, const std::string& right) {
  if (left == right) {
    return {};
  }
  const auto common = static_cast<size_t>(
      std::ranges::mismatch(left, right).in1 - left.begin());
  // The start of the line before the first difference: the texts agree up to
  // there, so it is the same in both.
  size_t start = left.rfind('\n', common == 0 ? 0 : common - 1);
  if (start != std::string::npos && start > 0) {
    start = left.rfind('\n', start - 1);
  }
  start = start == std::string::npos ? 0 : start + 1;
  return "left:\n" + left.substr(start, 400) + "\nright:\n" +
         right.substr(start, 400);
}

bool SameCalls(const ScanResult& left, const ScanResult& right) {
  return std::ranges::equal(
      left.calls, right.calls, [](const auto& lhs, const auto& rhs) {
        return lhs.kind == rhs.kind && lhs.offset == rhs.offset &&
               lhs.length == rhs.length;
      });
}

void CheckSameScan(const ScanResult& one, const ScanResult& many) {
  const bool same_tree = many.tree == one.tree;
  INFO("tree " << Divergence(one.tree, many.tree));
  CHECK(same_tree);
  const bool same_log = many.log == one.log;
  INFO("log " << Divergence(one.log, many.log));
  CHECK(same_log);
  CHECK(SameCalls(many, one));
}

ScanResult CaptureScan(const NtfsVolume<Cache::Strategy::NoCache>& volume,
                       const CountingDiskReader* reader, unsigned threads,
                       size_t progress_stops_at) {
  return Capture(volume, reader, threads, progress_stops_at);
}

ScanResult CaptureScan(const NtfsVolume<Cache::Strategy::FullCache>& volume,
                       const CountingDiskReader* reader, unsigned threads,
                       size_t progress_stops_at) {
  return Capture(volume, reader, threads, progress_stops_at);
}

}  // namespace NTFS_BROWSER_NAMESPACE::Tests
