#include <ntfs-browser/win-types.h>

#include <algorithm>
#include <cstddef>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include <catch2/catch_template_test_macros.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <ntfs-browser/cache/strategy.h>
#include <ntfs-browser/mft/idx.h>
#include <ntfs-browser/mft/tree.h>
#include <ntfs-browser/namespace.h>
#include <ntfs-browser/ntfs-volume.h>
#include <ntfs-browser/volume-options.h>

#include "counting-disk-reader.h"
#include "fake-ntfs-image.h"
#include "memory-disk-reader.h"
#include "mft/tree-testing.h"

using NTFS_BROWSER_NAMESPACE::NtfsVolume;
using NTFS_BROWSER_NAMESPACE::VolumeOptions;
using NTFS_BROWSER_NAMESPACE::Mft::MftTree;
namespace Cache = NTFS_BROWSER_NAMESPACE::Cache;
namespace Mft = NTFS_BROWSER_NAMESPACE::Mft;
namespace Tests = NTFS_BROWSER_NAMESPACE::Tests;

namespace {

constexpr ULONGLONG root_value = static_cast<ULONGLONG>(Mft::Idx::Root);

// Extension records of the fixture: the two of the listed file, and the one
// BuildFakeNtfsImageWithMftTree() already has.
constexpr ULONGLONG fixture_extensions = 3;

// Byte offset of record idx in the fixture: its $MFT starts at cluster 1.
constexpr ULONGLONG RecordOffset(ULONGLONG idx) {
  return Tests::fake_cluster_size + (idx * Tests::fake_file_record_size);
}

// Sets the budget of held records for one test, and restores the default.
class HeldBudget {
 public:
  explicit HeldBudget(std::optional<size_t> bytes) {
    Mft::SetMaxHeldRecordBytesForTests(bytes);
  }

  HeldBudget(const HeldBudget&) = delete;
  HeldBudget(HeldBudget&&) = delete;
  HeldBudget& operator=(const HeldBudget&) = delete;
  HeldBudget& operator=(HeldBudget&&) = delete;

  ~HeldBudget() { Mft::SetMaxHeldRecordBytesForTests(std::nullopt); }
};

// A scan of a fixture, with the volume it read and the reader that counted
// its reads.
template <Cache::Strategy S>
struct CountedScan {
  std::unique_ptr<NtfsVolume<S>> volume;
  const Tests::CountingDiskReader* reader;
  std::unique_ptr<MftTree> tree;
};

template <Cache::Strategy S>
CountedScan<S> Scan(std::vector<BYTE> image,
                    const VolumeOptions& options = {}) {
  auto reader = std::make_unique<Tests::CountingDiskReader>(
      std::make_unique<Tests::MemoryDiskReader>(std::move(image)));
  const Tests::CountingDiskReader* const counting = reader.get();
  auto volume = std::make_unique<NtfsVolume<S>>(std::move(reader), options);
  REQUIRE(volume->IsVolumeOK());
  auto tree = std::make_unique<MftTree>(*volume);
  return {std::move(volume), counting, std::move(tree)};
}

// Reads of a scan that touch record idx.
template <Cache::Strategy S>
size_t ReadsOfRecord(const CountedScan<S>& scan, ULONGLONG idx) {
  return scan.reader->ReadsInRange(
      RecordOffset(idx), RecordOffset(idx) + Tests::fake_file_record_size);
}

// What the tests compare between two trees, entry by entry.
struct EntrySummary {
  ULONGLONG record;
  std::vector<std::wstring> names;
  ULONGLONG size;

  bool operator==(const EntrySummary&) const = default;
};

std::vector<EntrySummary> Summarize(const MftTree& tree) {
  std::vector<EntrySummary> summary;
  for (const MftTree::Entry& entry : tree.Entries()) {
    EntrySummary& added = summary.emplace_back();
    added.record = entry.record;
    added.size = entry.size;
    for (const MftTree::Name& name : entry.names) {
      added.names.push_back(name.name);
    }
  }
  return summary;
}

// Whether record is one of the children tree files under dir.
bool HasChild(const MftTree& tree, ULONGLONG dir, ULONGLONG record) {
  const std::span<const ULONGLONG> children = tree.Children(dir);
  return std::ranges::find(children, record) != children.end();
}

}  // namespace

TEMPLATE_TEST_CASE_SIG(
    "MftTree takes what an $ATTRIBUTE_LIST names from the extension records "
    "on both sides of its record",
    "[mft][tree][attribute-list]", ((Cache::Strategy S), S),
    Cache::Strategy::NoCache, Cache::Strategy::FullCache) {
  const bool include_deleted = GENERATE(false, true);
  const bool recover = GENERATE(false, true);
  const VolumeOptions options{.include_deleted = include_deleted,
                              .recover_errors = recover};
  const CountedScan<S> scan =
      Scan<S>(Tests::BuildFakeNtfsImageWithMftTreeAttributeList(), options);
  const MftTree& tree = *scan.tree;

  const MftTree::Entry* const listed = tree.Find(Tests::mft_tree_list_base_idx);
  REQUIRE(listed != nullptr);
  REQUIRE(listed->names.size() == 2);
  CHECK(listed->names.front().name == L"listed.txt");
  CHECK(listed->names.front().parent_record == root_value);
  CHECK(listed->names.back().name == L"listed-link.txt");
  CHECK(listed->names.back().parent_record == Tests::mft_tree_docs_idx);
  CHECK(listed->size == Tests::mft_tree_list_data_size);
  CHECK(tree.GetPath(Tests::mft_tree_list_base_idx) == L"\\listed.txt");
  CHECK(HasChild(tree, root_value, Tests::mft_tree_list_base_idx));
  CHECK(
      HasChild(tree, Tests::mft_tree_docs_idx, Tests::mft_tree_list_base_idx));

  CHECK(tree.Find(Tests::mft_tree_list_low_ext_idx) == nullptr);
  CHECK(tree.Find(Tests::mft_tree_list_high_ext_idx) == nullptr);
  CHECK(tree.Stats().extensions == fixture_extensions);
  CHECK(tree.Stats().damaged == 0);
  // The listed file waited for the end of the scan, in its place.
  CHECK(std::ranges::is_sorted(tree.Entries(), {}, &MftTree::Entry::record));
}

TEST_CASE("MftTree reads no extension record again for an $ATTRIBUTE_LIST",
          "[mft][tree][attribute-list]") {
  SECTION("the scan holds the extension records it met") {
    const CountedScan<Cache::Strategy::NoCache> scan =
        Scan<Cache::Strategy::NoCache>(
            Tests::BuildFakeNtfsImageWithMftTreeAttributeList());
    // The one block read of the slots past the system files.
    CHECK(ReadsOfRecord(scan, Tests::mft_tree_list_low_ext_idx) == 1);
    CHECK(ReadsOfRecord(scan, Tests::mft_tree_list_high_ext_idx) == 1);
  }

  SECTION("a scan that may hold nothing reads them once per list entry") {
    const HeldBudget budget(0);
    const CountedScan<Cache::Strategy::NoCache> scan =
        Scan<Cache::Strategy::NoCache>(
            Tests::BuildFakeNtfsImageWithMftTreeAttributeList());
    CHECK(ReadsOfRecord(scan, Tests::mft_tree_list_low_ext_idx) == 2);
    CHECK(ReadsOfRecord(scan, Tests::mft_tree_list_high_ext_idx) == 3);
  }
}

TEMPLATE_TEST_CASE_SIG(
    "MftTree gives the same tree whatever records it may hold",
    "[mft][tree][attribute-list]", ((Cache::Strategy S), S),
    Cache::Strategy::NoCache, Cache::Strategy::FullCache) {
  const std::vector<EntrySummary> expected = Summarize(
      *Scan<S>(Tests::BuildFakeNtfsImageWithMftTreeAttributeList()).tree);

  // Nothing, or the extension record met first and not the listed file.
  const size_t bytes =
      GENERATE(size_t{0}, size_t{Tests::fake_file_record_size});
  const HeldBudget budget(bytes);
  CHECK(Summarize(*Scan<S>(Tests::BuildFakeNtfsImageWithMftTreeAttributeList())
                       .tree) == expected);
}

TEMPLATE_TEST_CASE_SIG(
    "MftTree drops a listed file whose extension record belongs to another "
    "file, unless recovering",
    "[mft][tree][attribute-list]", ((Cache::Strategy S), S),
    Cache::Strategy::NoCache, Cache::Strategy::FullCache) {
  const std::vector<BYTE> foreign =
      Tests::BuildFakeNtfsImageWithMftTreeAttributeList(
          Tests::FakeListedExtension::Foreign);

  SECTION("strict") {
    const CountedScan<S> scan = Scan<S>(foreign);
    CHECK(scan.tree->Find(Tests::mft_tree_list_base_idx) == nullptr);
    CHECK(scan.tree->Stats().damaged == 1);

    // Every other entry is there, in order: no empty entry is left behind.
    std::vector<EntrySummary> expected = Summarize(
        *Scan<S>(Tests::BuildFakeNtfsImageWithMftTreeAttributeList()).tree);
    std::erase_if(expected, [](const EntrySummary& entry) {
      return entry.record == Tests::mft_tree_list_base_idx;
    });
    CHECK(Summarize(*scan.tree) == expected);
  }

  SECTION("recovering") {
    const CountedScan<S> scan =
        Scan<S>(foreign, VolumeOptions{.recover_errors = true});
    const MftTree::Entry* const listed =
        scan.tree->Find(Tests::mft_tree_list_base_idx);
    REQUIRE(listed != nullptr);
    REQUIRE(listed->names.size() == 1);
    CHECK(listed->names.front().name == L"listed.txt");
    CHECK(scan.tree->Stats().damaged == 1);
  }
}

TEST_CASE(
    "MftTree announces a non-resident $ATTRIBUTE_LIST before its second pass "
    "reads it",
    "[mft][tree][attribute-list][prefetch]") {
  using Call = Tests::CountingDiskReader::Call;
  using Kind = Tests::CountingDiskReader::Kind;
  const CountedScan<Cache::Strategy::NoCache> scan =
      Scan<Cache::Strategy::NoCache>(
          Tests::BuildFakeNtfsImageWithMftTreeAttributeList(
              Tests::FakeListedExtension::Genuine,
              Tests::FakeListResidency::NonResident));
  const MftTree::Entry* const listed =
      scan.tree->Find(Tests::mft_tree_list_base_idx);
  REQUIRE(listed != nullptr);
  CHECK(listed->names.size() == 2);

  const ULONGLONG list =
      ULONGLONG{Tests::mft_tree_list_lcn} * Tests::fake_cluster_size;
  const std::vector<Call>& calls = scan.reader->Calls();
  const auto read = std::ranges::find_if(calls, [list](const Call& call) {
    return call.kind == Kind::Read &&
           call.offset < list + Tests::fake_cluster_size &&
           list < call.offset + call.length;
  });
  REQUIRE(read != calls.end());
  CHECK(std::any_of(calls.begin(), read, [&read](const Call& call) {
    return call.kind == Kind::Prefetch && call.offset <= read->offset &&
           read->offset + read->length <= call.offset + call.length;
  }));
}
