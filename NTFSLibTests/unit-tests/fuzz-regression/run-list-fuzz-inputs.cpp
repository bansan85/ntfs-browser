#include <ntfs-browser/win-types.h>

#include <cstdlib>
#include <filesystem>
#include <limits>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include <ntfs-browser/namespace.h>
#include <ntfs-browser/volume-options.h>

#include "fake-ntfs-image.h"
#include "fuzz-input-writer.h"

namespace Tests = NTFS_BROWSER_NAMESPACE::Tests;
using NTFS_BROWSER_NAMESPACE::VolumeOptions;
using Tests::FakeDataRun;
using Tests::FakeFreedStream;
using Tests::FakeUsnOptions;
using Tests::FakeUsnRecord;

namespace {

// One saved fuzz input: its file name, and the image it was recorded from.
struct Variant {
  std::string name;
  std::vector<BYTE> image;
};

// Run list bytes: one run of 3 clusters at LCN 10, one of 2 at LCN 20, then a
// header byte whose length field (15 bytes) no run can have. A decoder that
// recovers keeps the first two runs and stops there.
const std::vector<BYTE> cut_after_two_runs{0x11, 0x03, 0x0A, 0x11,
                                           0x02, 0x0A, 0x1F, 0x00};

// Run list bytes: an empty run at LCN 10, then 2 clusters at LCN 20.
const std::vector<BYTE> empty_first_run{0x11, 0x00, 0x0A, 0x11,
                                        0x02, 0x0A, 0x00};

// Run list bytes: one run of 2^63 + 1 clusters at LCN 2^63 - 1. Their sum is
// 2^64, which does not fit.
const std::vector<BYTE> overflowing_run{0x88, 0x01, 0x00, 0x00, 0x00, 0x00,
                                        0x00, 0x00, 0x80, 0xFF, 0xFF, 0xFF,
                                        0xFF, 0xFF, 0xFF, 0xFF, 0x7F, 0x00};

// The first VCN past 2^63: the declared bound the overflowing run needs.
constexpr ULONGLONG past_half_range = (1ULL << 63U) + 10;

// The freed tree with a non-resident data stream in the freed file that has
// none of its own.
std::vector<BYTE> ImageWith(const FakeFreedStream& stream) {
  std::vector<BYTE> image = Tests::BuildFakeNtfsImageWithFreedFileTree();
  Tests::PutFreedNonResidentRecord(image, Tests::mft_tree_stale_child_idx,
                                   stream);
  return image;
}

// A journal of two stretches with no hole between them, so that its run list
// holds two real runs in a row.
std::vector<BYTE> AdjacentStretches() {
  const auto record = [](std::wstring name, ULONGLONG file_record) {
    return FakeUsnRecord{.name = std::move(name), .file_record = file_record};
  };
  FakeUsnOptions options{.records = {record(L"first.txt", 100)}};
  options.more_stretches.push_back(
      {.hole_bytes = 0, .records = {record(L"second.txt", 101)}});
  std::vector<BYTE> image = Tests::BuildFakeNtfsImageWithFreedFileTree();
  Tests::PutUsnJournal(image, options);
  return image;
}

// The inputs, one per way a hostile run list can be wrong.
std::vector<Variant> Variants() {
  std::vector<Variant> variants;
  variants.push_back(
      {"run_list_empty_run",
       ImageWith({.real_size = 2 * Tests::fake_cluster_size,
                  .runs = {FakeDataRun{.lcn = 10, .clusters = 2}},
                  .raw_runs = empty_first_run,
                  .last_vcn = std::numeric_limits<ULONGLONG>::max()})});
  variants.push_back(
      {"run_list_overflowing_run",
       ImageWith({.real_size = Tests::fake_cluster_size,
                  .runs = {FakeDataRun{.lcn = 10, .clusters = 1}},
                  .raw_runs = overflowing_run,
                  .last_vcn = past_half_range})});
  variants.push_back(
      {"run_list_cut_after_two_runs",
       ImageWith({.real_size = 6 * Tests::fake_cluster_size,
                  .runs = {FakeDataRun{.lcn = 10, .clusters = 3},
                           FakeDataRun{.lcn = 20, .clusters = 2},
                           FakeDataRun{.lcn = 30, .clusters = 1}},
                  .raw_runs = cut_after_two_runs})});
  variants.push_back({"usn_journal_adjacent_stretches", AdjacentStretches()});
  return variants;
}

}  // namespace

// Writes the saved fuzz inputs where a run list is damaged or merges. The
// volume is opened recovering with deleted items shown, since only then does a
// scan list the extents of freed streams and read the journal. Set
// NTFS_BROWSER_FUZZ_INPUT_DIR to the folder to write them in (the repository's
// NTFSLibTests/fuzz/data) and run this hidden test case by its tag.
TEST_CASE("write the run list fuzz inputs", "[.write-fuzz-inputs]") {
  const char* const dir = std::getenv("NTFS_BROWSER_FUZZ_INPUT_DIR");
  if (dir == nullptr) {
    SKIP("NTFS_BROWSER_FUZZ_INPUT_DIR is not set");
  }

  const VolumeOptions recovering{.include_deleted = true,
                                 .recover_errors = true};
  for (Variant& variant : Variants()) {
    Tests::FuzzInputs::WriteRecordedInput(std::filesystem::path(dir) /
                                              variant.name,
                                          std::move(variant.image), recovering);
  }
}
