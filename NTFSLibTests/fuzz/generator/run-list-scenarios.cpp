#include <ntfs-browser/win-types.h>

#include <limits>
#include <string>
#include <utility>
#include <vector>

#include <ntfs-browser/namespace.h>
#include <ntfs-browser/test/fake-image/fake-ntfs-image.h>
#include <ntfs-browser/tool/fuzzgen/recorded-stream.h>
#include <ntfs-browser/tool/fuzzgen/run-list-scenarios.h>
#include <ntfs-browser/tool/fuzzgen/scenario-registry.h>

namespace NTFS_BROWSER_NAMESPACE::Tool::FuzzGen {

namespace {

namespace FakeImage = Test::FakeImage;
using FakeImage::FakeDataRun;
using FakeImage::FakeFreedStream;
using FakeImage::FakeUsnOptions;
using FakeImage::FakeUsnRecord;

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
  std::vector<BYTE> image = FakeImage::BuildFakeNtfsImageWithFreedFileTree();
  FakeImage::PutFreedNonResidentRecord(
      image, FakeImage::mft_tree_stale_child_idx, stream);
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
  std::vector<BYTE> image = FakeImage::BuildFakeNtfsImageWithFreedFileTree();
  FakeImage::PutUsnJournal(image, options);
  return image;
}

}  // namespace

void RunListScenarios::Register(ScenarioRegistry& registry) {
  // Opened recovering with deleted items shown: only then does a scan list the
  // extents of freed streams and read the journal.
  const auto options = RecordedStream::Recovering();
  registry.AddRecorded(
      "run_list_empty_run",
      [] {
        return ImageWith({.real_size = 2 * FakeImage::fake_cluster_size,
                          .runs = {FakeDataRun{.lcn = 10, .clusters = 2}},
                          .raw_runs = empty_first_run,
                          .last_vcn = std::numeric_limits<ULONGLONG>::max()});
      },
      options);
  registry.AddRecorded(
      "run_list_overflowing_run",
      [] {
        return ImageWith({.real_size = FakeImage::fake_cluster_size,
                          .runs = {FakeDataRun{.lcn = 10, .clusters = 1}},
                          .raw_runs = overflowing_run,
                          .last_vcn = past_half_range});
      },
      options);
  registry.AddRecorded(
      "run_list_cut_after_two_runs",
      [] {
        return ImageWith({.real_size = 6 * FakeImage::fake_cluster_size,
                          .runs = {FakeDataRun{.lcn = 10, .clusters = 3},
                                   FakeDataRun{.lcn = 20, .clusters = 2},
                                   FakeDataRun{.lcn = 30, .clusters = 1}},
                          .raw_runs = cut_after_two_runs});
      },
      options);
  registry.AddRecorded("usn_journal_adjacent_stretches", AdjacentStretches,
                       options);
}

}  // namespace NTFS_BROWSER_NAMESPACE::Tool::FuzzGen
