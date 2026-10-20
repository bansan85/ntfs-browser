#include <ntfs-browser/win-types.h>

#include <cstddef>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include <ntfs-browser/alloc/types.h>
#include <ntfs-browser/namespace.h>

#include "alloc-test-support.h"
#include "fake-ntfs-image.h"
#include "fuzz-core.h"
#include "memory-disk-reader.h"
#include "recording-disk-reader.h"

namespace Alloc = NTFS_BROWSER_NAMESPACE::Alloc;
namespace Tests = NTFS_BROWSER_NAMESPACE::Tests;
namespace Support = NTFS_BROWSER_NAMESPACE::Tests::AllocSupport;
using Tests::FakeBadClusterOptions;
using Tests::FakeDataRun;

namespace {

// One saved fuzz input: its file name, and the bad list the image holds.
struct Variant {
  std::string name;
  std::vector<Alloc::Extent> bad;
  FakeBadClusterOptions options;
};

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
  const std::vector<FakeDataRun> marked{FakeDataRun{.lcn = 40, .clusters = 2}};
  std::vector<BYTE> image =
      Support::WithBitmap(Tests::BuildFakeNtfsImageWithFreedFileTree(), marked);
  return Support::WithBadClusters(std::move(image), variant.bad,
                                  variant.options);
}

// Bytes of the boot sector read a NoCache volume issues first, and how far the
// fuzzer's reader moves the stream after it.
constexpr size_t boot_sector_read_bytes = 4096;
constexpr size_t boot_sector_stream_bytes = 512;

}  // namespace

// Writes the saved fuzz inputs of the bad cluster list. An input is the byte
// stream that NtfsFuzz::ProbeOnce() reads from the image, in read order and
// without addresses, which is what the fuzzer replays. The volume reads its
// boot sector in 4096 bytes, and the fuzzer's reader advances by 512 after that
// first read (see LoopingDiskReader), so the bytes between are cut out of the
// recording. Set NTFS_BROWSER_FUZZ_INPUT_DIR to the folder to write them in
// (the repository's NTFSLibTests/fuzz/data) and run this hidden test case by
// its tag.
TEST_CASE("write the bad cluster fuzz inputs", "[.write-fuzz-inputs]") {
  const char* const dir = std::getenv("NTFS_BROWSER_FUZZ_INPUT_DIR");
  if (dir == nullptr) {
    SKIP("NTFS_BROWSER_FUZZ_INPUT_DIR is not set");
  }

  for (const Variant& variant : Variants()) {
    const std::filesystem::path path =
        std::filesystem::path(dir) / variant.name;
    {
      NtfsFuzz::ProbeOnce(std::make_unique<Tests::RecordingDiskReader>(
          std::make_unique<Tests::MemoryDiskReader>(ImageOf(variant)), path));
    }
    REQUIRE(std::filesystem::exists(path));

    std::vector<char> stream;
    {
      std::ifstream in(path, std::ios::binary);
      stream.assign(std::istreambuf_iterator<char>(in), {});
    }
    REQUIRE(stream.size() > boot_sector_read_bytes);
    stream.erase(stream.begin() + boot_sector_stream_bytes,
                 stream.begin() + boot_sector_read_bytes);
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(stream.data(), static_cast<std::streamsize>(stream.size()));
  }
}
