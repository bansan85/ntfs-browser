#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>

#include <ntfs-browser/namespace.h>
#include <ntfs-browser/test/disk-readers/counting-disk-reader.h>
#include <ntfs-browser/test/disk-readers/memory-disk-reader.h>
#include <ntfs-browser/test/support/scan-comparison.h>
#include <ntfs-browser/test/support/test-log-sink.h>

#include "cache/strategy.h"
#include "disk-reader.h"
#include "ntfs-volume.h"
#include "tuning/knobs.h"
#include "tuning/tuning.h"
#include "volume-options.h"
#include "win-types.h"

namespace NTFS_BROWSER_NAMESPACE {
namespace Tuning {

enum class Pattern : std::uint8_t;

}  // namespace Tuning
}  // namespace NTFS_BROWSER_NAMESPACE

namespace NTFS_BROWSER_NAMESPACE::Test::Tuning {

using Call = Test::DiskReaders::CountingDiskReader::Call;
using Kind = Test::DiskReaders::CountingDiskReader::Kind;

// One knob to set.
struct Setting {
  std::string_view name;
  std::uint64_t value;
  std::optional<NTFS_BROWSER_NAMESPACE::Tuning::Pattern> pattern{};
};

// base with the settings applied through Tuning::ApplyKnob(), the one door a
// profile uses. A refusal fails the test.
inline VolumeOptions Tuned(VolumeOptions base,
                           std::initializer_list<Setting> settings) {
  for (const Setting& setting : settings) {
    const std::string failure = NTFS_BROWSER_NAMESPACE::Tuning::ApplyKnob(
        base, setting.name, setting.value, setting.pattern);
    INFO("setting " << setting.name << " = " << setting.value);
    REQUIRE(failure.empty());
  }
  return base;
}

// The options of a scan that keeps the freed records and recovers.
inline VolumeOptions Recovering() {
  return VolumeOptions{.include_deleted = true, .recover_errors = true};
}

// Scans image over a counting reader with threads threads, and gathers what
// the scan did.
inline Test::Support::ScanResult ScanOf(const std::vector<BYTE>& image,
                                        const VolumeOptions& options,
                                        unsigned threads = 1) {
  static_cast<void>(Test::Support::TakeCapturedLog());
  auto reader = std::make_unique<Test::DiskReaders::CountingDiskReader>(
      std::make_unique<Test::DiskReaders::MemoryDiskReader>(image));
  const Test::DiskReaders::CountingDiskReader* const counting = reader.get();
  const NtfsVolume<NTFS_BROWSER_NAMESPACE::Cache::Strategy::NoCache> volume(
      std::move(reader), options);
  REQUIRE(volume.IsVolumeOK());
  return Test::Support::CaptureScan(volume, counting, threads);
}

inline size_t Count(const std::vector<Call>& calls, Kind kind) {
  return static_cast<size_t>(std::ranges::count_if(
      calls, [kind](const Call& call) { return call.kind == kind; }));
}

// The calls of kind that satisfy pick.
template <typename Pick>
std::vector<Call> Select(const std::vector<Call>& calls, Kind kind, Pick pick) {
  std::vector<Call> picked;
  for (const Call& call : calls) {
    if (call.kind == kind && pick(call)) {
      picked.push_back(call);
    }
  }
  return picked;
}

// The longest read among calls, or 0.
inline ULONGLONG LongestRead(const std::vector<Call>& calls) {
  ULONGLONG longest = 0;
  for (const Call& call : calls) {
    if (call.kind == Kind::Read) {
      longest = (std::max)(longest, call.length);
    }
  }
  return longest;
}

// What the hints of calls hold at their fullest: ranges announced and not yet
// read. A read drops the ranges announced up to the one it falls in, as a
// reader that reads ahead does, and a withdrawal drops them all.
struct Outstanding {
  size_t ranges{0};
  ULONGLONG bytes{0};
};

inline Outstanding MaxOutstanding(const std::vector<Call>& calls) {
  std::vector<Call> pending;
  Outstanding most;
  for (const Call& call : calls) {
    if (call.kind == Kind::CancelPrefetch) {
      pending.clear();
    } else if (call.kind == Kind::Prefetch) {
      pending.push_back(call);
      ULONGLONG bytes = 0;
      for (const Call& hint : pending) {
        bytes += hint.length;
      }
      most.ranges = (std::max)(most.ranges, pending.size());
      most.bytes = (std::max)(most.bytes, bytes);
    } else if (call.kind == Kind::Read) {
      const auto hit = std::ranges::find_if(pending, [&call](const Call& hint) {
        return call.offset >= hint.offset &&
               call.offset + call.length <= hint.offset + hint.length;
      });
      if (hit != pending.end()) {
        pending.erase(pending.begin(), hit + 1);
      }
    }
  }
  return most;
}

// The calls that touch [first, last), and every withdrawal.
inline std::vector<Call> Region(const std::vector<Call>& calls, ULONGLONG first,
                                ULONGLONG last) {
  std::vector<Call> kept;
  for (const Call& call : calls) {
    if (call.kind == Kind::CancelPrefetch ||
        (call.offset < last && call.offset + call.length > first)) {
      kept.push_back(call);
    }
  }
  return kept;
}

}  // namespace NTFS_BROWSER_NAMESPACE::Test::Tuning
