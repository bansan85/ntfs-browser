#include <ntfs-browser/win-types.h>

#include <optional>
#include <vector>

#include <ntfs-browser/alloc/types.h>
#include <ntfs-browser/attr/type.h>
#include <ntfs-browser/namespace.h>
#include <ntfs-browser/test/fake-image/account-volume.h>
#include <ntfs-browser/test/fake-image/fake-ntfs-image.h>
#include <ntfs-browser/tool/fuzzgen/build-features.h>
#include <ntfs-browser/tool/fuzzgen/cluster-state-scenarios.h>
#include <ntfs-browser/tool/fuzzgen/recorded-stream.h>
#include <ntfs-browser/tool/fuzzgen/scenario-registry.h>

namespace NTFS_BROWSER_NAMESPACE::Tool::FuzzGen {

namespace {

namespace FakeImage = Test::FakeImage;
using FakeImage::FakeAccountFile;
using FakeImage::FakeAccountStream;
using FakeImage::FakeAccountVolume;
using FakeImage::FakeDataRun;
using FakeImage::MakeAccountStream;
using FakeImage::MakeFreedAccountFile;
using FakeImage::MakeLiveAccountFile;

constexpr ULONGLONG cluster = FakeImage::fake_cluster_size;

// The $LogFile sequence numbers of the freed files: the first, and a later one
// that the file freed afterwards carries.
constexpr ULONGLONG first_lsn = 100;
constexpr ULONGLONG later_lsn = 200;

// MFT slots of the files. The base image has freed and live files from 17 on,
// and the inputs replace them.
constexpr ULONGLONG freed_slot = 22;
constexpr ULONGLONG second_freed_slot = 23;
constexpr ULONGLONG live_slot = 17;
constexpr ULONGLONG first_extension_slot = 24;
constexpr ULONGLONG second_extension_slot = 25;

// The freed file the inputs judge: four clusters at 40.
FakeAccountFile Subject() {
  return MakeFreedAccountFile(first_lsn,
                              MakeAccountStream({{40, 4}}, 4 * cluster));
}

// The freed file, with a valid data length that stops one cluster short.
FakeAccountFile SubjectWithShortValidData() {
  FakeAccountFile subject = Subject();
  subject.streams.front().ini_size = 3 * cluster;
  return subject;
}

// A freed file over a live file's cluster, a freed file that was freed later,
// a bad cluster, and a file's own valid data length that stops short.
std::vector<BYTE> FreedClaims() {
  FakeAccountVolume volume;
  volume.Put(freed_slot, SubjectWithShortValidData());
  volume.Put(
      second_freed_slot,
      MakeFreedAccountFile(later_lsn, MakeAccountStream({{41, 1}}, cluster)));
  volume.Put(live_slot,
             MakeLiveAccountFile(MakeAccountStream({{42, 1}}, cluster)));
  volume.allocated = {FakeDataRun{.lcn = 42, .clusters = 2}};
  volume.bad = {Alloc::Extent{.lcn = 40, .clusters = 1}};
  return volume.Build();
}

// A freed file whose run list is cut, a damaged live file over free clusters,
// and a run past the volume.
std::vector<BYTE> CutRunLists() {
  FakeAccountVolume volume;
  FakeAccountFile cut = MakeFreedAccountFile(
      first_lsn, MakeAccountStream({{40, 2}, {50, 2}}, 4 * cluster));
  cut.streams.front().cut_after_runs = 1;
  volume.Put(freed_slot, cut);
  FakeAccountFile damaged = MakeLiveAccountFile(MakeAccountStream(
      {{44, 2}, {FakeImage::account_far_lcn, 2}}, 4 * cluster));
  damaged.streams.front().cut_after_runs = 2;
  volume.Put(live_slot, damaged);
  return volume.Build();
}

// A compressed freed stream with a unit another file rewrote, and sparse
// holes.
std::vector<BYTE> CompressedMap() {
  FakeAccountVolume volume;
  FakeAccountStream stream =
      MakeAccountStream({{40, 2},
                         FakeDataRun{.lcn = std::nullopt, .clusters = 2},
                         {44, 2},
                         FakeDataRun{.lcn = std::nullopt, .clusters = 2}},
                        8 * cluster);
  stream.comp_unit_size = FakeImage::compression_unit_size_shift;
  volume.Put(freed_slot, MakeFreedAccountFile(first_lsn, stream));
  volume.Put(live_slot,
             MakeLiveAccountFile(MakeAccountStream({{41, 1}}, cluster)));
  volume.allocated = {FakeDataRun{.lcn = 41, .clusters = 1}};
  return volume.Build();
}

// Files whose attributes sit in extension records that their $ATTRIBUTE_LIST
// names: a freed $DATA, and a live $INDEX_ALLOCATION over one of its clusters.
std::vector<BYTE> ExtensionClaims() {
  FakeAccountVolume volume;
  FakeAccountFile freed =
      MakeFreedAccountFile(first_lsn, MakeAccountStream({}, 0));
  freed.streams.clear();
  freed.extension_idx = first_extension_slot;
  freed.extension_streams = {MakeAccountStream({{40, 4}}, 4 * cluster)};
  volume.Put(freed_slot, freed);
  FakeAccountStream index;
  index.type = Attr::Type::IndexAllocation;
  index.runs = {FakeDataRun{.lcn = 41, .clusters = 1}};
  index.real_size = cluster;
  FakeAccountFile live;
  live.extension_idx = second_extension_slot;
  live.extension_streams = {index};
  volume.Put(live_slot, live);
  volume.allocated = {FakeDataRun{.lcn = 41, .clusters = 1}};
  return volume.Build();
}

// A freed file on a volume with no $Bitmap: nothing is known of the clusters.
std::vector<BYTE> NoBitmap() {
  FakeAccountVolume volume;
  volume.with_bitmap = false;
  volume.Put(freed_slot, Subject());
  return volume.Build();
}

// A sparse stream, a named one, and an encrypted one that the content check
// leaves alone.
std::vector<BYTE> Streams() {
  FakeAccountVolume volume;
  FakeAccountFile file = MakeFreedAccountFile(
      first_lsn,
      MakeAccountStream(
          {{40, 2}, FakeDataRun{.lcn = std::nullopt, .clusters = 3}, {45, 1}},
          6 * cluster));
  FakeAccountStream named = MakeAccountStream({{50, 2}}, 2 * cluster);
  named.name = L"zone";
  file.streams.push_back(named);
  volume.Put(freed_slot, file);
  FakeAccountStream encrypted = MakeAccountStream({{54, 2}}, 2 * cluster);
  encrypted.encrypted = true;
  volume.Put(second_freed_slot, MakeFreedAccountFile(first_lsn, encrypted));
  return volume.Build();
}

}  // namespace

void ClusterStateScenarios::Register(ScenarioRegistry& registry) {
  // Recovering, with freed records shown: the only way a file is concerned.
  const auto options = RecordedStream::Recovering();
  const auto account = RecordedStream::Probe::ClusterAccount;
  registry.AddRecorded("cluster_state_freed_claims", FreedClaims, options,
                       account);
  registry.AddRecorded("cluster_state_cut_run_lists", CutRunLists, options,
                       account);
  registry.AddRecorded("cluster_state_compressed_map", CompressedMap, options,
                       account, Feature::Decompression);
  registry.AddRecorded("cluster_state_extension_claims", ExtensionClaims,
                       options, account);
  registry.AddRecorded("cluster_state_no_bitmap", NoBitmap, options, account);
  registry.AddRecorded("cluster_state_streams", Streams, options, account);
}

}  // namespace NTFS_BROWSER_NAMESPACE::Tool::FuzzGen
