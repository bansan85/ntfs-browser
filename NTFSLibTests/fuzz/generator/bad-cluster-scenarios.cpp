#include <ntfs-browser/win-types.h>

#include <span>
#include <string>
#include <utility>
#include <vector>

#include <ntfs-browser/alloc/types.h>
#include <ntfs-browser/namespace.h>
#include <ntfs-browser/test/fake-image/fake-ntfs-image.h>
#include <ntfs-browser/tool/fuzzgen/bad-cluster-scenarios.h>
#include <ntfs-browser/tool/fuzzgen/recorded-stream.h>
#include <ntfs-browser/tool/fuzzgen/scenario-registry.h>
#include <ntfs-browser/volume-options.h>

namespace NTFS_BROWSER_NAMESPACE::Tool::FuzzGen {

namespace {

namespace FakeImage = Test::FakeImage;
using FakeImage::FakeBadClusterOptions;
using FakeImage::FakeDataRun;

// One saved input: its file name, and the bad list the image holds.
struct Variant {
  std::string name;
  std::vector<Alloc::Extent> bad;
  FakeBadClusterOptions options;
};

// The clusters the bitmap of every input marks used: the first extent of the
// valid lists.
constexpr FakeDataRun marked_run{.lcn = 40, .clusters = 2};

// The inputs, one per way a hostile $BadClus can be wrong.
std::vector<Variant> Variants() {
  return {
      {"bad_clusters_marked",
       {{.lcn = 45, .clusters = 2}, {.lcn = 60, .clusters = 1}},
       {}},
      {"bad_clusters_cut_runs",
       {{.lcn = 45, .clusters = 2}, {.lcn = 52, .clusters = 1}},
       {.cut_run_list = true}},
      {"bad_clusters_past_volume",
       {{.lcn = 30, .clusters = 5}, {.lcn = 300, .clusters = 10}},
       {}},
      {"bad_clusters_overlap",
       {{.lcn = 10, .clusters = 5},
        {.lcn = 12, .clusters = 6},
        {.lcn = 18, .clusters = 2},
        {.lcn = 18, .clusters = 2}},
       {}},
      {"bad_clusters_out_of_order",
       {{.lcn = 50, .clusters = 2},
        {.lcn = 10, .clusters = 3},
        {.lcn = 30, .clusters = 1}},
       {}},
      {"bad_clusters_huge_count",
       {{.lcn = 45, .clusters = 2}},
       {.forged_size = ULONGLONG{1} << 63}},
      {"bad_clusters_no_stream",
       {{.lcn = 45, .clusters = 2}},
       {.omit_bad_stream = true}},
      {"bad_clusters_odd_flags",
       {{.lcn = 45, .clusters = 2}},
       {.attr_flags = WORD{0x8001}}},
      {"bad_clusters_conflicting_flags",
       {{.lcn = 45, .clusters = 2}},
       {.attr_flags = WORD{0x4001}}},
  };
}

// The freed tree with a bitmap that marks the first extent, and the variant's
// $BadClus.
std::vector<BYTE> ImageOf(const Variant& variant) {
  std::vector<BYTE> image = FakeImage::BuildFakeNtfsImageWithFreedFileTree();
  const std::vector<FakeDataRun> marked{marked_run};
  FakeImage::PutClusterBitmap(image,
                              FakeImage::AllocatedBitmapFor(image, marked));
  FakeImage::PutBadClusterList(image, variant.bad, variant.options);
  return image;
}

}  // namespace

void BadClusterScenarios::Register(ScenarioRegistry& registry) {
  // The inputs are recorded from the main probes under the default options.
  for (const Variant& variant : Variants()) {
    registry.AddRecorded(
        variant.name, [variant] { return ImageOf(variant); },
        NTFS_BROWSER_NAMESPACE::VolumeOptions{});
  }
}

}  // namespace NTFS_BROWSER_NAMESPACE::Tool::FuzzGen
