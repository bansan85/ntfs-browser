#include <ntfs-browser/win-types.h>

#include <algorithm>
#include <cstddef>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <catch2/catch_template_test_macros.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <ntfs-browser/cache/strategy.h>
#include <ntfs-browser/log/log.h>
#include <ntfs-browser/log/sink.h>
#include <ntfs-browser/mft/idx.h>
#include <ntfs-browser/mft/tree.h>
#include <ntfs-browser/namespace.h>
#include <ntfs-browser/ntfs-volume.h>
#include <ntfs-browser/volume-options.h>

#include "counting-disk-reader.h"
#include "fake-ntfs-image.h"
#include "memory-disk-reader.h"
#include "mft/tree-testing.h"
#include "test-log-sink.h"

using NTFS_BROWSER_NAMESPACE::NtfsVolume;
using NTFS_BROWSER_NAMESPACE::VolumeOptions;
using NTFS_BROWSER_NAMESPACE::Mft::MftTree;
namespace Cache = NTFS_BROWSER_NAMESPACE::Cache;
namespace Log = NTFS_BROWSER_NAMESPACE::Log;
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

// Whether the scan announced, after its first read of record idx and before
// its last, a range that holds that last read: the hint the second pass gives
// the reader for a record it reads again.
template <Cache::Strategy S>
bool AnnouncedBeforeLastRead(const CountedScan<S>& scan, ULONGLONG idx) {
  using Call = Tests::CountingDiskReader::Call;
  using Kind = Tests::CountingDiskReader::Kind;
  const std::vector<Call>& calls = scan.reader->Calls();
  const auto touches = [idx](const Call& call) {
    return call.kind == Kind::Read && call.offset < RecordOffset(idx + 1) &&
           RecordOffset(idx) < call.offset + call.length;
  };
  const auto first = std::ranges::find_if(calls, touches);
  const auto last = std::ranges::find_if(calls.rbegin(), calls.rend(), touches);
  if (first == calls.end() || last == calls.rend() || &*first == &*last) {
    return false;
  }
  return std::any_of(first, last.base(), [&last](const Call& call) {
    return call.kind == Kind::Prefetch && call.offset <= last->offset &&
           last->offset + last->length <= call.offset + call.length;
  });
}

// The counters of a scan that the held budget MUST NOT change.
struct StatsSummary {
  ULONGLONG in_use;
  ULONGLONG deleted;
  ULONGLONG extensions;
  ULONGLONG unreadable;
  ULONGLONG damaged;
  ULONGLONG reused_extensions;

  bool operator==(const StatsSummary&) const = default;
};

StatsSummary SummarizeStats(const MftTree& tree) {
  const MftTree::ScanStats& stats = tree.Stats();
  return {.in_use = stats.in_use,
          .deleted = stats.deleted,
          .extensions = stats.extensions,
          .unreadable = stats.unreadable,
          .damaged = stats.damaged,
          .reused_extensions = stats.reused_extensions};
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

// Keeps the level of each message the library logs, the text aside.
class LevelRecorder final : public Log::ISink {
 public:
  [[nodiscard]] bool ShouldLog(Log::Level level) const override {
    return level != Log::Level::Off;
  }

  void Write(Log::Level level, std::string_view /*message*/) override {
    levels_.push_back(level);
  }

  void Flush() override {}

  [[nodiscard]] bool Logged(Log::Level level) const {
    return std::ranges::find(levels_, level) != levels_.end();
  }

 private:
  std::vector<Log::Level> levels_;
};

// Makes a LevelRecorder the library's only sink while it lives, and puts the
// capture sink back after it.
class RecordLevels {
 public:
  RecordLevels() : recorder_(std::make_shared<LevelRecorder>()) {
    Log::SetSinks({recorder_});
  }

  RecordLevels(const RecordLevels&) = delete;
  RecordLevels(RecordLevels&&) = delete;
  RecordLevels& operator=(const RecordLevels&) = delete;
  RecordLevels& operator=(RecordLevels&&) = delete;

  ~RecordLevels() { Tests::InstallCaptureSink(); }

  [[nodiscard]] const LevelRecorder& Levels() const { return *recorder_; }

 private:
  std::shared_ptr<LevelRecorder> recorder_;
};

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

  SECTION("a scan that may hold nothing reads each of them once more") {
    const HeldBudget budget(0);
    const CountedScan<Cache::Strategy::NoCache> scan =
        Scan<Cache::Strategy::NoCache>(
            Tests::BuildFakeNtfsImageWithMftTreeAttributeList());
    // The high one is named twice, by its $FILE_NAME and its $DATA.
    CHECK(ReadsOfRecord(scan, Tests::mft_tree_list_low_ext_idx) == 2);
    CHECK(ReadsOfRecord(scan, Tests::mft_tree_list_high_ext_idx) == 2);
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

TEMPLATE_TEST_CASE_SIG(
    "MftTree imports the extensions of a freed file, freed with it",
    "[mft][tree][attribute-list]", ((Cache::Strategy S), S),
    Cache::Strategy::NoCache, Cache::Strategy::FullCache) {
  const VolumeOptions options{.include_deleted = true};
  const CountedScan<S> scan = Scan<S>(
      Tests::BuildFakeNtfsImageWithMftTreeAttributeList(
          Tests::FakeListedExtension::Genuine,
          Tests::FakeListResidency::Resident, Tests::FakeListedState::Freed),
      options);

  const MftTree::Entry* const listed =
      scan.tree->Find(Tests::mft_tree_list_base_idx);
  REQUIRE(listed != nullptr);
  CHECK_FALSE(listed->in_use);
  // The name comes from one extension, the hard link and the $DATA from the
  // other.
  CHECK(listed->names.size() == 2);
  CHECK(listed->size == Tests::mft_tree_list_data_size);
  CHECK(scan.tree->Stats().damaged == 0);
  CHECK(scan.tree->Stats().reused_extensions == 0);
}

TEMPLATE_TEST_CASE_SIG(
    "MftTree skips an extension another file reused since its freed file was "
    "freed, without a defect",
    "[mft][tree][attribute-list]", ((Cache::Strategy S), S),
    Cache::Strategy::NoCache, Cache::Strategy::FullCache) {
  const bool recover = GENERATE(false, true);
  const VolumeOptions options{.include_deleted = true,
                              .recover_errors = recover};
  const std::vector<BYTE> image =
      Tests::BuildFakeNtfsImageWithMftTreeAttributeList(
          Tests::FakeListedExtension::Foreign,
          Tests::FakeListResidency::Resident, Tests::FakeListedState::Freed);

  const RecordLevels levels;
  const CountedScan<S> scan = Scan<S>(image, options);

  const MftTree::Entry* const listed =
      scan.tree->Find(Tests::mft_tree_list_base_idx);
  REQUIRE(listed != nullptr);
  // The extension freed with the file is imported; the reused one is not.
  REQUIRE(listed->names.size() == 1);
  CHECK(listed->names.front().name == L"listed.txt");
  CHECK(scan.tree->Stats().damaged == 0);
  CHECK(scan.tree->Stats().reused_extensions == 1);
  CHECK_FALSE(levels.Levels().Logged(Log::Level::Warn));
  CHECK_FALSE(levels.Levels().Logged(Log::Level::Error));
  CHECK(levels.Levels().Logged(Log::Level::Debug));
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

TEMPLATE_TEST_CASE_SIG(
    "MftTree reads no slot again that its first pass saw as another file's",
    "[mft][tree][attribute-list]", ((Cache::Strategy S), S),
    Cache::Strategy::NoCache, Cache::Strategy::FullCache) {
  const Tests::FakeListedExtension extension = GENERATE(
      Tests::FakeListedExtension::Foreign, Tests::FakeListedExtension::Base);
  const Tests::FakeListedState state =
      GENERATE(Tests::FakeListedState::Live, Tests::FakeListedState::Freed);
  const bool recover = GENERATE(false, true);
  const bool nothing_held = GENERATE(false, true);
  const bool freed = state == Tests::FakeListedState::Freed;
  const VolumeOptions options{.include_deleted = freed,
                              .recover_errors = recover};
  const std::vector<BYTE> image =
      Tests::BuildFakeNtfsImageWithMftTreeAttributeList(
          extension, Tests::FakeListResidency::Resident, state);

  const std::vector<EntrySummary> expected =
      Summarize(*Scan<S>(image, options).tree);

  const HeldBudget budget(nothing_held ? std::optional<size_t>{0}
                                       : std::nullopt);
  const CountedScan<S> scan = Scan<S>(image, options);
  const MftTree& tree = *scan.tree;

  // The block read of the first pass is the only one.
  CHECK(ReadsOfRecord(scan, Tests::mft_tree_list_high_ext_idx) == 1);
  CHECK(Summarize(tree) == expected);

  const MftTree::Entry* const listed = tree.Find(Tests::mft_tree_list_base_idx);
  if (!freed && !recover) {
    CHECK(listed == nullptr);
    CHECK(tree.Stats().damaged == 1);
    return;
  }
  REQUIRE(listed != nullptr);
  CHECK(listed->names.size() == 1);
  CHECK(listed->names.front().name == L"listed.txt");
  CHECK(tree.Stats().damaged == (freed ? 0 : 1));
  CHECK(tree.Stats().reused_extensions == (freed ? 1 : 0));
}

TEMPLATE_TEST_CASE_SIG(
    "MftTree gives the same tree and counters whatever it may hold",
    "[mft][tree][attribute-list]", ((Cache::Strategy S), S),
    Cache::Strategy::NoCache, Cache::Strategy::FullCache) {
  const Tests::FakeListedExtension extension = GENERATE(
      Tests::FakeListedExtension::Genuine, Tests::FakeListedExtension::Foreign,
      Tests::FakeListedExtension::Base);
  const Tests::FakeListResidency list =
      GENERATE(Tests::FakeListResidency::Resident,
               Tests::FakeListResidency::NonResident);
  const Tests::FakeListedState state =
      GENERATE(Tests::FakeListedState::Live, Tests::FakeListedState::Freed);
  const bool recover = GENERATE(false, true);
  const VolumeOptions options{.include_deleted =
                                  state == Tests::FakeListedState::Freed,
                              .recover_errors = recover};
  const std::vector<BYTE> image =
      Tests::BuildFakeNtfsImageWithMftTreeAttributeList(extension, list, state);

  const CountedScan<S> unlimited = Scan<S>(image, options);
  const std::vector<EntrySummary> expected = Summarize(*unlimited.tree);
  const StatsSummary expected_stats = SummarizeStats(*unlimited.tree);

  // Nothing, one record, two, three: the extension records and the listed one
  // are held in turn, or not.
  const size_t records = GENERATE(size_t{0}, size_t{1}, size_t{2}, size_t{3});
  INFO("records held: " << records);
  const HeldBudget budget(records * Tests::fake_file_record_size);
  const CountedScan<S> scan = Scan<S>(image, options);

  CHECK(Summarize(*scan.tree) == expected);
  CHECK(SummarizeStats(*scan.tree) == expected_stats);
}

TEMPLATE_TEST_CASE_SIG(
    "MftTree announces what it reads again when it may not hold it",
    "[mft][tree][attribute-list][prefetch]", ((Cache::Strategy S), S),
    Cache::Strategy::NoCache) {
  const HeldBudget budget(0);
  const CountedScan<S> scan =
      Scan<S>(Tests::BuildFakeNtfsImageWithMftTreeAttributeList());

  SECTION("each record is announced before the read that needs it") {
    // The listed record waited without its bytes, and its extensions went
    // unheld: each is read in the second pass, announced to the reader first.
    CHECK(AnnouncedBeforeLastRead(scan, Tests::mft_tree_list_base_idx));
    CHECK(AnnouncedBeforeLastRead(scan, Tests::mft_tree_list_low_ext_idx));
    CHECK(AnnouncedBeforeLastRead(scan, Tests::mft_tree_list_high_ext_idx));
  }

  SECTION("an extension the list names twice is read once") {
    CHECK(ReadsOfRecord(scan, Tests::mft_tree_list_base_idx) == 2);
    CHECK(ReadsOfRecord(scan, Tests::mft_tree_list_low_ext_idx) == 2);
    CHECK(ReadsOfRecord(scan, Tests::mft_tree_list_high_ext_idx) == 2);
  }
}

TEMPLATE_TEST_CASE_SIG(
    "MftTree holds an extension record only for a record that will ask for it",
    "[mft][tree][attribute-list]", ((Cache::Strategy S), S),
    Cache::Strategy::NoCache, Cache::Strategy::FullCache) {
  const std::vector<BYTE> image =
      Tests::BuildFakeNtfsImageWithMftTreeStrayExtension();
  const std::vector<EntrySummary> expected = Summarize(*Scan<S>(image).tree);

  // One record of room: the stray extension takes it, and gives it back when
  // the record it extends turns out to have no list. The listed file's low
  // extension takes it next.
  const HeldBudget budget(Tests::fake_file_record_size);
  const CountedScan<S> scan = Scan<S>(image);

  CHECK(Summarize(*scan.tree) == expected);
  CHECK(ReadsOfRecord(scan, Tests::mft_tree_list_low_ext_idx) == 1);
  CHECK(ReadsOfRecord(scan, Tests::mft_tree_hard_link_idx) == 1);
}
