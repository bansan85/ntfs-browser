#include <ntfs-browser/win-types.h>

#include <algorithm>
#include <cstddef>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include <ntfs-browser/cache/strategy.h>
#include <ntfs-browser/mft/tree.h>
#include <ntfs-browser/namespace.h>
#include <ntfs-browser/usn/types.h>
#include <ntfs-browser/volume-options.h>

#include "counting-disk-reader.h"
#include "fake-ntfs-image.h"
#include "usn-journal-record.h"
#include "usn-test-support.h"

namespace Cache = NTFS_BROWSER_NAMESPACE::Cache;
namespace Mft = NTFS_BROWSER_NAMESPACE::Mft;
namespace Tests = NTFS_BROWSER_NAMESPACE::Tests;
namespace Support = NTFS_BROWSER_NAMESPACE::Tests::UsnSupport;
using NTFS_BROWSER_NAMESPACE::VolumeOptions;
using Support::MakeRecord;
using Tests::CountingDiskReader;
using Tests::FakeUsnOptions;

namespace {

using Call = CountingDiskReader::Call;
using Kind = CountingDiskReader::Kind;

// Where the data of $J starts in the fixture: it goes past the end of the base
// image.
ULONGLONG JournalStart() {
  return Tests::BuildFakeNtfsImageWithFreedFileTree().size();
}

// The calls of a kind that touch the data of $J.
std::vector<Call> JournalCalls(const std::vector<Call>& calls, Kind kind) {
  std::vector<Call> found;
  for (const Call& call : calls) {
    if (call.kind == kind && call.offset >= JournalStart()) {
      found.push_back(call);
    }
  }
  return found;
}

// A journal of a few records.
FakeUsnOptions SmallJournal() {
  return {.records = {MakeRecord({.name = L"a.txt", .record = 40}),
                      MakeRecord({.name = L"b.txt", .record = 41})}};
}

// A journal of zero bytes that spans several announced ranges.
FakeUsnOptions LargeJournal() {
  constexpr size_t nine_mib = size_t{9} * 1024 * 1024;
  return {.raw_stream = std::vector<BYTE>(nine_mib, BYTE{0})};
}

// Whether every read of $J lies in a range announced before it.
bool EveryReadIsAnnouncedBefore(const std::vector<Call>& calls) {
  std::vector<Call> hints;
  for (const Call& call : calls) {
    if (call.kind == Kind::Prefetch && call.offset >= JournalStart()) {
      hints.push_back(call);
    }
    if (call.kind != Kind::Read || call.offset < JournalStart()) {
      continue;
    }
    const bool held = std::ranges::any_of(hints, [&call](const Call& hint) {
      return hint.offset <= call.offset &&
             call.offset + call.length <= hint.offset + hint.length;
    });
    if (!held) {
      return false;
    }
  }
  return true;
}

// The (offset, length) of the reads of $J when only the loader reads them.
std::vector<std::pair<ULONGLONG, ULONGLONG>>
    LoaderReads(const FakeUsnOptions& options) {
  const auto opened = Support::OpenVolume<Cache::Strategy::NoCache>(
      Support::JournalImage(options));
  opened.counter->Reset();
  REQUIRE(opened.volume->GetUsnJournal() != nullptr);
  std::vector<std::pair<ULONGLONG, ULONGLONG>> reads;
  for (const Call& call : JournalCalls(opened.counter->Calls(), Kind::Read)) {
    reads.emplace_back(call.offset, call.length);
  }
  return reads;
}

}  // namespace

TEST_CASE("a scan announces the data of $J before the journal is read",
          "[usn][tree][prefetch]") {
  for (const FakeUsnOptions& options : {SmallJournal(), LargeJournal()}) {
    const auto opened = Support::OpenVolume<Cache::Strategy::NoCache>(
        Support::JournalImage(options));
    const Mft::MftTree tree(*opened.volume);
    const std::vector<Call>& calls = opened.counter->Calls();

    const std::vector<Call> hints = JournalCalls(calls, Kind::Prefetch);
    CHECK_FALSE(hints.empty());
    CHECK_FALSE(JournalCalls(calls, Kind::Read).empty());
    CHECK(EveryReadIsAnnouncedBefore(calls));
  }
}

TEST_CASE("the announced ranges of $J are at most 4 MiB each",
          "[usn][tree][prefetch]") {
  const auto opened = Support::OpenVolume<Cache::Strategy::NoCache>(
      Support::JournalImage(LargeJournal()));
  const Mft::MftTree tree(*opened.volume);

  const std::vector<Call> hints =
      JournalCalls(opened.counter->Calls(), Kind::Prefetch);
  CHECK(hints.size() >= 3);
  for (const Call& hint : hints) {
    CHECK(hint.length <= ULONGLONG{4} * 1024 * 1024);
  }
}

TEST_CASE("announcing $J changes none of the reads that load it",
          "[usn][tree][prefetch]") {
  for (const FakeUsnOptions& options : {SmallJournal(), LargeJournal()}) {
    const auto opened = Support::OpenVolume<Cache::Strategy::NoCache>(
        Support::JournalImage(options));
    const Mft::MftTree tree(*opened.volume);

    std::vector<std::pair<ULONGLONG, ULONGLONG>> scanned;
    for (const Call& call : JournalCalls(opened.counter->Calls(), Kind::Read)) {
      scanned.emplace_back(call.offset, call.length);
    }
    CHECK(scanned == LoaderReads(options));
  }
}

TEST_CASE("a scan that does not recover announces nothing of $J",
          "[usn][tree][prefetch]") {
  const auto opened = Support::OpenVolume<Cache::Strategy::NoCache>(
      Support::JournalImage(SmallJournal()), VolumeOptions{});
  const Mft::MftTree tree(*opened.volume);

  CHECK(JournalCalls(opened.counter->Calls(), Kind::Prefetch).empty());
  CHECK(JournalCalls(opened.counter->Calls(), Kind::Read).empty());
}

TEST_CASE("a scan that progress() stopped announces nothing of $J",
          "[usn][tree][prefetch]") {
  const auto opened = Support::OpenVolume<Cache::Strategy::NoCache>(
      Support::JournalImage(SmallJournal()));
  Mft::MftTree::ScanOptions scan;
  scan.progress = [](ULONGLONG /*done*/, ULONGLONG /*total*/) { return false; };
  const Mft::MftTree tree(*opened.volume, scan);

  CHECK(JournalCalls(opened.counter->Calls(), Kind::Prefetch).empty());
}
