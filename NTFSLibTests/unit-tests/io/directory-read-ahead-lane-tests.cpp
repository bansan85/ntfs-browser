#include <ntfs-browser/win-types.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <memory>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include <catch2/catch_message.hpp>
#include <catch2/catch_template_test_macros.hpp>
#include <catch2/catch_test_macros.hpp>

#include <ntfs-browser/attr/mask.h>
#include <ntfs-browser/attr/type.h>
#include <ntfs-browser/cache/strategy.h>
#include <ntfs-browser/index-entry.h>
#include <ntfs-browser/io/directory-read-ahead.h>
#include <ntfs-browser/io/file-record.h>
#include <ntfs-browser/mft/idx.h>
#include <ntfs-browser/namespace.h>
#include <ntfs-browser/ntfs-volume.h>
#include <ntfs-browser/test/disk-readers/counting-disk-reader.h>
#include <ntfs-browser/test/disk-readers/memory-disk-reader.h>
#include <ntfs-browser/test/fake-image/fake-ntfs-image.h>
#include <ntfs-browser/test/support/test-log-sink.h>
#include <ntfs-browser/tuning/tuning.h>
#include <ntfs-browser/volume-options.h>

#include "directory-read-ahead-support.h"

namespace NTFS_BROWSER_NAMESPACE {
namespace Attr {

struct HeaderCommon;

}  // namespace Attr

namespace Io {

enum class RecordStanding : std::uint8_t;
template <Cache::Strategy S>
class FileRecord;

}  // namespace Io
}  // namespace NTFS_BROWSER_NAMESPACE

namespace Attr = NTFS_BROWSER_NAMESPACE::Attr;
namespace Cache = NTFS_BROWSER_NAMESPACE::Cache;
namespace Io = NTFS_BROWSER_NAMESPACE::Io;
namespace Mft = NTFS_BROWSER_NAMESPACE::Mft;
namespace TestSupport = NTFS_BROWSER_NAMESPACE::Test::Support;
namespace TestDiskReaders = NTFS_BROWSER_NAMESPACE::Test::DiskReaders;
namespace TestFakeImage = NTFS_BROWSER_NAMESPACE::Test::FakeImage;
namespace Tuning = NTFS_BROWSER_NAMESPACE::Tuning;
namespace ReadAhead = NTFS_BROWSER_NAMESPACE::Tests::ReadAhead;
using NTFS_BROWSER_NAMESPACE::NtfsVolume;
using NTFS_BROWSER_NAMESPACE::VolumeOptions;
using TestDiskReaders::CountingDiskReader;
using Call = CountingDiskReader::Call;
using Kind = CountingDiskReader::Kind;

namespace {

// The tree the lane is looked at on: sibling directories of 6 files each, whose
// index fits the record. The probes of a directory read the 6 records that
// follow it, in one run.
constexpr std::size_t directories = 44;
constexpr std::size_t files_per_directory = 6;

// Records a directory and its files take.
constexpr ULONGLONG directory_stride = files_per_directory + 1;

// The directories the tests plan are this many apart: 70 records, more than
// the 64 that a run of $MFT reads across, so that the batch which reads them
// holds no record of their probes.
constexpr std::size_t planned_every = 10;

// How many of them a test plans.
constexpr std::size_t planned = 4;

// A budget that never binds.
constexpr std::uint64_t large_budget = std::uint64_t{16} << 20U;

// The record number of directory number index of SpacedTree().
ULONGLONG LaneDirectory(std::size_t index) {
  return TestFakeImage::name_tree_first_record + (index * directory_stride);
}

// The first count planned directories, the next one first.
std::vector<ULONGLONG> Upcoming(std::size_t count, bool reversed = false) {
  std::vector<ULONGLONG> records;
  for (std::size_t index = 0; index < count; index++) {
    records.push_back(LaneDirectory(index * planned_every));
  }
  if (reversed) {
    std::ranges::reverse(records);
  }
  return records;
}

// How many calls of kind there are.
std::size_t CountOf(const std::vector<Call>& calls, Kind kind) {
  return static_cast<std::size_t>(std::ranges::count_if(
      calls, [kind](const Call& call) { return call.kind == kind; }));
}

// The calls of one kind, in order.
std::vector<Call> CallsOf(const std::vector<Call>& calls, Kind kind) {
  std::vector<Call> kept;
  std::ranges::copy_if(calls, std::back_inserter(kept),
                       [kind](const Call& call) { return call.kind == kind; });
  return kept;
}

// Whether a call is made on the standing lane.
bool IsLane(const Call& call) {
  return call.kind == Kind::PrefetchStanding ||
         call.kind == Kind::CancelStanding;
}

// The calls but those of the standing lane.
std::vector<Call> WithoutLane(const std::vector<Call>& calls) {
  std::vector<Call> kept;
  std::ranges::copy_if(calls, std::back_inserter(kept),
                       [](const Call& call) { return !IsLane(call); });
  return kept;
}

// Whether the ranges of two calls share a byte.
bool Overlap(const Call& left, const Call& right) {
  return left.offset < right.offset + right.length &&
         right.offset < left.offset + left.length;
}

// What the standing lane of a reader holds after calls, as Win32DiskReader
// keeps it: a withdrawal takes every range that overlaps it, and a read inside
// a range that ends where the range ends uses the range up. violations counts
// the announcements made over a range that was already on the lane.
struct Lane {
  std::vector<Call> left;
  std::size_t violations{0};
};

Lane LaneAfter(const std::vector<Call>& calls) {
  Lane lane;
  for (const Call& call : calls) {
    if (call.kind == Kind::PrefetchStanding) {
      lane.violations += std::ranges::any_of(lane.left,
                                             [&call](const Call& range) {
                                               return Overlap(range, call);
                                             })
                             ? 1
                             : 0;
      lane.left.push_back(call);
    } else if (call.kind == Kind::CancelStanding) {
      std::erase_if(lane.left, [&call](const Call& range) {
        return Overlap(range, call);
      });
    } else if (call.kind == Kind::Read) {
      const auto held =
          std::ranges::find_if(lane.left, [&call](const Call& range) {
            return range.offset <= call.offset &&
                   call.offset + call.length <= range.offset + range.length;
          });
      if (held != lane.left.end() &&
          call.offset + call.length == held->offset + held->length) {
        lane.left.erase(held);
      }
    }
  }
  return lane;
}

// The ranges announced on the lane that nothing read or withdrew, by the end
// of calls.
std::vector<Call> LeftOnTheLane(const std::vector<Call>& calls) {
  return LaneAfter(calls).left;
}

// A volume over a counting reader, and a record that parses its root.
template <Cache::Strategy S>
struct Rig {
  CountingDiskReader* reads{nullptr};
  std::unique_ptr<NtfsVolume<S>> volume;
  std::unique_ptr<Io::FileRecord<S>> listing;

  Rig(const std::vector<BYTE>& image, VolumeOptions options,
      Tuning::MediaKind kind = Tuning::MediaKind::Unknown) {
    auto memory = std::make_unique<TestDiskReaders::MemoryDiskReader>(image);
    memory->SetMediaKind(kind);
    auto counting = std::make_unique<CountingDiskReader>(std::move(memory));
    reads = counting.get();
    volume = std::make_unique<NtfsVolume<S>>(std::move(counting), options);
    REQUIRE(volume->IsVolumeOK());
    listing = std::make_unique<Io::FileRecord<S>>(*volume);
    listing->SetAttrMask(Attr::Mask::IndexRoot | Attr::Mask::IndexAllocation);
    REQUIRE(listing->ParseFileRecord(static_cast<ULONGLONG>(Mft::Idx::Root)));
    REQUIRE(listing->ParseAttrs());
    reads->Reset();
    (void)TestSupport::TakeCapturedLog();
  }
};

// The options of a run: a window of 8 directories, and runs announced at most.
VolumeOptions WithRuns(std::size_t runs) {
  VolumeOptions options;
  options.tuning.walk.read_ahead_dirs = 8;
  options.tuning.walk.read_ahead_bytes = large_budget;
  options.tuning.walk.read_ahead_runs = runs;
  return options;
}

// Lists the directory the record holds, as a listing does.
template <Cache::Strategy S>
std::size_t ListEntries(Io::FileRecord<S>& listing) {
  std::size_t entries = 0;
  listing.TraverseSubEntriesWithStanding(
      [](const NTFS_BROWSER_NAMESPACE::IndexEntryView&, Io::RecordStanding,
         void* context) { ++*static_cast<std::size_t*>(context); },
      &entries);
  return entries;
}

// A callback that keeps every attribute: it only has to be there.
void KeepEveryAttribute(const Attr::HeaderCommon& /*header*/, bool& discard) {
  discard = false;
}

}  // namespace

TEMPLATE_TEST_CASE_SIG(
    "The read ahead announces the probe runs of the held directories on the "
    "standing lane",
    "[io][directory-read-ahead][standing-lane]", ((Cache::Strategy S), S),
    Cache::Strategy::NoCache, Cache::Strategy::FullCache) {
  const auto image = TestFakeImage::BuildFakeNtfsImageWithNameTree(
      ReadAhead::SpacedTree(directories, files_per_directory));
  Rig<S> rig(image, WithRuns(64));
  Io::DirectoryReadAhead<S> ahead(*rig.volume);
  ahead.Attach(*rig.listing);

  ahead.Plan(Upcoming(planned));

  const std::vector<Call> announced =
      CallsOf(rig.reads->Calls(), Kind::PrefetchStanding);
  const Io::DirectoryReadAheadStats stats = ahead.GetStats();
  CHECK(announced.size() == planned);
  CHECK(stats.runs_planned == planned);
  CHECK(stats.runs_announced == planned);
  CHECK(stats.runs_withdrawn == 0);
  // The runs follow the order of the directories.
  CHECK(std::ranges::is_sorted(announced,
                               [](const Call& left, const Call& right) {
                                 return left.offset < right.offset;
                               }));
  if constexpr (S == Cache::Strategy::NoCache) {
    // The records after the directory: its files.
    const ULONGLONG first =
        ReadAhead::MftSpanOf(image).first +
        ((LaneDirectory(0) + 1) * rig.volume->GetFileRecordSize());
    CHECK(announced.front().offset == first);
    CHECK(announced.front().length ==
          files_per_directory * rig.volume->GetFileRecordSize());
  }
}

TEMPLATE_TEST_CASE_SIG(
    "The read ahead announces the probe runs of the stack from the top down",
    "[io][directory-read-ahead][standing-lane]", ((Cache::Strategy S), S),
    Cache::Strategy::NoCache, Cache::Strategy::FullCache) {
  const auto image = TestFakeImage::BuildFakeNtfsImageWithNameTree(
      ReadAhead::SpacedTree(directories, files_per_directory));
  Rig<S> rig(image, WithRuns(64));
  Io::DirectoryReadAhead<S> ahead(*rig.volume);
  ahead.Attach(*rig.listing);

  ahead.Plan(Upcoming(planned, true));

  const std::vector<Call> announced =
      CallsOf(rig.reads->Calls(), Kind::PrefetchStanding);
  REQUIRE(announced.size() == planned);
  CHECK(std::ranges::is_sorted(announced,
                               [](const Call& left, const Call& right) {
                                 return left.offset > right.offset;
                               }));
}

TEMPLATE_TEST_CASE_SIG(
    "The read ahead keeps at most walk.read_ahead_runs runs on the lane",
    "[io][directory-read-ahead][standing-lane]", ((Cache::Strategy S), S),
    Cache::Strategy::NoCache, Cache::Strategy::FullCache) {
  const auto image = TestFakeImage::BuildFakeNtfsImageWithNameTree(
      ReadAhead::SpacedTree(directories, files_per_directory));
  Rig<S> rig(image, WithRuns(3));
  Io::DirectoryReadAhead<S> ahead(*rig.volume);
  ahead.Attach(*rig.listing);

  ahead.Plan(Upcoming(planned));

  CHECK(CountOf(rig.reads->Calls(), Kind::PrefetchStanding) == 3);
  CHECK(ahead.GetStats().runs_planned == planned);
  CHECK(ahead.GetStats().runs_announced == 3);
}

TEMPLATE_TEST_CASE_SIG(
    "The read ahead fills the lane again as the listing reads a run",
    "[io][directory-read-ahead][standing-lane]", ((Cache::Strategy S), S),
    Cache::Strategy::NoCache, Cache::Strategy::FullCache) {
  const auto image = TestFakeImage::BuildFakeNtfsImageWithNameTree(
      ReadAhead::SpacedTree(directories, files_per_directory));
  Rig<S> rig(image, WithRuns(3));
  Io::DirectoryReadAhead<S> ahead(*rig.volume);
  ahead.Attach(*rig.listing);
  ahead.Plan(Upcoming(planned));
  REQUIRE(CountOf(rig.reads->Calls(), Kind::PrefetchStanding) == 3);

  // The listing enters the first directory, and its probes read its run.
  REQUIRE(rig.listing->ParseFileRecord(LaneDirectory(0)));
  REQUIRE(rig.listing->ParseAttrs());
  CHECK(ListEntries(*rig.listing) > 0);

  const Io::DirectoryReadAheadStats stats = ahead.GetStats();
  CHECK(stats.runs_read == 1);
  CHECK(stats.runs_unplanned == 0);
  // The run that was read left the lane, and the next one came in.
  CHECK(CountOf(rig.reads->Calls(), Kind::PrefetchStanding) == 4);
  if constexpr (S == Cache::Strategy::NoCache) {
    CHECK(CountOf(rig.reads->Calls(), Kind::CancelStanding) == 0);
  }
  CHECK(stats.runs_announced == 4);
}

TEMPLATE_TEST_CASE_SIG(
    "The read ahead withdraws the runs of a directory that Release frees",
    "[io][directory-read-ahead][standing-lane]", ((Cache::Strategy S), S),
    Cache::Strategy::NoCache, Cache::Strategy::FullCache) {
  const auto image = TestFakeImage::BuildFakeNtfsImageWithNameTree(
      ReadAhead::SpacedTree(directories, files_per_directory));
  Rig<S> rig(image, WithRuns(64));
  Io::DirectoryReadAhead<S> ahead(*rig.volume);
  ahead.Attach(*rig.listing);
  ahead.Plan(Upcoming(planned));
  const std::vector<Call> announced =
      CallsOf(rig.reads->Calls(), Kind::PrefetchStanding);
  REQUIRE(announced.size() == planned);

  ahead.Release(LaneDirectory(2 * planned_every));

  const std::vector<Call> withdrawn =
      CallsOf(rig.reads->Calls(), Kind::CancelStanding);
  REQUIRE(withdrawn.size() == 1);
  // A FullCache widens the range to its blocks.
  CHECK(withdrawn.front().offset <= announced.at(2).offset);
  CHECK(withdrawn.front().offset + withdrawn.front().length >=
        announced.at(2).offset + announced.at(2).length);
  CHECK(ahead.GetStats().runs_withdrawn == 1);

  // A directory that is not held, or released twice, withdraws nothing.
  ahead.Release(LaneDirectory(2 * planned_every));
  ahead.Release(LaneDirectory(31));
  CHECK(CountOf(rig.reads->Calls(), Kind::CancelStanding) == 1);
}

TEMPLATE_TEST_CASE_SIG(
    "The read ahead withdraws every run left on the lane when it is destroyed",
    "[io][directory-read-ahead][standing-lane]", ((Cache::Strategy S), S),
    Cache::Strategy::NoCache, Cache::Strategy::FullCache) {
  const auto image = TestFakeImage::BuildFakeNtfsImageWithNameTree(
      ReadAhead::SpacedTree(directories, files_per_directory));
  Rig<S> rig(image, WithRuns(64));
  {
    Io::DirectoryReadAhead<S> ahead(*rig.volume);
    ahead.Attach(*rig.listing);
    ahead.Plan(Upcoming(planned));
    REQUIRE(CountOf(rig.reads->Calls(), Kind::PrefetchStanding) == planned);
    REQUIRE(CountOf(rig.reads->Calls(), Kind::CancelStanding) == 0);
  }

  CHECK(CountOf(rig.reads->Calls(), Kind::CancelStanding) == planned);
  CHECK(LeftOnTheLane(rig.reads->Calls()).empty());
}

TEMPLATE_TEST_CASE_SIG(
    "The read ahead announces the runs of the directory a listing starts at",
    "[io][directory-read-ahead][standing-lane]", ((Cache::Strategy S), S),
    Cache::Strategy::NoCache, Cache::Strategy::FullCache) {
  const auto image = TestFakeImage::BuildFakeNtfsImageWithNameTree(
      ReadAhead::SpacedTree(directories, files_per_directory));
  Rig<S> rig(image, WithRuns(64));
  Io::DirectoryReadAhead<S> ahead(*rig.volume);
  ahead.Attach(*rig.listing);

  ahead.Begin();

  // The root names the directories, in its index blocks: a run for each node.
  const std::size_t announced =
      CountOf(rig.reads->Calls(), Kind::PrefetchStanding);
  CHECK(announced > 0);
  CHECK(ahead.GetStats().runs_planned >= announced);
  ahead.Release(static_cast<ULONGLONG>(Mft::Idx::Root));
  CHECK(CountOf(rig.reads->Calls(), Kind::CancelStanding) == announced);
}

namespace {

// Whether a recursive listing of image announces ranges on the standing lane.
template <Cache::Strategy S>
bool UsesLane(const std::vector<BYTE>& image, const VolumeOptions& options,
              std::size_t dirs = 64,
              Tuning::MediaKind kind = Tuning::MediaKind::Unknown) {
  const ReadAhead::Run run =
      ReadAhead::ListRecursively<S>(image, options, dirs, large_budget, kind);
  return CountOf(run.calls, Kind::PrefetchStanding) > 0;
}

// Whether a recursive listing of image makes no call at all on the lane.
template <Cache::Strategy S>
bool LeavesLaneAlone(const std::vector<BYTE>& image,
                     const VolumeOptions& options, std::size_t dirs = 64,
                     Tuning::MediaKind kind = Tuning::MediaKind::Unknown) {
  const ReadAhead::Run run =
      ReadAhead::ListRecursively<S>(image, options, dirs, large_budget, kind);
  return std::ranges::none_of(run.calls, IsLane);
}

}  // namespace

TEMPLATE_TEST_CASE_SIG(
    "The read ahead calls the standing lane only when it is on",
    "[io][directory-read-ahead][standing-lane]", ((Cache::Strategy S), S),
    Cache::Strategy::NoCache, Cache::Strategy::FullCache) {
  const auto image = TestFakeImage::BuildFakeNtfsImageWithNameTree(
      ReadAhead::SpacedTree(directories, files_per_directory));

  SECTION("with the lane on, a device of an unknown kind uses it") {
    CHECK(UsesLane<S>(image, VolumeOptions{}));
  }

  SECTION("a solid state device uses it") {
    CHECK(
        UsesLane<S>(image, VolumeOptions{}, 64, Tuning::MediaKind::SolidState));
  }

  SECTION("walk.read_ahead_runs at 0 turns it off") {
    VolumeOptions options;
    options.tuning.walk.read_ahead_runs = 0;
    CHECK(LeavesLaneAlone<S>(image, options));
  }

  SECTION("walk.read_ahead_dirs at 0 turns everything off") {
    CHECK(LeavesLaneAlone<S>(image, VolumeOptions{}, 0));
  }

  SECTION("a rotational device does not use it by default") {
    CHECK(LeavesLaneAlone<S>(image, VolumeOptions{}, 64,
                             Tuning::MediaKind::Rotational));
  }

  SECTION(
      "a rotational device uses it when walk.read_ahead_runs_rotational "
      "is set") {
    VolumeOptions options;
    options.tuning.walk.read_ahead_runs_rotational = 16;
    CHECK(UsesLane<S>(image, options, 64, Tuning::MediaKind::Rotational));
  }

  SECTION("the rotational knob does not serve a device of another kind") {
    VolumeOptions options;
    options.tuning.walk.read_ahead_runs = 0;
    options.tuning.walk.read_ahead_runs_rotational = 16;
    CHECK(LeavesLaneAlone<S>(image, options));
  }

  SECTION("a raw attribute callback turns it off") {
    Rig<S> rig(image, WithRuns(64));
    REQUIRE(
        rig.volume->InstallAttrRawCB(Attr::Type::Data, &KeepEveryAttribute));
    Io::DirectoryReadAhead<S> ahead(*rig.volume);
    ahead.Attach(*rig.listing);
    ahead.Begin();
    ahead.Plan(Upcoming(planned));

    CHECK(std::ranges::none_of(rig.reads->Calls(), IsLane));
    CHECK(ahead.GetStats().runs_planned == 0);
  }
}

TEMPLATE_TEST_CASE_SIG(
    "A recursive listing is the same with the standing lane on and off",
    "[io][directory-read-ahead][standing-lane]", ((Cache::Strategy S), S),
    Cache::Strategy::NoCache, Cache::Strategy::FullCache) {
  for (const ReadAhead::TreeFixture& fixture : ReadAhead::Fixtures()) {
    for (const VolumeOptions& options : ReadAhead::AllOptions()) {
      INFO(fixture.name << ", include_deleted " << options.include_deleted
                        << ", recover_errors " << options.recover_errors);
      VolumeOptions without = options;
      without.tuning.walk.read_ahead_runs = 0;
      const ReadAhead::Run off = ReadAhead::ListRecursively<S>(
          fixture.image, without, 64, large_budget);
      const ReadAhead::Run on = ReadAhead::ListRecursively<S>(
          fixture.image, options, 64, large_budget);

      CHECK(std::ranges::none_of(off.calls, IsLane));
      CHECK(on.exit_code == off.exit_code);
      CHECK(on.text == off.text);
      INFO(ReadAhead::FirstDifference(on.log, off.log));
      CHECK(on.log == off.log);
      CHECK(on.recovered == off.recovered);
      // The lane only hints: every other call is the same, in order.
      const std::vector<Call> kept = WithoutLane(on.calls);
      REQUIRE(kept.size() == off.calls.size());
      for (std::size_t index = 0; index < kept.size(); index++) {
        CHECK(kept.at(index).kind == off.calls.at(index).kind);
        CHECK(kept.at(index).offset == off.calls.at(index).offset);
        CHECK(kept.at(index).length == off.calls.at(index).length);
      }
    }
  }
}

TEMPLATE_TEST_CASE_SIG(
    "Every range announced on the standing lane is read or withdrawn by the "
    "end of a listing",
    "[io][directory-read-ahead][standing-lane]", ((Cache::Strategy S), S),
    Cache::Strategy::NoCache, Cache::Strategy::FullCache) {
  for (const ReadAhead::TreeFixture& fixture : ReadAhead::Fixtures()) {
    for (const VolumeOptions& options : ReadAhead::AllOptions()) {
      INFO(fixture.name << ", include_deleted " << options.include_deleted
                        << ", recover_errors " << options.recover_errors);
      const ReadAhead::Run run = ReadAhead::ListRecursively<S>(
          fixture.image, options, 64, large_budget);
      CHECK(LeftOnTheLane(run.calls).empty());
    }
  }
}

TEMPLATE_TEST_CASE_SIG(
    "The runs announced on the standing lane are the runs the listing reads",
    "[io][directory-read-ahead][standing-lane]", ((Cache::Strategy S), S),
    Cache::Strategy::NoCache, Cache::Strategy::FullCache) {
  // The share of announced runs that no read of the listing took whole. A run
  // that the window made smaller, or a read that cut a run in two, is the
  // rare exception.
  constexpr std::size_t unread_percent_at_most = 10;

  const std::vector<ReadAhead::TreeFixture> trees = [] {
    std::vector<ReadAhead::TreeFixture> kept;
    kept.push_back(
        {"wide", TestFakeImage::BuildFakeNtfsImageWithNameTree(
                     ReadAhead::SpacedTree(directories, files_per_directory))});
    kept.push_back({"grouped", TestFakeImage::BuildFakeNtfsImageWithNameTree(
                                   ReadAhead::GroupedTree(4, 12))});
    kept.push_back(
        {"big directories", TestFakeImage::BuildFakeNtfsImageWithNameTree(
                                ReadAhead::BigDirectoryTree(6))});
    return kept;
  }();
  for (const ReadAhead::TreeFixture& tree : trees) {
    INFO(tree.name);
    const ReadAhead::Run run = ReadAhead::ListRecursively<S>(
        tree.image, VolumeOptions{}, 64, large_budget);
    const std::vector<Call> announced =
        CallsOf(run.calls, Kind::PrefetchStanding);
    const std::vector<Call> reads = CallsOf(run.calls, Kind::Read);
    REQUIRE_FALSE(announced.empty());

    std::size_t unread = 0;
    for (const Call& range : announced) {
      // A read of the whole range, or one that ends where the range ends: a
      // FullCache reads by blocks.
      const bool taken = std::ranges::any_of(reads, [&range](const Call& read) {
        const ULONGLONG end = range.offset + range.length;
        return read.offset + read.length == end ||
               (read.offset <= range.offset &&
                end <= read.offset + read.length);
      });
      unread += taken ? 0 : 1;
    }
    CAPTURE(announced.size(), unread);
    CHECK(unread * 100 <= announced.size() * unread_percent_at_most);
  }
}

TEMPLATE_TEST_CASE_SIG("The ranges on the standing lane never overlap",
                       "[io][directory-read-ahead][standing-lane]",
                       ((Cache::Strategy S), S), Cache::Strategy::NoCache,
                       Cache::Strategy::FullCache) {
  for (const ReadAhead::TreeFixture& fixture : ReadAhead::Fixtures()) {
    INFO(fixture.name);
    const ReadAhead::Run run = ReadAhead::ListRecursively<S>(
        fixture.image, VolumeOptions{}, 64, large_budget);
    CHECK(LaneAfter(run.calls).violations == 0);
  }
}

TEMPLATE_TEST_CASE_SIG(
    "A run that leaves the first runs stays on the lane among the next ones",
    "[io][directory-read-ahead][standing-lane]", ((Cache::Strategy S), S),
    Cache::Strategy::NoCache, Cache::Strategy::FullCache) {
  const auto image = TestFakeImage::BuildFakeNtfsImageWithNameTree(
      ReadAhead::SpacedTree(directories, files_per_directory));

  const ReadAhead::VerifiedLaneZone verified;

  SECTION("a stack that turns over withdraws nothing within the next runs") {
    Rig<S> rig(image, WithRuns(2));
    Io::DirectoryReadAhead<S> ahead(*rig.volume);
    ahead.Attach(*rig.listing);
    ahead.Plan(Upcoming(planned));
    REQUIRE(CountOf(rig.reads->Calls(), Kind::PrefetchStanding) == 2);

    // The directories that were last are next now: the two runs on the lane
    // are the third and the fourth in the new order.
    ahead.Plan(Upcoming(planned, true));

    CHECK(CountOf(rig.reads->Calls(), Kind::CancelStanding) == 0);
    CHECK(CountOf(rig.reads->Calls(), Kind::PrefetchStanding) == 4);
    CHECK(ahead.GetStats().runs_withdrawn == 0);
    CHECK(LeftOnTheLane(rig.reads->Calls()).size() == 4);
  }

  SECTION("a run past the next ones is withdrawn") {
    Rig<S> rig(image, WithRuns(1));
    Io::DirectoryReadAhead<S> ahead(*rig.volume);
    ahead.Attach(*rig.listing);
    ahead.Plan(Upcoming(planned));
    REQUIRE(CountOf(rig.reads->Calls(), Kind::PrefetchStanding) == 1);

    // The run on the lane is the fourth in the new order: one run is first, one
    // is kept, and it is neither.
    ahead.Plan(Upcoming(planned, true));

    CHECK(CountOf(rig.reads->Calls(), Kind::CancelStanding) == 1);
    CHECK(ahead.GetStats().runs_withdrawn == 1);
    CHECK(LeftOnTheLane(rig.reads->Calls()).size() == 1);
  }
}

TEMPLATE_TEST_CASE_SIG(
    "VolumeOptions::media_kind decides the standing lane, not the reader",
    "[io][directory-read-ahead][standing-lane][media-kind]",
    ((Cache::Strategy S), S), Cache::Strategy::NoCache,
    Cache::Strategy::FullCache) {
  const auto image = TestFakeImage::BuildFakeNtfsImageWithNameTree(
      ReadAhead::SpacedTree(directories, files_per_directory));

  SECTION("a rotational statement turns the lane off on a solid state reader") {
    VolumeOptions options;
    options.media_kind = Tuning::MediaKind::Rotational;
    const ReadAhead::Run run = ReadAhead::ListRecursively<S>(
        image, options, 64, large_budget, Tuning::MediaKind::SolidState);
    CHECK(std::ranges::none_of(run.calls, IsLane));
  }

  SECTION("a solid state statement turns it on over a rotational reader") {
    VolumeOptions options;
    options.media_kind = Tuning::MediaKind::SolidState;
    const ReadAhead::Run run = ReadAhead::ListRecursively<S>(
        image, options, 64, large_budget, Tuning::MediaKind::Rotational);
    CHECK(CountOf(run.calls, Kind::PrefetchStanding) > 0);
  }

  SECTION("an unknown statement is a statement too") {
    VolumeOptions options;
    options.media_kind = Tuning::MediaKind::Unknown;
    const ReadAhead::Run run = ReadAhead::ListRecursively<S>(
        image, options, 64, large_budget, Tuning::MediaKind::Rotational);
    CHECK(CountOf(run.calls, Kind::PrefetchStanding) > 0);
  }

  SECTION("no statement leaves the reader's kind") {
    const ReadAhead::Run run =
        ReadAhead::ListRecursively<S>(image, VolumeOptions{}, 64, large_budget,
                                      Tuning::MediaKind::Rotational);
    CHECK(std::ranges::none_of(run.calls, IsLane));
  }
}

TEMPLATE_TEST_CASE_SIG("NtfsVolume::GetMediaKind answers the option first",
                       "[volume][media-kind]", ((Cache::Strategy S), S),
                       Cache::Strategy::NoCache, Cache::Strategy::FullCache) {
  const auto image = TestFakeImage::BuildFakeNtfsImageWithNameTree(
      ReadAhead::SpacedTree(directories, files_per_directory));

  const auto kind_of = [&image](const VolumeOptions& options,
                                Tuning::MediaKind reader_kind) {
    auto memory = std::make_unique<TestDiskReaders::MemoryDiskReader>(image);
    memory->SetMediaKind(reader_kind);
    const NtfsVolume<S> volume(std::move(memory), options);
    REQUIRE(volume.IsVolumeOK());
    return volume.GetMediaKind();
  };

  VolumeOptions stated;
  stated.media_kind = Tuning::MediaKind::SolidState;
  CHECK(kind_of(VolumeOptions{}, Tuning::MediaKind::Rotational) ==
        Tuning::MediaKind::Rotational);
  CHECK(kind_of(stated, Tuning::MediaKind::Rotational) ==
        Tuning::MediaKind::SolidState);
  CHECK(kind_of(stated, Tuning::MediaKind::Unknown) ==
        Tuning::MediaKind::SolidState);
}
