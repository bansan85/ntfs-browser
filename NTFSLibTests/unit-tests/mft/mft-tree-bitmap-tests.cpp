#include <ntfs-browser/win-types.h>

#include <algorithm>
#include <array>
#include <memory>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <catch2/catch_template_test_macros.hpp>
#include <catch2/catch_test_macros.hpp>

#include <ntfs-browser/cache/strategy.h>
#include <ntfs-browser/disk-reader.h>
#include <ntfs-browser/mft/tree.h>
#include <ntfs-browser/namespace.h>
#include <ntfs-browser/ntfs-volume.h>
#include <ntfs-browser/volume-options.h>

#include "catch2/catch_message.hpp"
#include "fake-ntfs-image.h"
#include "memory-disk-reader.h"

namespace Cache = NTFS_BROWSER_NAMESPACE::Cache;
using NTFS_BROWSER_NAMESPACE::NtfsVolume;
using NTFS_BROWSER_NAMESPACE::VolumeOptions;
using NTFS_BROWSER_NAMESPACE::Mft::MftTree;
using NTFS_BROWSER_NAMESPACE::Tests::BuildFakeNtfsImageWithMftBitmap;
using NTFS_BROWSER_NAMESPACE::Tests::BuildFakeNtfsImageWithMftTree;
using NTFS_BROWSER_NAMESPACE::Tests::fake_file_record_size;
using NTFS_BROWSER_NAMESPACE::Tests::FakeMftBitmap;
using NTFS_BROWSER_NAMESPACE::Tests::MemoryDiskReader;
using NTFS_BROWSER_NAMESPACE::Tests::mft_tree_bitmap_used_idx;
using NTFS_BROWSER_NAMESPACE::Tests::mft_tree_deleted_file_idx;
using NTFS_BROWSER_NAMESPACE::Tests::mft_tree_docs_idx;
using NTFS_BROWSER_NAMESPACE::Tests::mft_tree_record_count;
using NTFS_BROWSER_NAMESPACE::Tests::mft_tree_report_idx;

namespace {

// Byte range of one read the volume asked of the disk.
struct ReadSpan {
  ULONGLONG begin;
  ULONGLONG end;
};

// Serves a memory image and logs the range of every read that reaches it.
class LoggingDiskReader final : public NTFS_BROWSER_NAMESPACE::IDiskReader {
 public:
  LoggingDiskReader(std::vector<BYTE> data,
                    std::shared_ptr<std::vector<ReadSpan>> reads)
      : inner_(std::move(data)), reads_(std::move(reads)) {}

  bool Open(std::wstring_view path) override { return inner_.Open(path); }

  [[nodiscard]] bool ReadInto(LARGE_INTEGER& addr,
                              std::span<BYTE> dest) const override {
    const auto begin = static_cast<ULONGLONG>(addr.QuadPart);
    reads_->push_back({begin, begin + dest.size()});
    return inner_.ReadInto(addr, dest);
  }

 private:
  MemoryDiskReader inner_;
  std::shared_ptr<std::vector<ReadSpan>> reads_;
};

// Records of the fixture the bitmap marks used.
bool IsUsedIdx(ULONGLONG idx) {
  return std::ranges::find(mft_tree_bitmap_used_idx, idx) !=
         mft_tree_bitmap_used_idx.end();
}

// Record slots of the fixture, as many as there are records.
constexpr ULONGLONG slot_count = mft_tree_record_count;

// Slots the accurate bitmap marks free.
constexpr ULONGLONG free_slot_count =
    slot_count - mft_tree_bitmap_used_idx.size();

template <Cache::Strategy S>
std::unique_ptr<NtfsVolume<S>> Open(std::vector<BYTE> image,
                                    const VolumeOptions& options = {}) {
  auto volume = std::make_unique<NtfsVolume<S>>(
      std::make_unique<MemoryDiskReader>(std::move(image)), options);
  REQUIRE(volume->IsVolumeOK());
  REQUIRE(volume->GetRecordsCount() == slot_count);
  return volume;
}

// One line per entry, with its path and the fields a lost record would change.
std::vector<std::wstring> Describe(const MftTree& tree) {
  std::vector<std::wstring> lines;
  for (const MftTree::Entry& entry : tree.Entries()) {
    std::wstring line =
        std::to_wstring(entry.record) + L"|" + std::to_wstring(entry.sequence) +
        L"|" + tree.GetPath(entry.record) + L"|" +
        std::to_wstring(entry.in_use) + std::to_wstring(entry.directory) +
        L"|" + std::to_wstring(entry.size) + L"|" +
        std::to_wstring(entry.allocated_size) + L"|" +
        std::to_wstring(tree.IsReachable(entry.record));
    for (const MftTree::Name& name : entry.names) {
      line += L"|" + name.name + L"@" + std::to_wstring(name.parent_record) +
              std::to_wstring(name.parent_valid);
    }
    lines.push_back(std::move(line));
  }
  return lines;
}

template <Cache::Strategy S>
void RunBitmapSkipsFreeSlots(FakeMftBitmap kind) {
  const auto plain = Open<S>(BuildFakeNtfsImageWithMftTree());
  const MftTree reference(*plain);

  const auto volume = Open<S>(BuildFakeNtfsImageWithMftBitmap(kind));
  const MftTree tree(*volume);

  // Every entry the full scan kept in strict mode is still there, identical.
  CHECK(Describe(tree) == Describe(reference));
  CHECK(tree.Entries().size() == 7);

  const MftTree::ScanStats& stats = tree.Stats();
  CHECK(stats.slots == slot_count);
  CHECK(stats.in_use == reference.Stats().in_use);
  CHECK(stats.extensions == reference.Stats().extensions);
  CHECK(stats.unreachable == reference.Stats().unreachable);
  // The freed and the never used slots are skipped, not classified.
  CHECK(stats.skipped == free_slot_count);
  CHECK(stats.deleted == 0);
  CHECK(stats.unreadable == 0);
  CHECK(stats.damaged == 0);
  CHECK(stats.complete);
  // Every slot is counted once.
  CHECK(stats.in_use + stats.deleted + stats.extensions + stats.unreadable +
            stats.skipped ==
        stats.slots);
}

template <Cache::Strategy S>
void RunBitmapFallsBackToEverySlot(FakeMftBitmap kind) {
  const auto plain = Open<S>(BuildFakeNtfsImageWithMftTree());
  const MftTree reference(*plain);

  const auto volume = Open<S>(BuildFakeNtfsImageWithMftBitmap(kind));
  const MftTree tree(*volume);

  CHECK(Describe(tree) == Describe(reference));
  const MftTree::ScanStats& stats = tree.Stats();
  CHECK(stats.skipped == 0);
  CHECK(stats.in_use == reference.Stats().in_use);
  CHECK(stats.deleted == reference.Stats().deleted);
  CHECK(stats.extensions == reference.Stats().extensions);
  CHECK(stats.unreadable == reference.Stats().unreadable);
}

template <Cache::Strategy S>
void RunBitmapIgnoredWithIncludeDeleted(FakeMftBitmap kind) {
  const VolumeOptions options{.include_deleted = true};
  const auto plain = Open<S>(BuildFakeNtfsImageWithMftTree(), options);
  const MftTree reference(*plain);

  const auto volume = Open<S>(BuildFakeNtfsImageWithMftBitmap(kind), options);
  const MftTree tree(*volume);

  CHECK(Describe(tree) == Describe(reference));
  CHECK(tree.Entries().size() == 11);
  const MftTree::ScanStats& stats = tree.Stats();
  CHECK(stats.skipped == 0);
  CHECK(stats.in_use == 7);
  CHECK(stats.deleted == 4);
  CHECK(stats.unreadable == 14);
}

// Every slot that overlaps a logged read.
std::vector<bool> SlotsRead(const std::vector<ReadSpan>& reads) {
  const ULONGLONG mft_addr = NTFS_BROWSER_NAMESPACE::Tests::fake_cluster_size;
  std::vector<bool> touched(slot_count, false);
  for (ULONGLONG idx = 0; idx < slot_count; idx++) {
    const ULONGLONG begin = mft_addr + idx * fake_file_record_size;
    const ULONGLONG end = begin + fake_file_record_size;
    touched.at(idx) = std::ranges::any_of(reads, [&](const ReadSpan& read) {
      return read.begin < end && read.end > begin;
    });
  }
  return touched;
}

std::vector<ReadSpan> ScanAndLogReads(FakeMftBitmap kind,
                                      const VolumeOptions& options) {
  auto reads = std::make_shared<std::vector<ReadSpan>>();
  NtfsVolume<Cache::Strategy::NoCache> volume(
      std::make_unique<LoggingDiskReader>(BuildFakeNtfsImageWithMftBitmap(kind),
                                          reads),
      options);
  REQUIRE(volume.IsVolumeOK());
  reads->clear();

  const MftTree tree(volume);
  return *reads;
}

}  // namespace

TEMPLATE_TEST_CASE_SIG("MftTree skips the slots the $MFT bitmap marks free",
                       "[mft-tree][bitmap]", ((Cache::Strategy S), S),
                       Cache::Strategy::NoCache, Cache::Strategy::FullCache) {
  SECTION("Resident bitmap") {
    RunBitmapSkipsFreeSlots<S>(FakeMftBitmap::Resident);
  }
  SECTION("Non-resident bitmap") {
    RunBitmapSkipsFreeSlots<S>(FakeMftBitmap::NonResident);
  }
}

TEST_CASE("MftTree does not read the slots the $MFT bitmap marks free",
          "[mft-tree][bitmap]") {
  for (const FakeMftBitmap kind :
       {FakeMftBitmap::Resident, FakeMftBitmap::NonResident}) {
    const std::vector<bool> touched =
        SlotsRead(ScanAndLogReads(kind, VolumeOptions{}));
    // The used slots from Mft::Idx::User on share one block read, which also
    // covers the free slots between them.
    const ULONGLONG block_first = mft_tree_bitmap_used_idx[3];
    const ULONGLONG block_last = mft_tree_bitmap_used_idx.back();
    for (ULONGLONG idx = 0; idx < slot_count; idx++) {
      INFO("slot " << idx);
      CHECK(touched.at(idx) ==
            (IsUsedIdx(idx) || (idx >= block_first && idx <= block_last)));
    }
  }
}

TEST_CASE("MftTree reads every slot with include_deleted on, bitmap or not",
          "[mft-tree][bitmap]") {
  for (const FakeMftBitmap kind :
       {FakeMftBitmap::Resident, FakeMftBitmap::NonResident}) {
    const std::vector<bool> touched = SlotsRead(
        ScanAndLogReads(kind, VolumeOptions{.include_deleted = true}));
    for (ULONGLONG idx = 0; idx < slot_count; idx++) {
      INFO("slot " << idx);
      CHECK(touched.at(idx));
    }
  }
}

TEMPLATE_TEST_CASE_SIG(
    "MftTree ignores the $MFT bitmap with include_deleted on",
    "[mft-tree][bitmap]", ((Cache::Strategy S), S), Cache::Strategy::NoCache,
    Cache::Strategy::FullCache) {
  SECTION("Resident bitmap") {
    RunBitmapIgnoredWithIncludeDeleted<S>(FakeMftBitmap::Resident);
  }
  SECTION("Non-resident bitmap") {
    RunBitmapIgnoredWithIncludeDeleted<S>(FakeMftBitmap::NonResident);
  }
}

TEMPLATE_TEST_CASE_SIG(
    "MftTree reads every slot when the $MFT bitmap cannot be used",
    "[mft-tree][bitmap]", ((Cache::Strategy S), S), Cache::Strategy::NoCache,
    Cache::Strategy::FullCache) {
  SECTION("Absent bitmap") {
    const auto plain = Open<S>(BuildFakeNtfsImageWithMftTree());
    const MftTree tree(*plain);
    CHECK(tree.Stats().skipped == 0);
    CHECK(tree.Stats().unreadable == 14);
  }
  SECTION("Bitmap too short for the slots") {
    RunBitmapFallsBackToEverySlot<S>(FakeMftBitmap::TooShort);
  }
  SECTION("Bitmap that cannot be read") {
    RunBitmapFallsBackToEverySlot<S>(FakeMftBitmap::Unreadable);
  }
}

TEMPLATE_TEST_CASE_SIG("MftTree survives a $MFT bitmap that lies",
                       "[mft-tree][bitmap]", ((Cache::Strategy S), S),
                       Cache::Strategy::NoCache, Cache::Strategy::FullCache) {
  const auto volume =
      Open<S>(BuildFakeNtfsImageWithMftBitmap(FakeMftBitmap::Lying));
  const MftTree tree(*volume);

  // An in-use record marked free is skipped, as the bitmap says.
  CHECK(tree.Find(mft_tree_report_idx) == nullptr);
  CHECK(tree.Find(mft_tree_docs_idx) != nullptr);
  // A freed record marked used is read, and dropped as freed.
  CHECK(tree.Find(mft_tree_deleted_file_idx) == nullptr);

  const MftTree::ScanStats& stats = tree.Stats();
  CHECK(stats.in_use == 6);
  CHECK(stats.deleted == 1);
  CHECK(stats.skipped == free_slot_count);
  CHECK(stats.complete);
}
