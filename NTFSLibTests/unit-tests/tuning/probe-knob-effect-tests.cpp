#include <ntfs-browser/win-types.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include <ntfs-browser/attr/mask.h>
#include <ntfs-browser/cache/strategy.h>
#include <ntfs-browser/index-entry.h>
#include <ntfs-browser/io/file-record.h>
#include <ntfs-browser/io/record-standing.h>
#include <ntfs-browser/mft/idx.h>
#include <ntfs-browser/namespace.h>
#include <ntfs-browser/ntfs-volume.h>
#include <ntfs-browser/volume-options.h>

#include "counting-disk-reader.h"
#include "fake-ntfs-image.h"
#include "knob-effect-support.h"
#include "memory-disk-reader.h"
#include "walk-comparison.h"

namespace Cache = NTFS_BROWSER_NAMESPACE::Cache;
namespace Mft = NTFS_BROWSER_NAMESPACE::Mft;
namespace Tests = NTFS_BROWSER_NAMESPACE::Tests;
using NTFS_BROWSER_NAMESPACE::IndexEntryView;
using NTFS_BROWSER_NAMESPACE::NtfsVolume;
using NTFS_BROWSER_NAMESPACE::VolumeOptions;
using NTFS_BROWSER_NAMESPACE::Io::FileRecord;
using NTFS_BROWSER_NAMESPACE::Io::RecordStanding;
using namespace Tests::KnobEffect;

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

RootWalk WalkRoot(const std::vector<BYTE>& image,
                  const VolumeOptions& options) {
  auto reader = std::make_unique<Tests::CountingDiskReader>(
      std::make_unique<Tests::MemoryDiskReader>(image));
  Tests::CountingDiskReader& reads = *reader;
  const NtfsVolume<Cache::Strategy::NoCache> volume(std::move(reader), options);
  REQUIRE(volume.IsVolumeOK());

  FileRecord<Cache::Strategy::NoCache> root(volume);
  root.SetAttrMask(NTFS_BROWSER_NAMESPACE::Attr::Mask::IndexRoot |
                   NTFS_BROWSER_NAMESPACE::Attr::Mask::IndexAllocation);
  REQUIRE(root.ParseFileRecord(static_cast<ULONGLONG>(Mft::Idx::Root)));
  REQUIRE(root.ParseAttrs());
  reads.Reset();
  root.TraverseSubEntriesWithStanding(
      [](const IndexEntryView&, RecordStanding, void*) {}, nullptr);

  RootWalk walk;
  walk.calls = reads.Calls();
  const ULONGLONG first = Tests::FragmentedMftScanSlotOffset(
      static_cast<ULONGLONG>(Mft::Idx::User));
  const ULONGLONG last = Tests::FragmentedMftScanSlotOffset(scan_records);
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

// The entries a walk of every directory shows, in order, with their standing.
std::vector<Tests::WalkedEntry> Entries(const std::vector<BYTE>& image,
                                        const VolumeOptions& options) {
  const NtfsVolume<Cache::Strategy::NoCache> volume(
      std::make_unique<Tests::MemoryDiskReader>(image), options);
  REQUIRE(volume.IsVolumeOK());
  return Tests::WalkVolume(volume, true).entries;
}

const std::vector<BYTE>& Image() {
  static const std::vector<BYTE> image =
      Tests::BuildFakeNtfsImageWithIndexedMftScan(scan_records);
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

TEST_CASE("The probe knobs leave a walk of FullCache as it was",
          "[tuning][knob-effect][probe]") {
  const VolumeOptions tuned = Tuned({}, {{"probe.max_gap_bytes", 0},
                                         {"probe.max_run_bytes", 4096},
                                         {"probe.max_chunk_records", 3},
                                         {"probe.max_known_records", 2}});
  const auto walk = [](const VolumeOptions& options) {
    const NtfsVolume<Cache::Strategy::FullCache> volume(
        std::make_unique<Tests::MemoryDiskReader>(Image()), options);
    REQUIRE(volume.IsVolumeOK());
    return Tests::WalkVolume(volume, true);
  };

  const Tests::WalkResult plain = walk({});
  const Tests::WalkResult changed = walk(tuned);
  CHECK(changed.entries == plain.entries);
  CHECK_FALSE(plain.entries.empty());
}
