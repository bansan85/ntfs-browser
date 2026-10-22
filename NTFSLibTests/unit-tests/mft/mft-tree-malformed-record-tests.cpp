#include <ntfs-browser/win-types.h>

#include <array>
#include <cstring>
#include <memory>
#include <set>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include <catch2/catch_template_test_macros.hpp>
#include <catch2/catch_test_macros.hpp>

#include <ntfs-browser/attr/type.h>
#include <ntfs-browser/cache/strategy.h>
#include <ntfs-browser/mft/tree.h>
#include <ntfs-browser/namespace.h>
#include <ntfs-browser/ntfs-volume.h>
#include <ntfs-browser/volume-options.h>

#include "catch2/catch_message.hpp"
#include "fake-ntfs-image.h"
#include "memory-disk-reader.h"
#include "scratch-record.h"

namespace Attr = NTFS_BROWSER_NAMESPACE::Attr;
namespace Cache = NTFS_BROWSER_NAMESPACE::Cache;
namespace Mft = NTFS_BROWSER_NAMESPACE::Mft;
using NTFS_BROWSER_NAMESPACE::NtfsVolume;
using NTFS_BROWSER_NAMESPACE::VolumeOptions;
using NTFS_BROWSER_NAMESPACE::Tests::BuildFakeNtfsImageWithMftTree;
using NTFS_BROWSER_NAMESPACE::Tests::MemoryDiskReader;
using NTFS_BROWSER_NAMESPACE::Tests::mft_tree_hard_link_idx;
using NTFS_BROWSER_NAMESPACE::Tests::mft_tree_report_idx;
using NTFS_BROWSER_NAMESPACE::Tests::mft_tree_reused_dir_idx;
using NTFS_BROWSER_NAMESPACE::Tests::ScratchFileNameBody;
using NTFS_BROWSER_NAMESPACE::Tests::ScratchHeader;
using NTFS_BROWSER_NAMESPACE::Tests::ScratchRecord;
using NTFS_BROWSER_NAMESPACE::Tests::ScratchStdInfoBody;

namespace {

// The root directory at the sequence number 5 the fake root has.
constexpr ULONGLONG root_reference = 5ULL | (5ULL << 48U);

// Bytes of the $DATA of the short-header record.
constexpr std::array<BYTE, 3> short_content{'a', 'b', 'c'};

// Offset of the update sequence array of the NTFS 3.0 short header, and of the
// first attribute that follows it.
constexpr WORD short_header_us_offset = 0x2A;
constexpr WORD short_header_attr_offset = 0x30;

// Scans the image under the options. The tree is owned data: it outlives the
// volume.
template <Cache::Strategy S>
Mft::MftTree Scan(std::vector<BYTE> image, const VolumeOptions& options) {
  const NtfsVolume<S> volume(
      std::make_unique<MemoryDiskReader>(std::move(image)), options);
  REQUIRE(volume.IsVolumeOK());
  return Mft::MftTree(volume);
}

// The record numbers of the entries of a tree.
std::set<ULONGLONG> Records(const Mft::MftTree& tree) {
  std::set<ULONGLONG> records;
  for (const Mft::MftTree::Entry& entry : tree.Entries()) {
    records.insert(entry.record);
  }
  return records;
}

// Overwrites the first four bytes of record idx in the image of the tree
// fixture.
void SetSignature(std::vector<BYTE>& image, ULONGLONG idx,
                  const std::array<char, 4>& signature) {
  std::memcpy(
      &image.at(NTFS_BROWSER_NAMESPACE::Tests::fake_cluster_size +
                idx * NTFS_BROWSER_NAMESPACE::Tests::fake_file_record_size),
      signature.data(), signature.size());
}

// Scans the tree fixture with a scratch record at mft_tree_report_idx, once
// for each mode, and returns how many records each scan counted as damaged,
// strict first. The scratch record is placed after edit changed the image.
template <Cache::Strategy S, typename Edit>
std::pair<ULONGLONG, ULONGLONG> DamagedCounts(const ScratchRecord& scratch,
                                              Edit edit) {
  std::vector<BYTE> image = BuildFakeNtfsImageWithMftTree();
  scratch.Place(image, mft_tree_report_idx);
  edit(image);
  return {
      Scan<S>(image, VolumeOptions{.recover_errors = false}).Stats().damaged,
      Scan<S>(image, VolumeOptions{.recover_errors = true}).Stats().damaged};
}

}  // namespace

TEMPLATE_TEST_CASE_SIG(
    "A record that does not start with FILE is unreadable, whatever its "
    "signature says",
    "[mft-tree][malformed-record]", ((Cache::Strategy S), S),
    Cache::Strategy::NoCache, Cache::Strategy::FullCache) {
  const Mft::MftTree baseline =
      Scan<S>(BuildFakeNtfsImageWithMftTree(), VolumeOptions{});

  std::vector<BYTE> image = BuildFakeNtfsImageWithMftTree();
  // BAAD is what NTFS writes over a record it found damaged, CHKD what
  // chkdsk leaves, and INDX is an index block's signature.
  SetSignature(image, mft_tree_report_idx, {'B', 'A', 'A', 'D'});
  SetSignature(image, mft_tree_hard_link_idx, {'C', 'H', 'K', 'D'});
  SetSignature(image, mft_tree_reused_dir_idx, {'I', 'N', 'D', 'X'});

  for (const bool recover : {false, true}) {
    CAPTURE(recover);
    const Mft::MftTree tree =
        Scan<S>(image, VolumeOptions{.recover_errors = recover});

    std::set<ULONGLONG> expected = Records(baseline);
    for (const ULONGLONG record : {mft_tree_report_idx, mft_tree_hard_link_idx,
                                   mft_tree_reused_dir_idx}) {
      expected.erase(record);
    }
    CHECK(Records(tree) == expected);

    const Mft::MftTree::ScanStats& stats = tree.Stats();
    const Mft::MftTree::ScanStats& before = baseline.Stats();
    CHECK(stats.slots == before.slots);
    CHECK(stats.unreadable == before.unreadable + 3);
    CHECK(stats.in_use == before.in_use - 3);
    CHECK(stats.deleted == before.deleted);
    CHECK(stats.damaged == before.damaged);
    CHECK(stats.complete);
  }
}

TEMPLATE_TEST_CASE_SIG(
    "A record with the NTFS 3.0 short header is an entry of the scan",
    "[mft-tree][malformed-record]", ((Cache::Strategy S), S),
    Cache::Strategy::NoCache, Cache::Strategy::FullCache) {
  const auto std_info = ScratchStdInfoBody();
  const auto name = ScratchFileNameBody(L"short.txt", root_reference);
  ScratchRecord scratch(
      ScratchHeader{.offset_of_us = short_header_us_offset,
                    .size_of_us = 3,
                    .offset_of_attr = short_header_attr_offset,
                    .sequence = 7});
  scratch.Resident(Attr::Type::StandardInformation, std_info)
      .Resident(Attr::Type::FileName, name)
      .Resident(Attr::Type::Data, short_content)
      .End();
  std::vector<BYTE> image = BuildFakeNtfsImageWithMftTree();
  scratch.Place(image, mft_tree_report_idx);

  for (const bool recover : {false, true}) {
    CAPTURE(recover);
    const Mft::MftTree tree =
        Scan<S>(image, VolumeOptions{.recover_errors = recover});
    const Mft::MftTree::Entry* entry = tree.Find(mft_tree_report_idx);
    REQUIRE(entry != nullptr);
    CHECK(entry->in_use);
    CHECK(entry->sequence == 7);
    CHECK(entry->size == short_content.size());
    REQUIRE(entry->names.size() == 1);
    CHECK(entry->names.front().name == L"short.txt");
    CHECK(entry->names.front().parent_record == 5);
    CHECK(tree.GetPath(mft_tree_report_idx) == L"\\short.txt");
  }
}

TEMPLATE_TEST_CASE_SIG(
    "A run list that overruns its attribute is one damaged record in either "
    "mode",
    "[mft-tree][malformed-record]", ((Cache::Strategy S), S),
    Cache::Strategy::NoCache, Cache::Strategy::FullCache) {
  const auto std_info = ScratchStdInfoBody();
  const auto name = ScratchFileNameBody(L"cut.bin", root_reference);
  // Two clusters at LCN 8, in an attribute that declares one.
  constexpr std::array<BYTE, 4> run_list{0x11, 0x02, 0x08, 0x00};
  const auto build = [&](ULONGLONG declared_clusters) {
    ScratchRecord scratch;
    scratch.Resident(Attr::Type::StandardInformation, std_info)
        .Resident(Attr::Type::FileName, name)
        .NonResident(Attr::Type::Data, run_list, declared_clusters, 1024)
        .End();
    return scratch;
  };
  const auto nothing = [](std::vector<BYTE>&) {};

  // The same record with a declared size that fits its run is not damaged.
  const auto [sound_strict, sound_recovering] =
      DamagedCounts<S>(build(2), nothing);
  CHECK(sound_strict == sound_recovering);

  const auto [strict, recovering] = DamagedCounts<S>(build(1), nothing);
  CHECK(strict == sound_strict + 1);
  CHECK(recovering == strict);
}

TEMPLATE_TEST_CASE_SIG(
    "A record with one failed sector is one damaged record in either mode",
    "[mft-tree][malformed-record]", ((Cache::Strategy S), S),
    Cache::Strategy::NoCache, Cache::Strategy::FullCache) {
  const auto std_info = ScratchStdInfoBody();
  const auto name = ScratchFileNameBody(L"torn.bin", root_reference);
  ScratchRecord scratch;
  scratch.Resident(Attr::Type::StandardInformation, std_info)
      .Resident(Attr::Type::FileName, name)
      .Resident(Attr::Type::Data, short_content)
      .End();

  const auto [sound_strict, sound_recovering] =
      DamagedCounts<S>(scratch, [](std::vector<BYTE>&) {});
  CHECK(sound_strict == sound_recovering);

  // The last word of the first sector is no longer the update sequence
  // number.
  const auto tear = [](std::vector<BYTE>& image) {
    constexpr std::size_t first_sector_end = 510;
    image.at(NTFS_BROWSER_NAMESPACE::Tests::fake_cluster_size +
             (mft_tree_report_idx *
              NTFS_BROWSER_NAMESPACE::Tests::fake_file_record_size) +
             first_sector_end) = 0x77;
  };
  const auto [strict, recovering] = DamagedCounts<S>(scratch, tear);
  CHECK(strict == sound_strict + 1);
  CHECK(recovering == strict);
}
