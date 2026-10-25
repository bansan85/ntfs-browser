#include <ntfs-browser/win-types.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <catch2/catch_template_test_macros.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <ntfs-browser/attr/mask.h>
#include <ntfs-browser/cache/strategy.h>
#include <ntfs-browser/index-entry.h>
#include <ntfs-browser/io/file-record.h>
#include <ntfs-browser/io/record-standing.h>
#include <ntfs-browser/mft/idx.h>
#include <ntfs-browser/namespace.h>
#include <ntfs-browser/ntfs-volume.h>
#include <ntfs-browser/volume-options.h>

#include "catch2/matchers/catch_matchers.hpp"
#include "counting-disk-reader.h"
#include "fake-ntfs-image.h"
#include "memory-disk-reader.h"
#include "test-log-sink.h"
#include "walk-comparison.h"

namespace Cache = NTFS_BROWSER_NAMESPACE::Cache;
namespace Mft = NTFS_BROWSER_NAMESPACE::Mft;
namespace Tests = NTFS_BROWSER_NAMESPACE::Tests;
using Catch::Matchers::ContainsSubstring;
using NTFS_BROWSER_NAMESPACE::IndexEntryView;
using NTFS_BROWSER_NAMESPACE::NtfsVolume;
using NTFS_BROWSER_NAMESPACE::VolumeOptions;
using NTFS_BROWSER_NAMESPACE::Io::FileRecord;
using NTFS_BROWSER_NAMESPACE::Io::RecordStanding;
using Tests::CountingDiskReader;
using Tests::MemoryDiskReader;
using Tests::WalkedEntry;
using Tests::WalkResult;

namespace {

// Slots of the indexed scan fixture the tests use.
constexpr ULONGLONG scan_records = 100;

// Slot kinds of the fixture, as slot % 8 gives them: a freed file, and a
// zero-filled slot.
constexpr ULONGLONG freed_kind = 2;
constexpr ULONGLONG zeroed_kind = 4;
constexpr ULONGLONG slot_kinds = 8;

// The four ways to open a volume that decide what a walk shows.
std::array<VolumeOptions, 4> AllOptions() {
  return {VolumeOptions{}, VolumeOptions{.recover_errors = true},
          VolumeOptions{.include_deleted = true},
          VolumeOptions{.include_deleted = true, .recover_errors = true}};
}

template <Cache::Strategy S>
std::unique_ptr<NtfsVolume<S>>
    OpenVolume(std::unique_ptr<NTFS_BROWSER_NAMESPACE::IDiskReader> reader,
               const VolumeOptions& options) {
  auto volume = std::make_unique<NtfsVolume<S>>(std::move(reader), options);
  REQUIRE(volume->IsVolumeOK());
  return volume;
}

// Walks a volume of image both ways and checks that the batched walk shows the
// entries of the default one, in the same order, with the standing the probe of
// a caller gives, and logs what it logs. Returns the batched walk.
template <Cache::Strategy S>
WalkResult CheckSameWalk(const std::vector<BYTE>& image,
                         const VolumeOptions& options) {
  const auto plain =
      OpenVolume<S>(std::make_unique<MemoryDiskReader>(image), options);
  const WalkResult expected = Tests::WalkVolume(*plain, false);
  const auto batched =
      OpenVolume<S>(std::make_unique<MemoryDiskReader>(image), options);
  WalkResult actual = Tests::WalkVolume(*batched, true);

  CHECK(actual.entries == expected.entries);
  CHECK(actual.log == expected.log);
  return actual;
}

// The fixtures a walk is compared on, and a name for each.
struct Fixture {
  std::string name;
  std::function<std::vector<BYTE>()> build;
  // Whether every way of opening the fixture lists some entry.
  bool lists_entries = false;
};

std::vector<Fixture> Fixtures() {
  using namespace NTFS_BROWSER_NAMESPACE::Tests;
  return {
      {"indexed scan", [] { return BuildFakeNtfsImageWithIndexedMftScan(100); },
       true},
      {"indexed scan, small blocks",
       [] { return BuildFakeNtfsImageWithIndexedMftScan(120, 4); }, true},
      {"indexed scan, one block",
       [] { return BuildFakeNtfsImageWithIndexedMftScan(20); }, true},
      {"orphaned blocks",
       [] { return BuildFakeNtfsImageWithOrphanedIndexBlocks(); }},
      {"multi-cluster orphaned block",
       [] { return BuildFakeNtfsImageWithMultiClusterOrphanedIndexBlock(); }},
      {"bad index block entry",
       [] { return BuildFakeNtfsImageWithBadIndexBlockEntry(); }},
      {"malformed index entry",
       [] { return BuildFakeNtfsImageWithMalformedIndexEntryFilename(); }},
      {"huge orphan block count",
       [] { return BuildFakeNtfsImageWithHugeOrphanScanBlockCount(); }},
  };
}

// The standing the entry should get in a walk of the indexed scan fixture that
// shows deleted items and recovers. A slot whose update sequence check fails is
// salvaged then, and stands like a live one.
RecordStanding ExpectedStanding(const WalkedEntry& entry) {
  const ULONGLONG kind = entry.reference % slot_kinds;
  if (kind == freed_kind) {
    return RecordStanding::Deleted;
  }
  if (kind == zeroed_kind) {
    return RecordStanding::Unreadable;
  }
  if (entry.sequence == Tests::indexed_scan_stale_sequence) {
    return RecordStanding::Reused;
  }
  return RecordStanding::Live;
}

// The bytes of slot idx of the fixture's $MFT: the range its probe reads.
ULONGLONG SlotOffset(ULONGLONG idx) {
  return Tests::FragmentedMftScanSlotOffset(idx);
}

// What a walk of the root directory alone did to the reader, and what it
// logged.
struct RootWalk {
  std::vector<CountingDiskReader::Call> calls;
  std::string log;
};

// Walks the root directory only, through the walk that wants the given mode,
// over a volume of image. The calls and the log are those of the walk, not of
// opening the volume or parsing the root.
template <Cache::Strategy S>
RootWalk WalkRootOnly(const std::vector<BYTE>& image, bool batched,
                      const VolumeOptions& options = {}) {
  auto reader = std::make_unique<CountingDiskReader>(
      std::make_unique<MemoryDiskReader>(image));
  CountingDiskReader& reads = *reader;
  const auto volume = OpenVolume<S>(std::move(reader), options);

  FileRecord<S> root(*volume);
  root.SetAttrMask(NTFS_BROWSER_NAMESPACE::Attr::Mask::IndexRoot |
                   NTFS_BROWSER_NAMESPACE::Attr::Mask::IndexAllocation);
  REQUIRE(root.ParseFileRecord(static_cast<ULONGLONG>(Mft::Idx::Root)));
  REQUIRE(root.ParseAttrs());
  reads.Reset();
  (void)Tests::TakeCapturedLog();
  if (batched) {
    root.TraverseSubEntriesWithStanding(
        [](const IndexEntryView&, RecordStanding, void*) {}, nullptr);
  } else {
    root.TraverseSubEntries([](const IndexEntryView&, void*) {}, nullptr);
  }
  return {.calls = reads.Calls(), .log = Tests::TakeCapturedLog()};
}

// How many times text occurs in log.
size_t Occurrences(const std::string& log, const std::string& text) {
  size_t count = 0;
  for (size_t at = log.find(text); at != std::string::npos;
       at = log.find(text, at + text.size())) {
    count++;
  }
  return count;
}

size_t ReadsIn(const std::vector<CountingDiskReader::Call>& calls) {
  return static_cast<size_t>(
      std::ranges::count_if(calls, [](const CountingDiskReader::Call& call) {
        return call.kind == CountingDiskReader::Kind::Read;
      }));
}

}  // namespace

TEMPLATE_TEST_CASE_SIG("The batched walk shows what the default walk shows",
                       "[file-record][traverse-standing]",
                       ((Cache::Strategy S), S), Cache::Strategy::NoCache,
                       Cache::Strategy::FullCache) {
  for (const Fixture& fixture : Fixtures()) {
    for (const VolumeOptions& options : AllOptions()) {
      INFO(fixture.name << ", include_deleted " << options.include_deleted
                        << ", recover_errors " << options.recover_errors);
      const WalkResult walk = CheckSameWalk<S>(fixture.build(), options);
      if (fixture.lists_entries) {
        CHECK_FALSE(walk.entries.empty());
      }
    }
  }
}

TEMPLATE_TEST_CASE_SIG("The batched walk gives every standing of a record",
                       "[file-record][traverse-standing]",
                       ((Cache::Strategy S), S), Cache::Strategy::NoCache,
                       Cache::Strategy::FullCache) {
  const WalkResult walk = CheckSameWalk<S>(
      Tests::BuildFakeNtfsImageWithIndexedMftScan(scan_records),
      {.include_deleted = true, .recover_errors = true});

  for (const WalkedEntry& entry : walk.entries) {
    INFO(entry.reference << " " << entry.sequence);
    CHECK(entry.standing == ExpectedStanding(entry));
  }
  for (const RecordStanding standing :
       {RecordStanding::Live, RecordStanding::Deleted, RecordStanding::Reused,
        RecordStanding::Unreadable}) {
    CHECK(std::ranges::any_of(walk.entries, [&](const WalkedEntry& entry) {
      return entry.standing == standing;
    }));
  }
}

TEMPLATE_TEST_CASE_SIG(
    "The batched walk hides the entries the default walk hides",
    "[file-record][traverse-standing]", ((Cache::Strategy S), S),
    Cache::Strategy::NoCache, Cache::Strategy::FullCache) {
  const std::vector<BYTE> image =
      Tests::BuildFakeNtfsImageWithIndexedMftScan(scan_records);

  const WalkResult strict = CheckSameWalk<S>(image, {});
  for (const WalkedEntry& entry : strict.entries) {
    CHECK(entry.standing == RecordStanding::Live);
  }

  const WalkResult deleted = CheckSameWalk<S>(image, {.include_deleted = true});
  CHECK(deleted.entries.size() > strict.entries.size());
  CHECK(std::ranges::none_of(deleted.entries, [](const WalkedEntry& entry) {
    return entry.standing == RecordStanding::Unreadable;
  }));
}

TEMPLATE_TEST_CASE_SIG(
    "The batched walk logs a probe at the entry that names the record",
    "[file-record][traverse-standing]", ((Cache::Strategy S), S),
    Cache::Strategy::NoCache, Cache::Strategy::FullCache) {
  const WalkResult walk = CheckSameWalk<S>(
      Tests::BuildFakeNtfsImageWithIndexedMftScan(scan_records),
      {.include_deleted = true});

  CHECK_THAT(walk.log, ContainsSubstring("does not parse"));
  CHECK_THAT(walk.log, ContainsSubstring("File Record 17 Found"));
}

TEMPLATE_TEST_CASE_SIG(
    "The batched walk probes a record once, however many entries name it",
    "[file-record][traverse-standing]", ((Cache::Strategy S), S),
    Cache::Strategy::NoCache, Cache::Strategy::FullCache) {
  const std::vector<BYTE> image =
      Tests::BuildFakeNtfsImageWithIndexedMftScan(scan_records);
  const RootWalk plain = WalkRootOnly<S>(image, false);
  const RootWalk batched = WalkRootOnly<S>(image, true);

  for (ULONGLONG idx = static_cast<ULONGLONG>(Mft::Idx::User);
       idx < scan_records; idx++) {
    // A slot that fails its checks leaves no line, and no other does.
    if (idx % slot_kinds == zeroed_kind || idx % slot_kinds == 6) {
      continue;
    }
    INFO("slot " << idx);
    const std::string found = "File Record " + std::to_string(idx) + " Found";
    // One entry names the record, plus the alias and the stale entry.
    const size_t names = 1 +
                         (idx % Tests::indexed_scan_alias_every == 0 ? 1 : 0) +
                         (idx % Tests::indexed_scan_stale_every == 0 ? 1 : 0);
    CHECK(Occurrences(plain.log, found) == names);
    CHECK(Occurrences(batched.log, found) == names);
  }
}

TEST_CASE("The batched walk reads a record once however many entries name it",
          "[file-record][traverse-standing]") {
  const std::vector<BYTE> image =
      Tests::BuildFakeNtfsImageWithIndexedMftScan(scan_records);
  const RootWalk plain = WalkRootOnly<Cache::Strategy::NoCache>(image, false);
  const RootWalk batched = WalkRootOnly<Cache::Strategy::NoCache>(image, true);

  const ULONGLONG first_slot =
      SlotOffset(static_cast<ULONGLONG>(Mft::Idx::User));
  const ULONGLONG last_slot = SlotOffset(scan_records);
  const auto probe_reads = [&](const RootWalk& walk) {
    return static_cast<size_t>(std::ranges::count_if(
        walk.calls, [&](const CountingDiskReader::Call& call) {
          return call.kind == CountingDiskReader::Kind::Read &&
                 call.offset >= first_slot && call.offset < last_slot;
        }));
  };
  // The default walk reads a record for each entry. The batched one reads
  // neighbouring records together.
  CHECK(probe_reads(plain) >=
        scan_records - static_cast<ULONGLONG>(Mft::Idx::User));
  CHECK(probe_reads(batched) < probe_reads(plain) / 2);
  CHECK(ReadsIn(batched.calls) < ReadsIn(plain.calls));
}

TEST_CASE(
    "The batched walk over a block cache reads no more than the default one",
    "[file-record][traverse-standing]") {
  const std::vector<BYTE> image =
      Tests::BuildFakeNtfsImageWithIndexedMftScan(scan_records);
  CHECK(ReadsIn(WalkRootOnly<Cache::Strategy::FullCache>(image, true).calls) <=
        ReadsIn(WalkRootOnly<Cache::Strategy::FullCache>(image, false).calls));
}

TEST_CASE("The batched walk reads the records in $MFT order and announces them",
          "[file-record][traverse-standing]") {
  const auto calls =
      WalkRootOnly<Cache::Strategy::NoCache>(
          Tests::BuildFakeNtfsImageWithIndexedMftScan(scan_records), true)
          .calls;

  const ULONGLONG first_slot =
      SlotOffset(static_cast<ULONGLONG>(Mft::Idx::User));
  const ULONGLONG last_slot = SlotOffset(scan_records);
  const auto is_probe = [&](const CountingDiskReader::Call& call) {
    return call.kind == CountingDiskReader::Kind::Read &&
           call.offset >= first_slot && call.offset < last_slot;
  };

  // The reads between two withdrawals are those of one group.
  std::vector<std::vector<CountingDiskReader::Call>> groups(1);
  std::vector<CountingDiskReader::Call> announced;
  ULONGLONG longest_read = 0;
  for (const CountingDiskReader::Call& call : calls) {
    if (call.kind == CountingDiskReader::Kind::Prefetch) {
      announced.push_back(call);
    } else if (call.kind == CountingDiskReader::Kind::CancelPrefetch) {
      groups.emplace_back();
    } else if (is_probe(call)) {
      groups.back().push_back(call);
      longest_read = (std::max)(longest_read, call.length);
      // An earlier hint covers the read.
      CHECK(std::ranges::any_of(
          announced, [&](const CountingDiskReader::Call& hint) {
            return hint.offset <= call.offset &&
                   call.offset + call.length <= hint.offset + hint.length;
          }));
    }
  }
  size_t widest_group = 0;
  for (const auto& group : groups) {
    widest_group = (std::max)(widest_group, group.size());
    CHECK(std::ranges::is_sorted(group, {}, &CountingDiskReader::Call::offset));
  }
  CHECK(widest_group > 1);
  // Neighbouring records come in one read.
  CHECK(longest_read > Tests::fake_file_record_size);
  // Nothing is left announced: a withdrawal follows the last hint.
  const auto last_hint = std::ranges::find_if(
      calls.rbegin(), calls.rend(), [](const CountingDiskReader::Call& call) {
        return call.kind != CountingDiskReader::Kind::Read;
      });
  REQUIRE(last_hint != calls.rend());
  CHECK(last_hint->kind == CountingDiskReader::Kind::CancelPrefetch);
}

TEMPLATE_TEST_CASE_SIG(
    "The batched walk gives a directory of many index blocks",
    "[file-record][traverse-standing]", ((Cache::Strategy S), S),
    Cache::Strategy::NoCache, Cache::Strategy::FullCache) {
  const WalkResult walk =
      CheckSameWalk<S>(Tests::BuildFakeNtfsImageWithIndexedMftScan(150, 4),
                       {.include_deleted = true, .recover_errors = true});

  // Every slot past the system files, and the aliases and stale entries.
  CHECK(walk.entries.size() > 150 - static_cast<ULONGLONG>(Mft::Idx::User));
}
