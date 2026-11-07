#include <ntfs-browser/win-types.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include <ntfs-browser/attr/mask.h>
#include <ntfs-browser/cache/strategy.h>
#include <ntfs-browser/disk-reader.h>
#include <ntfs-browser/index-entry.h>
#include <ntfs-browser/io/file-record.h>
#include <ntfs-browser/mft/idx.h>
#include <ntfs-browser/namespace.h>
#include <ntfs-browser/ntfs-volume.h>
#include <ntfs-browser/test/disk-readers/counting-disk-reader.h>
#include <ntfs-browser/test/disk-readers/memory-disk-reader.h>
#include <ntfs-browser/test/fake-image/fake-ntfs-image.h>
#include <ntfs-browser/test/support/test-log-sink.h>
#include <ntfs-browser/volume-options.h>

#include "io/record-window.h"

namespace NtfsBrowser {
namespace Io {

enum class RecordStanding : std::uint8_t;

}  // namespace Io
}  // namespace NtfsBrowser

namespace Cache = NTFS_BROWSER_NAMESPACE::Cache;
namespace Io = NTFS_BROWSER_NAMESPACE::Io;
namespace Mft = NTFS_BROWSER_NAMESPACE::Mft;
namespace Tests = NTFS_BROWSER_NAMESPACE::Tests;
using Io::FileRecord;
using Io::RecordStanding;
using NTFS_BROWSER_NAMESPACE::IndexEntryView;
using NTFS_BROWSER_NAMESPACE::NtfsVolume;
using NTFS_BROWSER_NAMESPACE::VolumeOptions;
using Tests::CountingDiskReader;
using Tests::MemoryDiskReader;

namespace {

constexpr Cache::Strategy walk_cache = Cache::Strategy::NoCache;
using Store = FileRecord<walk_cache>::ProbeStore;

// Slots of the indexed scan fixture the tests walk.
constexpr ULONGLONG scan_records = 100;

// Room for every record of the fixture.
constexpr std::uint64_t window_bytes = std::uint64_t{1} << 20U;

// What a walk of the root directory gave and did.
struct RootWalk {
  std::vector<std::pair<ULONGLONG, RecordStanding>> entries;
  std::vector<CountingDiskReader::Call> calls;
  std::string log;
};

// Walks the root directory of image with TraverseSubEntriesWithStanding(), with
// store given to the root record when it is not null. The calls and the log are
// those of the walk.
RootWalk WalkRoot(const std::vector<BYTE>& image, const Store* store) {
  auto reader = std::make_unique<CountingDiskReader>(
      std::make_unique<MemoryDiskReader>(image));
  CountingDiskReader& reads = *reader;
  NtfsVolume<walk_cache> volume(std::move(reader), VolumeOptions{});
  REQUIRE(volume.IsVolumeOK());

  FileRecord<walk_cache> root(volume);
  root.SetAttrMask(NTFS_BROWSER_NAMESPACE::Attr::Mask::IndexRoot |
                   NTFS_BROWSER_NAMESPACE::Attr::Mask::IndexAllocation);
  REQUIRE(root.ParseFileRecord(static_cast<ULONGLONG>(Mft::Idx::Root)));
  REQUIRE(root.ParseAttrs());
  if (store != nullptr) {
    root.SetProbeStore(*store);
  }
  reads.Reset();
  (void)Tests::TakeCapturedLog();

  RootWalk walk;
  root.TraverseSubEntriesWithStanding(
      [](const IndexEntryView& entry, RecordStanding standing, void* context) {
        static_cast<RootWalk*>(context)->entries.emplace_back(
            entry.GetFileReference(), standing);
      },
      &walk);
  walk.calls = reads.Calls();
  walk.log = Tests::TakeCapturedLog();
  return walk;
}

// The reads of the walk that fall in the records of $MFT the fixture lists.
std::size_t ProbeReads(const std::vector<CountingDiskReader::Call>& calls) {
  const ULONGLONG first = Tests::FragmentedMftScanSlotOffset(
      static_cast<ULONGLONG>(Mft::Idx::User));
  const ULONGLONG last = Tests::FragmentedMftScanSlotOffset(scan_records);
  return static_cast<std::size_t>(
      std::ranges::count_if(calls, [&](const CountingDiskReader::Call& call) {
        return call.kind == CountingDiskReader::Kind::Read &&
               call.offset >= first && call.offset < last;
      }));
}

// A store over window: it finds what the window holds and keeps every run.
Store StoreOver(Io::RecordWindow& window) {
  return {.find = [&window](ULONGLONG slot) { return window.Find(slot); },
          .keep =
              [&window](ULONGLONG first, std::span<const BYTE> bytes) {
                window.Put(first, bytes);
              }};
}

}  // namespace

TEST_CASE("A probe store spares the probes the records it holds",
          "[io][probe-store]") {
  const std::vector<BYTE> image =
      Tests::BuildFakeNtfsImageWithIndexedMftScan(scan_records);
  const RootWalk plain = WalkRoot(image, nullptr);
  REQUIRE(ProbeReads(plain.calls) > 0);

  Io::RecordWindow window(window_bytes, Tests::fake_file_record_size);
  const Store store = StoreOver(window);
  const RootWalk first = WalkRoot(image, &store);
  CHECK(first.entries == plain.entries);
  CHECK(first.log == plain.log);
  // A run the first walk keeps may serve a later chunk of the same walk.
  CHECK(ProbeReads(first.calls) <= ProbeReads(plain.calls));
  CHECK(window.HeldBytes() > 0);

  // A walk over a volume of its own finds the records in the window.
  const RootWalk second = WalkRoot(image, &store);
  CHECK(second.entries == plain.entries);
  CHECK(second.log == plain.log);
  // What is left is read whatever the store holds: a record that fails its
  // update sequence check is read again by itself, and the index blocks.
  CHECK(ProbeReads(second.calls) < ProbeReads(first.calls));
  CHECK(second.calls.size() < plain.calls.size());
}

TEST_CASE("A probe store that only keeps changes no read",
          "[io][probe-store]") {
  const std::vector<BYTE> image =
      Tests::BuildFakeNtfsImageWithIndexedMftScan(scan_records);
  const RootWalk plain = WalkRoot(image, nullptr);

  std::size_t runs = 0;
  const Store store{.find = {},
                    .keep = [&runs](ULONGLONG, std::span<const BYTE> bytes) {
                      CHECK_FALSE(bytes.empty());
                      CHECK(bytes.size() % Tests::fake_file_record_size == 0);
                      runs++;
                    }};
  const RootWalk kept = WalkRoot(image, &store);
  CHECK(runs > 0);
  CHECK(kept.entries == plain.entries);
  CHECK(kept.log == plain.log);
  REQUIRE(kept.calls.size() == plain.calls.size());
  for (std::size_t index = 0; index < kept.calls.size(); index++) {
    CHECK(kept.calls.at(index).kind == plain.calls.at(index).kind);
    CHECK(kept.calls.at(index).offset == plain.calls.at(index).offset);
    CHECK(kept.calls.at(index).length == plain.calls.at(index).length);
  }
}

TEST_CASE("A probe store that finds nothing leaves the walk as it was",
          "[io][probe-store]") {
  const std::vector<BYTE> image =
      Tests::BuildFakeNtfsImageWithIndexedMftScan(scan_records);
  const RootWalk plain = WalkRoot(image, nullptr);

  std::size_t asked = 0;
  const Store store{.find =
                        [&asked](ULONGLONG) {
                          asked++;
                          return std::span<const BYTE>();
                        },
                    .keep = {}};
  const RootWalk walk = WalkRoot(image, &store);
  CHECK(asked > 0);
  CHECK(walk.entries == plain.entries);
  CHECK(walk.log == plain.log);
  CHECK(walk.calls.size() == plain.calls.size());
}

TEST_CASE("The plain walk never asks the probe store", "[io][probe-store]") {
  const std::vector<BYTE> image =
      Tests::BuildFakeNtfsImageWithIndexedMftScan(scan_records);
  auto reader = std::make_unique<MemoryDiskReader>(image);
  NtfsVolume<walk_cache> volume(std::move(reader), VolumeOptions{});
  REQUIRE(volume.IsVolumeOK());
  FileRecord<walk_cache> root(volume);
  root.SetAttrMask(NTFS_BROWSER_NAMESPACE::Attr::Mask::IndexRoot |
                   NTFS_BROWSER_NAMESPACE::Attr::Mask::IndexAllocation);
  REQUIRE(root.ParseFileRecord(static_cast<ULONGLONG>(Mft::Idx::Root)));
  REQUIRE(root.ParseAttrs());

  std::size_t asked = 0;
  std::size_t kept = 0;
  root.SetProbeStore(
      {.find =
           [&asked](ULONGLONG) {
             asked++;
             return std::span<const BYTE>();
           },
       .keep = [&kept](ULONGLONG, std::span<const BYTE>) { kept++; }});
  std::size_t listed = 0;
  root.TraverseSubEntries(
      [](const IndexEntryView&, void* context) {
        (*static_cast<std::size_t*>(context))++;
      },
      &listed);
  CHECK(listed > 0);
  CHECK(asked == 0);
  CHECK(kept == 0);
}
