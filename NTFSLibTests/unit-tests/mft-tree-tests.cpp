#include <ntfs-browser/win-types.h>

#include <memory>
#include <optional>
#include <span>
#include <sstream>
#include <string>
#include <vector>

#include <catch2/catch_template_test_macros.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <spdlog/logger.h>
#include <spdlog/sinks/ostream_sink.h>
#include <spdlog/spdlog.h>

#include <ntfs-browser/log.h>
#include <ntfs-browser/mft-idx.h>
#include <ntfs-browser/mft-tree.h>
#include <ntfs-browser/ntfs-volume.h>
#include <ntfs-browser/strategy.h>
#include <ntfs-browser/volume-options.h>

#include "catch2/matchers/catch_matchers.hpp"
#include "corpus-test-support.h"
#include "fake-ntfs-image.h"
#include "memory-disk-reader.h"

using Catch::Matchers::ContainsSubstring;
using NtfsBrowser::MftEntry;
using NtfsBrowser::MftScanOptions;
using NtfsBrowser::MftScanStats;
using NtfsBrowser::MftTree;
using NtfsBrowser::NtfsVolume;
using NtfsBrowser::Strategy;
using NtfsBrowser::VolumeOptions;
using NtfsBrowser::Enum::MftIdx;
using NtfsBrowserTests::attr_name_exceeds_total_size_record_idx;
using NtfsBrowserTests::BuildFakeNtfsImageWithAttrNameExceedsTotalSize;
using NtfsBrowserTests::BuildFakeNtfsImageWithHugeMftRealSize;
using NtfsBrowserTests::BuildFakeNtfsImageWithMftExtensionRecord;
using NtfsBrowserTests::BuildFakeNtfsImageWithMftTree;
using NtfsBrowserTests::FileTimeToTicks;
using NtfsBrowserTests::MemoryDiskReader;
using NtfsBrowserTests::mft_tree_deleted_child_idx;
using NtfsBrowserTests::mft_tree_deleted_dir_idx;
using NtfsBrowserTests::mft_tree_deleted_file_idx;
using NtfsBrowserTests::mft_tree_docs_idx;
using NtfsBrowserTests::mft_tree_extension_idx;
using NtfsBrowserTests::mft_tree_hard_link_idx;
using NtfsBrowserTests::mft_tree_record_count;
using NtfsBrowserTests::mft_tree_report_allocated_size;
using NtfsBrowserTests::mft_tree_report_data_size;
using NtfsBrowserTests::mft_tree_report_idx;
using NtfsBrowserTests::mft_tree_reused_dir_idx;
using NtfsBrowserTests::mft_tree_stale_child_idx;
using NtfsBrowserTests::mft_tree_zeroed_idx;

namespace {

constexpr ULONGLONG root_value = static_cast<ULONGLONG>(MftIdx::Root);

// Opens BuildFakeNtfsImageWithMftTree() as a volume.
template <Strategy S>
std::unique_ptr<NtfsVolume<S>>
    OpenMftTreeVolume(const VolumeOptions& options = {}) {
  auto volume = std::make_unique<NtfsVolume<S>>(
      std::make_unique<MemoryDiskReader>(BuildFakeNtfsImageWithMftTree()),
      options);
  REQUIRE(volume->IsVolumeOK());
  REQUIRE(volume->GetRecordsCount() == mft_tree_record_count);
  return volume;
}

// Children() as a vector, so Catch2 prints it on a mismatch.
std::vector<ULONGLONG> ChildrenOf(const MftTree& tree, ULONGLONG dir) {
  const auto children = tree.Children(dir);
  return {children.begin(), children.end()};
}

template <Strategy S>
void RunMftTreeRebuildsPaths() {
  // The fixture's deleted records (old.tmp, OldDir, draft.doc, stale.txt)
  // must stay visible for the sections below to exercise them: include_deleted
  // is off by default now, so this test opts in explicitly.
  const auto volume =
      OpenMftTreeVolume<S>(VolumeOptions{.include_deleted = true});
  const MftTree tree(*volume);

  CHECK(tree.GetPath(root_value) == L"\\");
  CHECK(tree.GetPath(static_cast<ULONGLONG>(MftIdx::Mft)) == L"\\$MFT");
  CHECK(tree.GetPath(mft_tree_docs_idx) == L"\\Docs");

  SECTION("A DOS alias stays out of the path and the size comes from $DATA") {
    const MftEntry* report = tree.Find(mft_tree_report_idx);
    REQUIRE(report != nullptr);
    CHECK(tree.GetPath(mft_tree_report_idx) == L"\\Docs\\report.txt");
    REQUIRE(report->names.size() == 2);
    // The REQUIRE above checks that names holds 2 entries.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    CHECK(report->names[1].dos_only);
    CHECK(report->size == mft_tree_report_data_size);
    // A resident $DATA's allocated size is its attribute record's own
    // padded reservation, which exceeds the real data size here.
    CHECK(report->allocated_size == mft_tree_report_allocated_size);
    CHECK(report->read_only);
    CHECK(report->archive);
    CHECK(report->in_use);
    CHECK_FALSE(report->directory);
    // WriteStandardInformationAttr() gives create/write/change/access
    // distinct on-disk values, so a wrong field wiring (eg. change_time
    // reading alter_time) would collapse two of these into one.
    CHECK(FileTimeToTicks(report->change_time) !=
          FileTimeToTicks(report->write_time));
    CHECK(FileTimeToTicks(report->change_time) !=
          FileTimeToTicks(report->create_time));
    CHECK(FileTimeToTicks(report->change_time) !=
          FileTimeToTicks(report->access_time));
  }

  SECTION("Each hard link has its own path") {
    CHECK(tree.GetPath(mft_tree_hard_link_idx) == L"\\link-a");
    CHECK(tree.GetPath(mft_tree_hard_link_idx, 1) == L"\\Docs\\link-b");
  }

  SECTION("Children lists each record once, in record order") {
    CHECK(ChildrenOf(tree, root_value) ==
          std::vector<ULONGLONG>{static_cast<ULONGLONG>(MftIdx::Mft),
                                 mft_tree_docs_idx, mft_tree_hard_link_idx,
                                 mft_tree_reused_dir_idx});
    CHECK(ChildrenOf(tree, mft_tree_docs_idx) ==
          std::vector<ULONGLONG>{mft_tree_report_idx, mft_tree_hard_link_idx,
                                 mft_tree_deleted_file_idx,
                                 mft_tree_deleted_dir_idx});
    CHECK(ChildrenOf(tree, mft_tree_deleted_dir_idx) ==
          std::vector<ULONGLONG>{mft_tree_deleted_child_idx});
  }

  SECTION("A deleted subtree stays linked through the bumped sequence number") {
    const MftEntry* deleted = tree.Find(mft_tree_deleted_file_idx);
    REQUIRE(deleted != nullptr);
    CHECK_FALSE(deleted->in_use);
    CHECK(tree.GetPath(mft_tree_deleted_file_idx) == L"\\Docs\\old.tmp");
    CHECK(tree.GetPath(mft_tree_deleted_child_idx) ==
          L"\\Docs\\OldDir\\draft.doc");
    CHECK(tree.IsReachable(mft_tree_deleted_child_idx));
  }

  SECTION("A parent reference to a reused record breaks the path there") {
    std::optional<ULONGLONG> lost;
    CHECK(tree.GetPath(mft_tree_stale_child_idx, &lost) == L"stale.txt");
    CHECK(lost == mft_tree_reused_dir_idx);
    CHECK_FALSE(tree.IsReachable(mft_tree_stale_child_idx));
  }

  SECTION("Extension records and unused slots are not entries") {
    CHECK(tree.Find(mft_tree_extension_idx) == nullptr);
    CHECK(tree.Find(mft_tree_zeroed_idx) == nullptr);
  }

  SECTION("Stats count every slot") {
    const MftScanStats& stats = tree.Stats();
    CHECK(stats.slots == mft_tree_record_count);
    // $MFT, $Volume, the root, Docs, report.txt, the hard link, NewDir.
    CHECK(stats.in_use == 7);
    CHECK(stats.deleted == 4);
    CHECK(stats.extensions == 1);
    // Records 1, 2, 4, 6-15 and 25.
    CHECK(stats.unreadable == 14);
    CHECK(stats.damaged == 0);
    // $Volume, which has no name, and stale.txt.
    CHECK(stats.unreachable == 2);
    CHECK(stats.complete);
    CHECK(tree.Entries().size() == 11);
  }
}

template <Strategy S>
void RunMftTreeWithoutDeleted() {
  // include_deleted defaults off, so the plain default volume already
  // excludes the fixture's freed records.
  const auto volume = OpenMftTreeVolume<S>();
  const MftTree tree(*volume);

  CHECK(tree.Find(mft_tree_deleted_file_idx) == nullptr);
  CHECK(tree.Find(mft_tree_deleted_dir_idx) == nullptr);
  CHECK(tree.Find(mft_tree_stale_child_idx) == nullptr);
  CHECK(ChildrenOf(tree, mft_tree_docs_idx) ==
        std::vector<ULONGLONG>{mft_tree_report_idx, mft_tree_hard_link_idx});
  CHECK(tree.Entries().size() == 7);
  CHECK(tree.Stats().deleted == 4);
  CHECK(tree.Stats().unreachable == 1);
}

// MftTree drops a record whose attributes fail to parse when strict, and
// keeps it (per FileRecord's own recover_errors behaviour) when recovering.
// Uses a record with a masked-in attribute name exceeding its own
// total_size: a strict rejection FileRecord::ParseAttrs() itself already
// covers (see attr-name-bounds-tests.cpp); this checks MftTree's own
// drop-vs-keep response to that outcome.
template <Strategy S>
void RunMftTreeDropsUnrecoveredRecord() {
  {
    const auto volume =
        std::make_unique<NtfsVolume<S>>(std::make_unique<MemoryDiskReader>(
            BuildFakeNtfsImageWithAttrNameExceedsTotalSize()));
    REQUIRE(volume->IsVolumeOK());

    const MftTree tree(*volume);
    CHECK(tree.Stats().damaged == 1);
    CHECK(tree.Find(attr_name_exceeds_total_size_record_idx) == nullptr);
  }
  {
    const auto volume = std::make_unique<NtfsVolume<S>>(
        std::make_unique<MemoryDiskReader>(
            BuildFakeNtfsImageWithAttrNameExceedsTotalSize()),
        VolumeOptions{.recover_errors = true});
    REQUIRE(volume->IsVolumeOK());

    const MftTree tree(*volume);
    CHECK(tree.Stats().damaged == 0);
    CHECK(tree.Find(attr_name_exceeds_total_size_record_idx) != nullptr);
  }
}

template <Strategy S>
void RunMftTreeProgressStops() {
  const auto volume = OpenMftTreeVolume<S>();
  std::vector<ULONGLONG> calls;
  const MftTree tree(*volume,
                     MftScanOptions{.progress = [&](ULONGLONG done, ULONGLONG) {
                       calls.push_back(done);
                       return false;
                     }});

  CHECK(calls == std::vector<ULONGLONG>{0});
  CHECK_FALSE(tree.Stats().complete);
  CHECK(tree.Entries().empty());
}

template <Strategy S>
void RunMftTreeSkipsMftExtensionRecord() {
  const auto volume =
      std::make_unique<NtfsVolume<S>>(std::make_unique<MemoryDiskReader>(
          BuildFakeNtfsImageWithMftExtensionRecord()));
  REQUIRE(volume->IsVolumeOK());
  REQUIRE(volume->GetRecordsCount() == mft_tree_record_count);

  const MftTree tree(*volume);

  CHECK(tree.Find(mft_tree_zeroed_idx) == nullptr);
  // Same counts as the plain fixture, with the extension counted as one.
  CHECK(tree.Stats().extensions == 2);
  CHECK(tree.Stats().in_use == 7);
  CHECK(tree.Stats().unreadable == 13);
  CHECK(tree.Entries().size() == 7);
}

template <Strategy S>
void RunMftTreeClampsForgedRealSize() {
  // Aborts a runaway scan, so the test fails instead of spinning.
  constexpr ULONGLONG runaway_slots = 4096;

  const auto volume =
      std::make_unique<NtfsVolume<S>>(std::make_unique<MemoryDiskReader>(
          BuildFakeNtfsImageWithHugeMftRealSize()));
  REQUIRE(volume->IsVolumeOK());
  CHECK(volume->GetRecordsCount() == mft_tree_record_count);

  const MftTree tree(*volume,
                     MftScanOptions{.progress = [&](ULONGLONG done, ULONGLONG) {
                       return done < runaway_slots;
                     }});

  CHECK(tree.Stats().slots == mft_tree_record_count);
  CHECK(tree.Stats().complete);
}

}  // namespace

TEMPLATE_TEST_CASE_SIG("MftTree bounds its scan by the clusters $MFT maps",
                       "[mft-tree][regression]", ((Strategy S), S),
                       Strategy::NoCache, Strategy::FullCache) {
  RunMftTreeClampsForgedRealSize<S>();
}

TEMPLATE_TEST_CASE_SIG("MftTree skips an extension record of $MFT",
                       "[mft-tree][regression]", ((Strategy S), S),
                       Strategy::NoCache, Strategy::FullCache) {
  RunMftTreeSkipsMftExtensionRecord<S>();
}

TEMPLATE_TEST_CASE_SIG(
    "MftTree rebuilds paths from $FILE_NAME parent references", "[mft-tree]",
    ((Strategy S), S), Strategy::NoCache, Strategy::FullCache) {
  RunMftTreeRebuildsPaths<S>();
}

TEMPLATE_TEST_CASE_SIG("MftTree drops freed records when asked", "[mft-tree]",
                       ((Strategy S), S), Strategy::NoCache,
                       Strategy::FullCache) {
  RunMftTreeWithoutDeleted<S>();
}

TEMPLATE_TEST_CASE_SIG(
    "MftTree drops a record its FileRecord could not parse by default",
    "[mft-tree][regression]", ((Strategy S), S), Strategy::NoCache,
    Strategy::FullCache) {
  RunMftTreeDropsUnrecoveredRecord<S>();
}

TEMPLATE_TEST_CASE_SIG("MftTree stops when progress returns false",
                       "[mft-tree]", ((Strategy S), S), Strategy::NoCache,
                       Strategy::FullCache) {
  RunMftTreeProgressStops<S>();
}

TEMPLATE_TEST_CASE_SIG("MftTree logs no warning for never-used record slots",
                       "[mft-tree]", ((Strategy S), S), Strategy::NoCache,
                       Strategy::FullCache) {
  const std::shared_ptr<spdlog::logger> logger =
      spdlog::get(std::string(NtfsBrowser::Log::logger_name));
  REQUIRE(logger);

  std::ostringstream out;
  const auto sink = std::make_shared<spdlog::sinks::ostream_sink_st>(out);
  sink->set_pattern("%l %v");
  logger->sinks().push_back(sink);
  {
    const auto volume = OpenMftTreeVolume<S>();
    const MftTree tree(*volume);
  }
  logger->sinks().pop_back();

  CHECK_THAT(out.str(), ContainsSubstring("debug Invalid file record"));
  CHECK_THAT(out.str(), !ContainsSubstring("warning"));
}
