#include <ntfs-browser/win-types.h>

#include <cstdlib>
#include <filesystem>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include <ntfs-browser/alloc/types.h>
#include <ntfs-browser/attr/type.h>
#include <ntfs-browser/namespace.h>
#include <ntfs-browser/volume-options.h>

#include "cluster-account-support.h"
#include "fake-ntfs-image.h"
#include "fuzz-input-writer.h"

namespace Alloc = NTFS_BROWSER_NAMESPACE::Alloc;
namespace Attr = NTFS_BROWSER_NAMESPACE::Attr;
namespace Support = NTFS_BROWSER_NAMESPACE::Tests::AccountSupport;
namespace Tests = NTFS_BROWSER_NAMESPACE::Tests;
using NTFS_BROWSER_NAMESPACE::VolumeOptions;
using Support::Freed;
using Support::Live;
using Support::Stream;
using Tests::FakeAccountFile;
using Tests::FakeAccountStream;
using Tests::FakeDataRun;

namespace {

// One saved fuzz input: its file name, and the image it was recorded from.
struct Variant {
  std::string name;
  std::vector<BYTE> image;
};

constexpr ULONGLONG cluster = Tests::fake_cluster_size;

// The freed file the inputs judge: four clusters at 40.
FakeAccountFile Subject(ULONGLONG lsn = 100) {
  return Freed(lsn, Stream({{40, 4}}, 4 * cluster));
}

// A freed file over a live file's cluster, a freed file that was freed later,
// a bad cluster, and a file's own valid data length that stops short.
std::vector<BYTE> FreedClaims() {
  Support::Volume volume;
  FakeAccountFile subject = Subject();
  subject.streams.front().ini_size = 3 * cluster;
  volume.Put(22, subject);
  volume.Put(23, Freed(200, Stream({{41, 1}}, cluster)));
  volume.Put(17, Live(Stream({{42, 1}}, cluster)));
  volume.allocated = {FakeDataRun{.lcn = 42, .clusters = 2}};
  volume.bad = {Alloc::Extent{.lcn = 40, .clusters = 1}};
  return volume.Build();
}

// A freed file whose run list is cut, a damaged live file over free clusters,
// and a run past the volume.
std::vector<BYTE> CutRunLists() {
  Support::Volume volume;
  FakeAccountFile cut = Freed(100, Stream({{40, 2}, {50, 2}}, 4 * cluster));
  cut.streams.front().cut_after_runs = 1;
  volume.Put(22, cut);
  FakeAccountFile damaged =
      Live(Stream({{44, 2}, {Support::far_lcn, 2}}, 4 * cluster));
  damaged.streams.front().cut_after_runs = 2;
  volume.Put(17, damaged);
  return volume.Build();
}

// A compressed freed stream with a unit another file rewrote, and sparse
// holes.
std::vector<BYTE> CompressedMap() {
  Support::Volume volume;
  FakeAccountStream stream =
      Stream({{40, 2},
              FakeDataRun{.lcn = std::nullopt, .clusters = 2},
              {44, 2},
              FakeDataRun{.lcn = std::nullopt, .clusters = 2}},
             8 * cluster);
  stream.comp_unit_size = Tests::compression_unit_size_shift;
  volume.Put(22, Freed(100, stream));
  volume.Put(17, Live(Stream({{41, 1}}, cluster)));
  volume.allocated = {FakeDataRun{.lcn = 41, .clusters = 1}};
  return volume.Build();
}

// Files whose attributes sit in extension records that their $ATTRIBUTE_LIST
// names: a freed $DATA, and a live $INDEX_ALLOCATION over one of its clusters.
std::vector<BYTE> ExtensionClaims() {
  Support::Volume volume;
  FakeAccountFile freed = Freed(100, Stream({}, 0));
  freed.streams.clear();
  freed.extension_idx = 24;
  freed.extension_streams = {Stream({{40, 4}}, 4 * cluster)};
  volume.Put(22, freed);
  FakeAccountStream index;
  index.type = Attr::Type::IndexAllocation;
  index.runs = {FakeDataRun{.lcn = 41, .clusters = 1}};
  index.real_size = cluster;
  FakeAccountFile live;
  live.extension_idx = 25;
  live.extension_streams = {index};
  volume.Put(17, live);
  volume.allocated = {FakeDataRun{.lcn = 41, .clusters = 1}};
  return volume.Build();
}

// A freed file on a volume with no $Bitmap: nothing is known of the clusters.
std::vector<BYTE> NoBitmap() {
  Support::Volume volume;
  volume.with_bitmap = false;
  volume.Put(22, Subject());
  return volume.Build();
}

// A sparse stream, a named one, and an encrypted one that the content check
// leaves alone.
std::vector<BYTE> Streams() {
  Support::Volume volume;
  FakeAccountFile file =
      Freed(100, Stream({{40, 2},
                         FakeDataRun{.lcn = std::nullopt, .clusters = 3},
                         {45, 1}},
                        6 * cluster));
  FakeAccountStream named = Stream({{50, 2}}, 2 * cluster);
  named.name = L"zone";
  file.streams.push_back(named);
  volume.Put(22, file);
  FakeAccountStream encrypted = Stream({{54, 2}}, 2 * cluster);
  encrypted.encrypted = true;
  volume.Put(23, Freed(100, encrypted));
  return volume.Build();
}

// The inputs, one per way the clusters of a file can be wrong.
std::vector<Variant> Variants() {
  std::vector<Variant> variants;
  variants.push_back({"cluster_state_freed_claims", FreedClaims()});
  variants.push_back({"cluster_state_cut_run_lists", CutRunLists()});
  variants.push_back({"cluster_state_compressed_map", CompressedMap()});
  variants.push_back({"cluster_state_extension_claims", ExtensionClaims()});
  variants.push_back({"cluster_state_no_bitmap", NoBitmap()});
  variants.push_back({"cluster_state_streams", Streams()});
  return variants;
}

}  // namespace

// Writes the saved fuzz inputs for the cluster account probe of
// FuzzTestcase(): the scan with the account at its fullest, recorded from that
// scan itself, since the main probes read in another order. The volume
// recovers and shows freed records, the only way a file is concerned. Set
// NTFS_BROWSER_FUZZ_INPUT_DIR to the folder to write them in (the repository's
// NTFSLibTests/fuzz/data) and run this hidden test case by its tag.
TEST_CASE("write the cluster state fuzz inputs", "[.write-fuzz-inputs]") {
  const char* const dir = std::getenv("NTFS_BROWSER_FUZZ_INPUT_DIR");
  if (dir == nullptr) {
    SKIP("NTFS_BROWSER_FUZZ_INPUT_DIR is not set");
  }

  const VolumeOptions options{.include_deleted = true, .recover_errors = true};
  for (Variant& variant : Variants()) {
    Tests::FuzzInputs::WriteRecordedAccountInput(
        std::filesystem::path(dir) / variant.name, std::move(variant.image),
        options);
  }
}
