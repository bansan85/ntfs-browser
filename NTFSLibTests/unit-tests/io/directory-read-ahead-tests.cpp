#include <ntfs-browser/win-types.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include <catch2/catch_message.hpp>
#include <catch2/catch_template_test_macros.hpp>
#include <catch2/catch_test_macros.hpp>

#include <ntfs-browser/cache/strategy.h>
#include <ntfs-browser/namespace.h>
#include <ntfs-browser/test/disk-readers/counting-disk-reader.h>
#include <ntfs-browser/test/fake-image/fake-ntfs-image.h>
#include <ntfs-browser/tuning/tuning.h>
#include <ntfs-browser/volume-options.h>

#include "directory-read-ahead-support.h"

namespace Cache = NTFS_BROWSER_NAMESPACE::Cache;
namespace Tests = NTFS_BROWSER_NAMESPACE::Tests;
namespace Tuning = NTFS_BROWSER_NAMESPACE::Tuning;
namespace ReadAhead = NTFS_BROWSER_NAMESPACE::Tests::ReadAhead;
using NTFS_BROWSER_NAMESPACE::VolumeOptions;
using Tests::CountingDiskReader;

namespace {

// Values of walk.read_ahead_dirs the listing is compared on.
constexpr std::size_t window_sizes[] = {1, 2, 8, 64};

// A budget of walk.read_ahead_bytes small enough to stop the planning midway:
// four records of the fake volume, and not one block more.
constexpr std::uint64_t small_budget = 4096;

// A budget that never binds.
constexpr std::uint64_t large_budget = std::uint64_t{16} << 20U;

// Values of walk.record_window_bytes the listing is compared on: too small to
// hold a run, one run, a few, and the default.
constexpr std::uint64_t window_bytes[] = {1024, 4096, 65536,
                                          std::uint64_t{4} << 20U};

// A sector of $MFT may be read once more for each round of the read ahead than
// the listing without it reads it: a run reads across the holes between the
// records it plans, and the holes of one round are the records of another.
// Every other sector is read no more often.

// How many rounds of reads end by withdrawing what nothing used.
std::size_t Rounds(const std::vector<CountingDiskReader::Call>& calls) {
  return static_cast<std::size_t>(
      std::ranges::count_if(calls, [](const CountingDiskReader::Call& call) {
        return call.kind == CountingDiskReader::Kind::CancelPrefetch;
      }));
}

// How many more rounds of reads ended by a withdrawal in on than in off.
std::size_t ExtraRounds(const std::vector<CountingDiskReader::Call>& on,
                        const std::vector<CountingDiskReader::Call>& off) {
  return Rounds(on) > Rounds(off) ? Rounds(on) - Rounds(off) : 0;
}

// The sectors that with reads more often than without. A sector of $MFT may be
// read extra_reads_in_mft times more.
std::vector<std::size_t>
    SectorsReadTooOften(const std::vector<std::uint32_t>& with,
                        const std::vector<std::uint32_t>& without,
                        const ReadAhead::MftSpan& mft,
                        std::size_t extra_reads_in_mft) {
  std::vector<std::size_t> read_too_often;
  for (std::size_t sector = 0; sector < with.size(); sector++) {
    const ULONGLONG at = sector * 512U;
    const bool in_mft = at >= mft.first && at < mft.end;
    const std::size_t allowed =
        without.at(sector) + (in_mft ? extra_reads_in_mft : 0U);
    if (with.at(sector) > allowed) {
      read_too_often.push_back(sector);
    }
  }
  return read_too_often;
}

}  // namespace

TEMPLATE_TEST_CASE_SIG(
    "A recursive listing reads the same with the read ahead absent or 0",
    "[io][directory-read-ahead]", ((Cache::Strategy S), S),
    Cache::Strategy::NoCache, Cache::Strategy::FullCache) {
  for (const ReadAhead::TreeFixture& fixture : ReadAhead::Fixtures()) {
    for (const VolumeOptions& options : ReadAhead::AllOptions()) {
      INFO(fixture.name << ", include_deleted " << options.include_deleted
                        << ", recover_errors " << options.recover_errors);
      const ReadAhead::Run absent =
          ReadAhead::ListRecursively<S>(fixture.image, options, 0, 0);
      VolumeOptions explicit_off = options;
      explicit_off.tuning.walk.read_ahead_dirs = 0;
      const ReadAhead::Run off = ReadAhead::ListRecursively<S>(
          fixture.image, explicit_off, 0, large_budget);

      CHECK(off.exit_code == absent.exit_code);
      CHECK(off.text == absent.text);
      CHECK(off.log == absent.log);
      REQUIRE(off.calls.size() == absent.calls.size());
      for (std::size_t index = 0; index < off.calls.size(); index++) {
        CHECK(off.calls.at(index).kind == absent.calls.at(index).kind);
        CHECK(off.calls.at(index).offset == absent.calls.at(index).offset);
        CHECK(off.calls.at(index).length == absent.calls.at(index).length);
      }
    }
  }
}

TEMPLATE_TEST_CASE_SIG("A recursive listing is the same with the read ahead on",
                       "[io][directory-read-ahead]", ((Cache::Strategy S), S),
                       Cache::Strategy::NoCache, Cache::Strategy::FullCache) {
  for (const ReadAhead::TreeFixture& fixture : ReadAhead::Fixtures()) {
    for (const VolumeOptions& options : ReadAhead::AllOptions()) {
      const ReadAhead::Run off =
          ReadAhead::ListRecursively<S>(fixture.image, options, 0, 0);
      CHECK(off.exit_code == 0);
      CHECK_FALSE(off.text.empty());

      std::vector<std::pair<std::size_t, std::uint64_t>> settings;
      for (const std::size_t dirs : window_sizes) {
        settings.emplace_back(dirs, large_budget);
      }
      settings.emplace_back(64, small_budget);
      settings.emplace_back(8, 0);
      for (const auto& [dirs, bytes] : settings) {
        INFO(fixture.name << ", include_deleted " << options.include_deleted
                          << ", recover_errors " << options.recover_errors
                          << ", " << dirs << " directories, " << bytes
                          << " bytes");
        const ReadAhead::Run on =
            ReadAhead::ListRecursively<S>(fixture.image, options, dirs, bytes);

        CHECK(on.exit_code == off.exit_code);
        CHECK(on.text == off.text);
        INFO(ReadAhead::FirstDifference(on.log, off.log));
        CHECK(on.log == off.log);
      }
    }
  }
}

TEMPLATE_TEST_CASE_SIG(
    "A recursive listing with the read ahead on reads nothing twice that it "
    "reads once without",
    "[io][directory-read-ahead]", ((Cache::Strategy S), S),
    Cache::Strategy::NoCache, Cache::Strategy::FullCache) {
  for (const ReadAhead::TreeFixture& fixture : ReadAhead::Fixtures()) {
    const ReadAhead::MftSpan mft = ReadAhead::MftSpanOf(fixture.image);
    for (const VolumeOptions& options : ReadAhead::AllOptions()) {
      const ReadAhead::Run off =
          ReadAhead::ListRecursively<S>(fixture.image, options, 0, 0);
      const std::vector<std::uint32_t> without =
          ReadAhead::SectorReadCounts(off.calls, fixture.image.size());

      for (const std::size_t dirs : window_sizes) {
        INFO(fixture.name << ", include_deleted " << options.include_deleted
                          << ", recover_errors " << options.recover_errors
                          << ", " << dirs << " directories");
        const ReadAhead::Run on = ReadAhead::ListRecursively<S>(
            fixture.image, options, dirs, large_budget);
        const std::vector<std::uint32_t> with =
            ReadAhead::SectorReadCounts(on.calls, fixture.image.size());

        REQUIRE(with.size() == without.size());
        const std::vector<std::size_t> read_too_often = SectorsReadTooOften(
            with, without, mft, ExtraRounds(on.calls, off.calls));
        const std::size_t first_wrong =
            read_too_often.empty() ? 0 : read_too_often.front();
        INFO("first sector read too often: "
             << first_wrong << ", " << with.at(first_wrong) << " times against "
             << without.at(first_wrong) << ", $MFT from " << mft.first / 512
             << " to " << mft.end / 512);
        CHECK(read_too_often.empty());
      }
    }
  }
}

TEMPLATE_TEST_CASE_SIG(
    "A recursive listing reads ahead 64 directories when no tuning is given",
    "[io][directory-read-ahead]", ((Cache::Strategy S), S),
    Cache::Strategy::NoCache, Cache::Strategy::FullCache) {
  const Tuning::Settings defaults;
  REQUIRE(defaults.walk.read_ahead_dirs == 64);
  for (const ReadAhead::TreeFixture& fixture : ReadAhead::Fixtures()) {
    for (const VolumeOptions& options : ReadAhead::AllOptions()) {
      INFO(fixture.name << ", include_deleted " << options.include_deleted
                        << ", recover_errors " << options.recover_errors);
      const ReadAhead::Run given =
          ReadAhead::ListWithDefaultTuning<S>(fixture.image, options);
      const ReadAhead::Run explicit_on = ReadAhead::ListRecursively<S>(
          fixture.image, options, defaults.walk.read_ahead_dirs,
          defaults.walk.read_ahead_bytes);
      const ReadAhead::Run off =
          ReadAhead::ListRecursively<S>(fixture.image, options, 0, 0);

      // The default is the explicit 64: the same reads, in the same order.
      CHECK(given.exit_code == explicit_on.exit_code);
      CHECK(given.text == explicit_on.text);
      CHECK(given.log == explicit_on.log);
      REQUIRE(given.calls.size() == explicit_on.calls.size());
      for (std::size_t index = 0; index < given.calls.size(); index++) {
        CHECK(given.calls.at(index).kind == explicit_on.calls.at(index).kind);
        CHECK(given.calls.at(index).offset ==
              explicit_on.calls.at(index).offset);
        CHECK(given.calls.at(index).length ==
              explicit_on.calls.at(index).length);
      }

      // And what it lists is what the listing without it lists.
      CHECK(given.exit_code == off.exit_code);
      CHECK(given.text == off.text);
      CHECK(given.log == off.log);
    }
  }
}

TEST_CASE("The default read ahead plans rounds of reads on a wide tree",
          "[io][directory-read-ahead]") {
  const std::vector<BYTE> image =
      Tests::BuildFakeNtfsImageWithNameTree(ReadAhead::WideTree(40));
  // The window of $MFT and the memo spare the probes their rounds: leave them
  // out.
  VolumeOptions options;
  options.tuning.walk.record_window_bytes = 0;
  options.tuning.walk.record_memo_entries = 0;
  const ReadAhead::Run given =
      ReadAhead::ListWithDefaultTuning<Cache::Strategy::NoCache>(image,
                                                                 options);
  const ReadAhead::Run off =
      ReadAhead::ListRecursively<Cache::Strategy::NoCache>(image, options, 0,
                                                           0);
  CHECK(Rounds(given.calls) > Rounds(off.calls));
}

TEMPLATE_TEST_CASE_SIG(
    "A recursive listing is the same with a record window of any size",
    "[io][directory-read-ahead]", ((Cache::Strategy S), S),
    Cache::Strategy::NoCache, Cache::Strategy::FullCache) {
  for (const ReadAhead::TreeFixture& fixture : ReadAhead::Fixtures()) {
    for (const VolumeOptions& options : ReadAhead::AllOptions()) {
      VolumeOptions without = options;
      without.tuning.walk.record_window_bytes = 0;
      const ReadAhead::Run off = ReadAhead::ListRecursively<S>(
          fixture.image, without, 64, large_budget);

      for (const std::uint64_t bytes : window_bytes) {
        INFO(fixture.name << ", include_deleted " << options.include_deleted
                          << ", recover_errors " << options.recover_errors
                          << ", window " << bytes << " bytes");
        VolumeOptions with = options;
        with.tuning.walk.record_window_bytes = bytes;
        const ReadAhead::Run on = ReadAhead::ListRecursively<S>(
            fixture.image, with, 64, large_budget);

        CHECK(on.exit_code == off.exit_code);
        CHECK(on.text == off.text);
        INFO(ReadAhead::FirstDifference(on.log, off.log));
        CHECK(on.log == off.log);
      }
    }
  }
}

TEST_CASE("A block cache reads the same whatever the record window",
          "[io][directory-read-ahead]") {
  constexpr Cache::Strategy S = Cache::Strategy::FullCache;
  for (const ReadAhead::TreeFixture& fixture : ReadAhead::Fixtures()) {
    VolumeOptions without;
    without.tuning.walk.record_window_bytes = 0;
    const ReadAhead::Run off =
        ReadAhead::ListRecursively<S>(fixture.image, without, 64, large_budget);
    const ReadAhead::Run on =
        ReadAhead::ListWithDefaultTuning<S>(fixture.image, VolumeOptions{});

    INFO(fixture.name);
    REQUIRE(on.calls.size() == off.calls.size());
    for (std::size_t index = 0; index < on.calls.size(); index++) {
      CHECK(on.calls.at(index).kind == off.calls.at(index).kind);
      CHECK(on.calls.at(index).offset == off.calls.at(index).offset);
      CHECK(on.calls.at(index).length == off.calls.at(index).length);
    }
  }
}

TEST_CASE(
    "The record window spares a listing without a cache its reads of $MFT",
    "[io][directory-read-ahead]") {
  constexpr Cache::Strategy S = Cache::Strategy::NoCache;
  const std::vector<std::vector<BYTE>> images{
      Tests::BuildFakeNtfsImageWithNameTree(ReadAhead::WideTree(40)),
      Tests::BuildFakeNtfsImageWithNameTree(ReadAhead::GroupedTree(4, 12))};
  for (const std::vector<BYTE>& image : images) {
    VolumeOptions without;
    without.tuning.walk.record_window_bytes = 0;
    const ReadAhead::Run off =
        ReadAhead::ListRecursively<S>(image, without, 64, large_budget);
    const ReadAhead::Run on =
        ReadAhead::ListWithDefaultTuning<S>(image, VolumeOptions{});

    const auto reads = [](const ReadAhead::Run& run) {
      return std::ranges::count_if(
          run.calls, [](const CountingDiskReader::Call& call) {
            return call.kind == CountingDiskReader::Kind::Read;
          });
    };
    CAPTURE(reads(off), reads(on));
    CHECK(on.text == off.text);
    CHECK(reads(on) < reads(off));
  }
}

TEMPLATE_TEST_CASE_SIG(
    "A recursive listing is the same with a record memo of any size",
    "[io][directory-read-ahead]", ((Cache::Strategy S), S),
    Cache::Strategy::NoCache, Cache::Strategy::FullCache) {
  for (const ReadAhead::TreeFixture& fixture : ReadAhead::Fixtures()) {
    for (VolumeOptions options : ReadAhead::AllOptions()) {
      // The window would serve the probes first: the memo serves them alone.
      options.tuning.walk.record_window_bytes = 0;
      VolumeOptions without = options;
      without.tuning.walk.record_memo_entries = 0;
      const ReadAhead::Run off = ReadAhead::ListRecursively<S>(
          fixture.image, without, 64, large_budget);

      // One place, a few that collide, and the default.
      for (const std::uint64_t entries : {1, 7, 64, 65536}) {
        INFO(fixture.name << ", include_deleted " << options.include_deleted
                          << ", recover_errors " << options.recover_errors
                          << ", memo " << entries << " entries");
        VolumeOptions with = options;
        with.tuning.walk.record_memo_entries = entries;
        const ReadAhead::Run on = ReadAhead::ListRecursively<S>(
            fixture.image, with, 64, large_budget);

        CHECK(on.exit_code == off.exit_code);
        CHECK(on.text == off.text);
        INFO(ReadAhead::FirstDifference(on.log, off.log));
        CHECK(on.log == off.log);
      }
    }
  }
}

TEST_CASE("A block cache reads the same whatever the record memo",
          "[io][directory-read-ahead]") {
  constexpr Cache::Strategy S = Cache::Strategy::FullCache;
  for (const ReadAhead::TreeFixture& fixture : ReadAhead::Fixtures()) {
    VolumeOptions without;
    without.tuning.walk.record_memo_entries = 0;
    const ReadAhead::Run off =
        ReadAhead::ListRecursively<S>(fixture.image, without, 64, large_budget);
    const ReadAhead::Run on =
        ReadAhead::ListWithDefaultTuning<S>(fixture.image, VolumeOptions{});

    INFO(fixture.name);
    REQUIRE(on.calls.size() == off.calls.size());
    for (std::size_t index = 0; index < on.calls.size(); index++) {
      CHECK(on.calls.at(index).kind == off.calls.at(index).kind);
      CHECK(on.calls.at(index).offset == off.calls.at(index).offset);
      CHECK(on.calls.at(index).length == off.calls.at(index).length);
    }
  }
}

TEST_CASE("The record memo spares a listing without a cache its reads of $MFT",
          "[io][directory-read-ahead]") {
  constexpr Cache::Strategy S = Cache::Strategy::NoCache;
  const std::vector<std::vector<BYTE>> images{
      Tests::BuildFakeNtfsImageWithNameTree(ReadAhead::WideTree(40)),
      Tests::BuildFakeNtfsImageWithNameTree(ReadAhead::GroupedTree(4, 12))};
  for (const std::vector<BYTE>& image : images) {
    // The window would hold the same runs: leave it out.
    VolumeOptions without;
    without.tuning.walk.record_window_bytes = 0;
    without.tuning.walk.record_memo_entries = 0;
    VolumeOptions with = without;
    with.tuning.walk.record_memo_entries = 65536;
    const ReadAhead::Run off =
        ReadAhead::ListRecursively<S>(image, without, 64, large_budget);
    const ReadAhead::Run on =
        ReadAhead::ListRecursively<S>(image, with, 64, large_budget);

    const auto reads = [](const ReadAhead::Run& run) {
      return std::ranges::count_if(
          run.calls, [](const CountingDiskReader::Call& call) {
            return call.kind == CountingDiskReader::Kind::Read;
          });
    };
    CAPTURE(reads(off), reads(on));
    CHECK(on.text == off.text);
    CHECK(on.log == off.log);
    CHECK(reads(on) < reads(off));
  }
}
