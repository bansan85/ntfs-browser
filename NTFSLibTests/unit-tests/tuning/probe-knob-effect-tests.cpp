#include <ntfs-browser/win-types.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>

#include <ntfs-browser/attr/mask.h>
#include <ntfs-browser/cache/strategy.h>
#include <ntfs-browser/disk-reader.h>
#include <ntfs-browser/io/file-record.h>
#include <ntfs-browser/mft/idx.h>
#include <ntfs-browser/namespace.h>
#include <ntfs-browser/ntfs-volume.h>
#include <ntfs-browser/test/disk-readers/counting-disk-reader.h>
#include <ntfs-browser/test/disk-readers/memory-disk-reader.h>
#include <ntfs-browser/test/fake-image/fake-ntfs-image.h>
#include <ntfs-browser/test/support/walk-comparison.h>
#include <ntfs-browser/tuning/tuning.h>
#include <ntfs-browser/volume-options.h>

#include "knob-effect-support.h"

namespace NTFS_BROWSER_NAMESPACE {

class IndexEntryView;

namespace Io {

enum class RecordStanding : std::uint8_t;

}  // namespace Io
}  // namespace NTFS_BROWSER_NAMESPACE

namespace Cache = NTFS_BROWSER_NAMESPACE::Cache;
namespace Mft = NTFS_BROWSER_NAMESPACE::Mft;
namespace Tuning = NTFS_BROWSER_NAMESPACE::Tuning;
namespace TestTuning = NTFS_BROWSER_NAMESPACE::Test::Tuning;
namespace TestSupport = NTFS_BROWSER_NAMESPACE::Test::Support;
namespace TestDiskReaders = NTFS_BROWSER_NAMESPACE::Test::DiskReaders;
namespace TestFakeImage = NTFS_BROWSER_NAMESPACE::Test::FakeImage;
using NTFS_BROWSER_NAMESPACE::IndexEntryView;
using NTFS_BROWSER_NAMESPACE::NtfsVolume;
using NTFS_BROWSER_NAMESPACE::VolumeOptions;
using NTFS_BROWSER_NAMESPACE::Io::FileRecord;
using NTFS_BROWSER_NAMESPACE::Io::RecordStanding;
using namespace TestTuning;

namespace {

// Slots of the indexed fixture: the root lists a few index blocks of names
// that scatter the slots, so each node probes records far apart.
constexpr ULONGLONG scan_records = 100;

// What the batched walk of the root did to the reader once the root was
// parsed, and the probes among it.
struct RootWalk {
  std::vector<Call> calls;
  size_t probe_reads{0};
  size_t groups{1};
  ULONGLONG longest_probe{0};
};

template <Cache::Strategy S = Cache::Strategy::NoCache>
RootWalk WalkRoot(const std::vector<BYTE>& image, const VolumeOptions& options,
                  Tuning::MediaKind kind = Tuning::MediaKind::Unknown) {
  auto memory = std::make_unique<TestDiskReaders::MemoryDiskReader>(image);
  memory->SetMediaKind(kind);
  auto reader =
      std::make_unique<TestDiskReaders::CountingDiskReader>(std::move(memory));
  TestDiskReaders::CountingDiskReader& reads = *reader;
  const NtfsVolume<S> volume(std::move(reader), options);
  REQUIRE(volume.IsVolumeOK());

  FileRecord<S> root(volume);
  root.SetAttrMask(NTFS_BROWSER_NAMESPACE::Attr::Mask::IndexRoot |
                   NTFS_BROWSER_NAMESPACE::Attr::Mask::IndexAllocation);
  REQUIRE(root.ParseFileRecord(static_cast<ULONGLONG>(Mft::Idx::Root)));
  REQUIRE(root.ParseAttrs());
  reads.Reset();
  root.TraverseSubEntriesWithStanding(
      [](const IndexEntryView&, RecordStanding, void*) {}, nullptr);

  RootWalk walk;
  walk.calls = reads.Calls();
  const ULONGLONG first = TestFakeImage::FragmentedMftScanSlotOffset(
      static_cast<ULONGLONG>(Mft::Idx::User));
  const ULONGLONG last =
      TestFakeImage::FragmentedMftScanSlotOffset(scan_records);
  for (const Call& call : walk.calls) {
    if (call.kind == Kind::CancelPrefetch) {
      walk.groups++;
    } else if (call.kind == Kind::Read && call.offset >= first &&
               call.offset < last) {
      walk.probe_reads++;
      walk.longest_probe = (std::max)(walk.longest_probe, call.length);
    }
  }
  return walk;
}

// True when two walks made the same calls, in the same order.
bool SameCalls(const RootWalk& left, const RootWalk& right) {
  return std::ranges::equal(
      left.calls, right.calls, [](const Call& a, const Call& b) {
        return a.kind == b.kind && a.offset == b.offset && a.length == b.length;
      });
}

// The entries a walk of every directory shows, in order, with their standing.
template <Cache::Strategy S = Cache::Strategy::NoCache>
std::vector<TestSupport::WalkedEntry>
    Entries(const std::vector<BYTE>& image, const VolumeOptions& options,
            Tuning::MediaKind kind = Tuning::MediaKind::Unknown) {
  auto memory = std::make_unique<TestDiskReaders::MemoryDiskReader>(image);
  memory->SetMediaKind(kind);
  const NtfsVolume<S> volume(std::move(memory), options);
  REQUIRE(volume.IsVolumeOK());
  return TestSupport::WalkVolume(volume, true).entries;
}

const std::vector<BYTE>& Image() {
  static const std::vector<BYTE> image =
      TestFakeImage::BuildFakeNtfsImageWithIndexedMftScan(scan_records);
  return image;
}

}  // namespace

TEST_CASE("probe.max_gap_bytes sets the hole of $MFT a probe reads across",
          "[tuning][knob-effect][probe]") {
  const RootWalk base = WalkRoot(Image(), {});
  const RootWalk apart =
      WalkRoot(Image(), Tuned({}, {{"probe.max_gap_bytes", 0}}));
  const RootWalk some =
      WalkRoot(Image(), Tuned({}, {{"probe.max_gap_bytes", 1024}}));

  // The records of a node sit apart: with no hole allowed, each run is shorter
  // and there are more of them.
  CHECK(apart.probe_reads > some.probe_reads);
  CHECK(some.probe_reads > base.probe_reads);
  CHECK(apart.longest_probe < base.longest_probe);
  CHECK(Entries(Image(), Tuned({}, {{"probe.max_gap_bytes", 0}})) ==
        Entries(Image(), {}));
}

TEST_CASE(
    "The probes read across the wide gap on a hard disk through FullCache",
    "[tuning][knob-effect][probe]") {
  constexpr auto full = Cache::Strategy::FullCache;
  // No narrow gap, and a wide one with no density asked for: a device that
  // honours it reads fewer, longer runs.
  const VolumeOptions wide =
      Tuned({}, {{"probe.max_gap_bytes", 0},
                 {"probe.wide_gap_bytes", 1024 * 1024},
                 {"probe.wide_gap_min_share_per_mille", 0}});
  const RootWalk narrow =
      WalkRoot<full>(Image(), Tuned({}, {{"probe.max_gap_bytes", 0}}));
  const RootWalk unknown = WalkRoot<full>(Image(), wide);
  const RootWalk flash =
      WalkRoot<full>(Image(), wide, Tuning::MediaKind::SolidState);
  const RootWalk hard =
      WalkRoot<full>(Image(), wide, Tuning::MediaKind::Rotational);

  CHECK(SameCalls(unknown, narrow));
  CHECK(SameCalls(flash, narrow));
  CHECK_FALSE(SameCalls(hard, narrow));
  CHECK(hard.probe_reads < narrow.probe_reads);
  CHECK(hard.longest_probe > narrow.longest_probe);
  CHECK(Entries<full>(Image(), wide, Tuning::MediaKind::Rotational) ==
        Entries<full>(Image(), {}));
}

TEST_CASE("The wide gap of a hard disk keeps the density it was asked for",
          "[tuning][knob-effect][probe]") {
  constexpr auto full = Cache::Strategy::FullCache;
  const auto share = [](std::uint64_t per_mille) {
    return Tuned({}, {{"probe.max_gap_bytes", 0},
                      {"probe.wide_gap_bytes", 1024 * 1024},
                      {"probe.wide_gap_min_share_per_mille", per_mille}});
  };
  const RootWalk narrow =
      WalkRoot<full>(Image(), Tuned({}, {{"probe.max_gap_bytes", 0}}));
  const RootWalk loose =
      WalkRoot<full>(Image(), share(0), Tuning::MediaKind::Rotational);
  // Every slot of a run wanted: no hole is crossed.
  const RootWalk strict =
      WalkRoot<full>(Image(), share(1000), Tuning::MediaKind::Rotational);

  CHECK(SameCalls(strict, narrow));
  CHECK(loose.probe_reads < strict.probe_reads);
}

TEST_CASE("A NoCache walk never reads across the wide gap",
          "[tuning][knob-effect][probe]") {
  const VolumeOptions wide =
      Tuned({}, {{"probe.max_gap_bytes", 0},
                 {"probe.wide_gap_bytes", 1024 * 1024},
                 {"probe.wide_gap_min_share_per_mille", 0}});
  const RootWalk narrow =
      WalkRoot(Image(), Tuned({}, {{"probe.max_gap_bytes", 0}}));
  const RootWalk hard = WalkRoot(Image(), wide, Tuning::MediaKind::Rotational);

  CHECK(SameCalls(hard, narrow));
}

TEST_CASE("probe.max_run_bytes sets the size of one read of probes",
          "[tuning][knob-effect][probe]") {
  const RootWalk base = WalkRoot(Image(), {});
  for (const std::uint64_t run : {std::uint64_t{4096}, std::uint64_t{8192}}) {
    INFO("run " << run);
    const RootWalk tuned =
        WalkRoot(Image(), Tuned({}, {{"probe.max_run_bytes", run}}));

    CHECK(tuned.longest_probe <= run);
    CHECK(base.longest_probe > run);
    CHECK(tuned.probe_reads > base.probe_reads);
    CHECK(Entries(Image(), Tuned({}, {{"probe.max_run_bytes", run}})) ==
          Entries(Image(), {}));
  }
}

TEST_CASE("probe.max_chunk_records sets the records of one round of probes",
          "[tuning][knob-effect][probe]") {
  const RootWalk base = WalkRoot(Image(), {});
  const RootWalk single =
      WalkRoot(Image(), Tuned({}, {{"probe.max_chunk_records", 1}}));
  const RootWalk four =
      WalkRoot(Image(), Tuned({}, {{"probe.max_chunk_records", 4}}));

  // A round announces, reads and withdraws: the rounds are the groups.
  CHECK(single.groups > four.groups);
  CHECK(four.groups > base.groups);
  CHECK(single.probe_reads > four.probe_reads);
  CHECK(four.probe_reads > base.probe_reads);
  CHECK(Entries(Image(), Tuned({}, {{"probe.max_chunk_records", 1}})) ==
        Entries(Image(), {}));
}

TEST_CASE("probe.max_known_records sets the memo of probed records",
          "[tuning][knob-effect][probe]") {
  const RootWalk base = WalkRoot(Image(), {});
  const RootWalk forgetful =
      WalkRoot(Image(), Tuned({}, {{"probe.max_known_records", 1}}));

  // A record the memo forgot is probed again when another entry names it.
  CHECK(forgetful.probe_reads > base.probe_reads);
  CHECK(Entries(Image(), Tuned({}, {{"probe.max_known_records", 1}})) ==
        Entries(Image(), {}));
}

TEST_CASE("A volume reports the kind of device its reader says",
          "[tuning][knob-effect][probe]") {
  for (const Tuning::MediaKind kind :
       {Tuning::MediaKind::Unknown, Tuning::MediaKind::SolidState,
        Tuning::MediaKind::Rotational}) {
    auto memory = std::make_unique<TestDiskReaders::MemoryDiskReader>(Image());
    memory->SetMediaKind(kind);
    const NtfsVolume<Cache::Strategy::NoCache> volume(std::move(memory), {});
    REQUIRE(volume.IsVolumeOK());
    CHECK(volume.GetMediaKind() == kind);
  }
}

TEST_CASE("The probe knobs leave a walk of FullCache as it was",
          "[tuning][knob-effect][probe]") {
  const VolumeOptions tuned = Tuned({}, {{"probe.max_gap_bytes", 0},
                                         {"probe.max_run_bytes", 4096},
                                         {"probe.max_chunk_records", 3},
                                         {"probe.max_known_records", 2}});
  const auto walk = [](const VolumeOptions& options) {
    const NtfsVolume<Cache::Strategy::FullCache> volume(
        std::make_unique<TestDiskReaders::MemoryDiskReader>(Image()), options);
    REQUIRE(volume.IsVolumeOK());
    return TestSupport::WalkVolume(volume, true);
  };

  const TestSupport::WalkResult plain = walk({});
  const TestSupport::WalkResult changed = walk(tuned);
  CHECK(changed.entries == plain.entries);
  CHECK_FALSE(plain.entries.empty());
}
