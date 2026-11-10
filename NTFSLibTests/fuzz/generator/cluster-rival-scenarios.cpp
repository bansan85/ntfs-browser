#include "cluster-rival-scenarios.h"

#include <vector>

#include <ntfs-browser/namespace.h>
#include <ntfs-browser/test/fake-image/account-volume.h>
#include <ntfs-browser/test/fake-image/fake-ntfs-image.h>
#include <ntfs-browser/tool/fuzzgen/recorded-stream.h>
#include <ntfs-browser/tool/fuzzgen/scenario-registry.h>

#include "alloc/types.h"
#include "win-types.h"

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

// The $LogFile sequence numbers of the freed files. Files with the same one
// cannot be ordered by it. A larger one was freed later.
constexpr ULONGLONG early_lsn = 100;
constexpr ULONGLONG late_lsn = 300;

// The MFT slots of the files: the base image has freed and live files from 17
// on, and the inputs replace them.
constexpr ULONGLONG first_slot = 17;
constexpr ULONGLONG second_slot = 18;
constexpr ULONGLONG third_slot = 19;
constexpr ULONGLONG fourth_slot = 20;
constexpr ULONGLONG fifth_slot = 21;
constexpr ULONGLONG sixth_slot = 22;
constexpr ULONGLONG seventh_slot = 23;
constexpr ULONGLONG eighth_slot = 24;

// A stream of runs whose data length stops after written clusters of it.
FakeAccountStream ShortWritten(std::vector<FakeDataRun> runs,
                               ULONGLONG clusters, ULONGLONG written) {
  FakeAccountStream stream =
      MakeAccountStream(std::move(runs), clusters * cluster);
  stream.ini_size = written * cluster;
  return stream;
}

// Two freed files with the same sequence number over the same clusters, a freed
// file whose data length stops short, and a live file that never wrote the
// clusters a freed file keeps. No order tells who owns what.
std::vector<BYTE> RivalOrders() {
  FakeAccountVolume volume;
  // Cut short: its last two clusters were never written.
  volume.Put(first_slot,
             MakeFreedAccountFile(early_lsn, ShortWritten({{40, 4}}, 4, 2)));
  // The same sequence number as the file before, over two of its clusters.
  volume.Put(second_slot,
             MakeFreedAccountFile(early_lsn,
                                  MakeAccountStream({{41, 2}}, 2 * cluster)));
  // A live file that has not written its clusters, over a freed file's.
  volume.Put(third_slot, MakeLiveAccountFile(ShortWritten({{44, 2}}, 2, 0)));
  volume.Put(fourth_slot,
             MakeFreedAccountFile(late_lsn,
                                  MakeAccountStream({{44, 2}}, 2 * cluster)));
  // Freed one after the other over the same clusters.
  volume.Put(fifth_slot,
             MakeFreedAccountFile(early_lsn,
                                  MakeAccountStream({{48, 2}}, 2 * cluster)));
  volume.Put(sixth_slot,
             MakeFreedAccountFile(late_lsn,
                                  MakeAccountStream({{48, 2}}, 2 * cluster)));
  // Two live files over one cluster.
  volume.Put(seventh_slot,
             MakeLiveAccountFile(MakeAccountStream({{52, 2}}, 2 * cluster)));
  volume.Put(eighth_slot,
             MakeLiveAccountFile(MakeAccountStream({{53, 2}}, 2 * cluster)));
  volume.allocated = {FakeDataRun{.lcn = 44, .clusters = 2},
                      FakeDataRun{.lcn = 52, .clusters = 3}};
  return volume.Build();
}

// A freed file over six clusters, a live file over the same six, and a bitmap
// that marks the two in the middle only: the freed file meets three stretches
// of the bitmap and one verdict.
std::vector<BYTE> MergedRivals() {
  FakeAccountVolume volume;
  volume.Put(first_slot,
             MakeFreedAccountFile(early_lsn,
                                  MakeAccountStream({{40, 6}}, 6 * cluster)));
  volume.Put(second_slot,
             MakeLiveAccountFile(MakeAccountStream({{40, 6}}, 6 * cluster)));
  volume.allocated = {FakeDataRun{.lcn = 42, .clusters = 2}};
  return volume.Build();
}

// Freed and live files whose runs a bad cluster cuts in the middle, and one
// that starts on a bad cluster.
std::vector<BYTE> BadInRuns() {
  FakeAccountVolume volume;
  volume.Put(first_slot,
             MakeFreedAccountFile(early_lsn,
                                  MakeAccountStream({{40, 8}}, 8 * cluster)));
  volume.Put(second_slot,
             MakeLiveAccountFile(MakeAccountStream({{48, 4}}, 4 * cluster)));
  volume.Put(third_slot,
             MakeFreedAccountFile(late_lsn,
                                  MakeAccountStream({{54, 3}}, 3 * cluster)));
  volume.allocated = {FakeDataRun{.lcn = 46, .clusters = 8}};
  volume.bad = {Alloc::Extent{.lcn = 42, .clusters = 1},
                Alloc::Extent{.lcn = 44, .clusters = 2},
                Alloc::Extent{.lcn = 49, .clusters = 1},
                Alloc::Extent{.lcn = 54, .clusters = 1}};
  return volume.Build();
}

}  // namespace

void ClusterRivalScenarios::Register(ScenarioRegistry& registry) {
  // Recovering, with freed records shown: the only way a file is concerned.
  const auto options = RecordedStream::Recovering();
  const auto account = RecordedStream::Probe::ClusterAccount;
  registry.AddRecorded("cluster_state_rival_orders", RivalOrders, options,
                       account);
  registry.AddRecorded("cluster_state_merged_rivals", MergedRivals, options,
                       account);
  registry.AddRecorded("cluster_state_bad_in_runs", BadInRuns, options,
                       account);
  registry.AddRecorded("api_mft_list_rivals", RivalOrders, options,
                       RecordedStream::Probe::MftListApi);
  registry.AddRecorded("api_mft_list_bad_in_runs", BadInRuns, options,
                       RecordedStream::Probe::MftListApi);
}

}  // namespace NTFS_BROWSER_NAMESPACE::Tool::FuzzGen
