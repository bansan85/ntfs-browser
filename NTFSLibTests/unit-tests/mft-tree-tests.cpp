#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <spdlog/sinks/ostream_sink.h>
#include <spdlog/spdlog.h>

#include <ntfs-browser/log.h>
#include <ntfs-browser/mft-idx.h>
#include <ntfs-browser/mft-tree.h>
#include <ntfs-browser/ntfs-volume.h>
#include <ntfs-browser/strategy.h>

#include "fake-ntfs-image.h"
#include "memory-disk-reader.h"

using Catch::Matchers::ContainsSubstring;
using NtfsBrowser::MftEntry;
using NtfsBrowser::MftScanOptions;
using NtfsBrowser::MftScanStats;
using NtfsBrowser::MftTree;
using NtfsBrowser::NtfsVolume;
using NtfsBrowser::Strategy;
using NtfsBrowser::Enum::MftIdx;
using namespace NtfsBrowserTests;

namespace
{

constexpr ULONGLONG kRoot = static_cast<ULONGLONG>(MftIdx::ROOT);

// Opens BuildFakeNtfsImageWithMftTree() as a volume.
template <Strategy S>
std::unique_ptr<NtfsVolume<S>> OpenMftTreeVolume()
{
  auto volume = std::make_unique<NtfsVolume<S>>(
      std::make_unique<MemoryDiskReader>(BuildFakeNtfsImageWithMftTree()));
  REQUIRE(volume->IsVolumeOK());
  REQUIRE(volume->GetRecordsCount() == kMftTreeRecordCount);
  return volume;
}

// Children() as a vector, so Catch2 prints it on a mismatch.
std::vector<ULONGLONG> ChildrenOf(const MftTree& tree, ULONGLONG dir)
{
  const auto children = tree.Children(dir);
  return {children.begin(), children.end()};
}

template <Strategy S>
void RunMftTreeRebuildsPaths()
{
  const auto volume = OpenMftTreeVolume<S>();
  const MftTree tree(*volume);

  CHECK(tree.GetPath(kRoot) == L"\\");
  CHECK(tree.GetPath(static_cast<ULONGLONG>(MftIdx::MFT)) == L"\\$MFT");
  CHECK(tree.GetPath(kMftTreeDocsIdx) == L"\\Docs");

  SECTION("A DOS alias stays out of the path and the size comes from $DATA")
  {
    const MftEntry* report = tree.Find(kMftTreeReportIdx);
    REQUIRE(report != nullptr);
    CHECK(tree.GetPath(kMftTreeReportIdx) == L"\\Docs\\report.txt");
    REQUIRE(report->names.size() == 2);
    CHECK(report->names[1].dos_only);
    CHECK(report->size == kMftTreeReportDataSize);
    CHECK(report->read_only);
    CHECK(report->in_use);
    CHECK_FALSE(report->directory);
  }

  SECTION("Each hard link has its own path")
  {
    CHECK(tree.GetPath(kMftTreeHardLinkIdx) == L"\\link-a");
    CHECK(tree.GetPath(kMftTreeHardLinkIdx, 1) == L"\\Docs\\link-b");
  }

  SECTION("Children lists each record once, in record order")
  {
    CHECK(ChildrenOf(tree, kRoot) ==
          std::vector<ULONGLONG>{static_cast<ULONGLONG>(MftIdx::MFT),
                                 kMftTreeDocsIdx, kMftTreeHardLinkIdx,
                                 kMftTreeReusedDirIdx});
    CHECK(ChildrenOf(tree, kMftTreeDocsIdx) ==
          std::vector<ULONGLONG>{kMftTreeReportIdx, kMftTreeHardLinkIdx,
                                 kMftTreeDeletedFileIdx,
                                 kMftTreeDeletedDirIdx});
    CHECK(ChildrenOf(tree, kMftTreeDeletedDirIdx) ==
          std::vector<ULONGLONG>{kMftTreeDeletedChildIdx});
  }

  SECTION("A deleted subtree stays linked through the bumped sequence number")
  {
    const MftEntry* deleted = tree.Find(kMftTreeDeletedFileIdx);
    REQUIRE(deleted != nullptr);
    CHECK_FALSE(deleted->in_use);
    CHECK(tree.GetPath(kMftTreeDeletedFileIdx) == L"\\Docs\\old.tmp");
    CHECK(tree.GetPath(kMftTreeDeletedChildIdx) ==
          L"\\Docs\\OldDir\\draft.doc");
    CHECK(tree.IsReachable(kMftTreeDeletedChildIdx));
  }

  SECTION("A parent reference to a reused record breaks the path there")
  {
    std::optional<ULONGLONG> lost;
    CHECK(tree.GetPath(kMftTreeStaleChildIdx, &lost) == L"stale.txt");
    CHECK(lost == kMftTreeReusedDirIdx);
    CHECK_FALSE(tree.IsReachable(kMftTreeStaleChildIdx));
  }

  SECTION("Extension records and unused slots are not entries")
  {
    CHECK(tree.Find(kMftTreeExtensionIdx) == nullptr);
    CHECK(tree.Find(kMftTreeZeroedIdx) == nullptr);
  }

  SECTION("Stats count every slot")
  {
    const MftScanStats& stats = tree.Stats();
    CHECK(stats.slots == kMftTreeRecordCount);
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
void RunMftTreeWithoutDeleted()
{
  const auto volume = OpenMftTreeVolume<S>();
  const MftTree tree(*volume, MftScanOptions{.include_deleted = false});

  CHECK(tree.Find(kMftTreeDeletedFileIdx) == nullptr);
  CHECK(tree.Find(kMftTreeDeletedDirIdx) == nullptr);
  CHECK(tree.Find(kMftTreeStaleChildIdx) == nullptr);
  CHECK(ChildrenOf(tree, kMftTreeDocsIdx) ==
        std::vector<ULONGLONG>{kMftTreeReportIdx, kMftTreeHardLinkIdx});
  CHECK(tree.Entries().size() == 7);
  CHECK(tree.Stats().deleted == 4);
  CHECK(tree.Stats().unreachable == 1);
}

template <Strategy S>
void RunMftTreeProgressStops()
{
  const auto volume = OpenMftTreeVolume<S>();
  std::vector<ULONGLONG> calls;
  const MftTree tree(*volume,
                     MftScanOptions{.progress = [&](ULONGLONG done, ULONGLONG)
                                    {
                                      calls.push_back(done);
                                      return false;
                                    }});

  CHECK(calls == std::vector<ULONGLONG>{0});
  CHECK_FALSE(tree.Stats().complete);
  CHECK(tree.Entries().empty());
}

}  // namespace

TEST_CASE("MftTree rebuilds paths from $FILE_NAME parent references",
          "[mft-tree]")
{
  RunMftTreeRebuildsPaths<Strategy::NO_CACHE>();
}

TEST_CASE(
    "MftTree rebuilds paths from $FILE_NAME parent references (FULL_CACHE)",
    "[mft-tree]")
{
  RunMftTreeRebuildsPaths<Strategy::FULL_CACHE>();
}

TEST_CASE("MftTree drops freed records when asked", "[mft-tree]")
{
  RunMftTreeWithoutDeleted<Strategy::NO_CACHE>();
}

TEST_CASE("MftTree drops freed records when asked (FULL_CACHE)", "[mft-tree]")
{
  RunMftTreeWithoutDeleted<Strategy::FULL_CACHE>();
}

TEST_CASE("MftTree stops when progress returns false", "[mft-tree]")
{
  RunMftTreeProgressStops<Strategy::NO_CACHE>();
}

TEST_CASE("MftTree logs no warning for never-used record slots", "[mft-tree]")
{
  const std::shared_ptr<spdlog::logger> logger =
      spdlog::get(std::string(NtfsBrowser::Log::kLoggerName));
  REQUIRE(logger);

  std::ostringstream out;
  const auto sink = std::make_shared<spdlog::sinks::ostream_sink_st>(out);
  sink->set_pattern("%l %v");
  logger->sinks().push_back(sink);
  {
    const auto volume = OpenMftTreeVolume<Strategy::NO_CACHE>();
    const MftTree tree(*volume);
  }
  logger->sinks().pop_back();

  CHECK_THAT(out.str(), ContainsSubstring("debug Invalid file record"));
  CHECK_THAT(out.str(), !ContainsSubstring("warning"));
}
