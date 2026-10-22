#include <ntfs-browser/win-types.h>

#include <algorithm>
#include <cstddef>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include <ntfs-browser/attr/type.h>
#include <ntfs-browser/cache/strategy.h>
#include <ntfs-browser/mft/tree.h>
#include <ntfs-browser/namespace.h>
#include <ntfs-browser/ntfs-volume.h>

#include "memory-disk-reader.h"
#include "volume-tree-support.h"

using NTFS_BROWSER_NAMESPACE::NtfsVolume;
using NTFS_BROWSER_NAMESPACE::Mft::MftTree;
namespace Attr = NTFS_BROWSER_NAMESPACE::Attr;
namespace Cache = NTFS_BROWSER_NAMESPACE::Cache;
namespace Mft = NTFS_BROWSER_NAMESPACE::Mft;
using NTFS_BROWSER_NAMESPACE::Tests::MemoryDiskReader;

namespace {

// Offset of the name space BYTE in a $FILE_NAME body (Data::Filename).
constexpr size_t filename_name_space_field = 65;

// Name space value of a DOS-only $FILE_NAME, which MftTree treats as an alias.
constexpr BYTE filename_dos_namespace = 2;

// Offset of the name length in a $FILE_NAME body (Data::Filename).
constexpr size_t filename_name_length_field = 64;

// Mask for the 48-bit record number inside a file reference.
constexpr ULONGLONG ref_record_mask = 0x0000FFFFFFFFFFFFULL;

// Sequence number the MFT tree fixture gives its root directory.
constexpr ULONGLONG mft_tree_root_sequence = 5;

// Builds a file reference from a record number and a sequence number.
[[nodiscard]] constexpr ULONGLONG MakeRef(ULONGLONG record,
                                          ULONGLONG sequence) {
  return record | (sequence << 48U);
}

// Repoints the record number of every $FILE_NAME of idx, keeping its sequence.
void SetFileNameParents(std::vector<BYTE>& image, ULONGLONG idx,
                        ULONGLONG record_number) {
  for (const size_t attr : AttrOffsets(image, idx, Attr::Type::FileName)) {
    const size_t body = ResidentBody(image, attr);
    const auto ref = Load<ULONGLONG>(image, body);
    Store<ULONGLONG>(image, body, (ref & ~ref_record_mask) | record_number);
  }
}

// Writes new_ref over every $FILE_NAME of idx that names from_record.
void RedirectFileNames(std::vector<BYTE>& image, ULONGLONG idx,
                       ULONGLONG from_record, ULONGLONG new_ref) {
  for (const size_t attr : AttrOffsets(image, idx, Attr::Type::FileName)) {
    const size_t body = ResidentBody(image, attr);
    if ((Load<ULONGLONG>(image, body) & ref_record_mask) == from_record) {
      Store<ULONGLONG>(image, body, new_ref);
    }
  }
}

// Opens the MFT tree fixture as a FullCache volume.
std::unique_ptr<NtfsVolume<Cache::Strategy::FullCache>> OpenMftTree() {
  auto volume = std::make_unique<NtfsVolume<Cache::Strategy::FullCache>>(
      std::make_unique<MemoryDiskReader>(
          NTFS_BROWSER_NAMESPACE::Tests::BuildFakeNtfsImageWithMftTree()));
  REQUIRE(volume->IsVolumeOK());
  return volume;
}

}  // namespace

TEST_CASE("MftTree copies and moves its scanned namespace", "[cov-vol]") {
  const auto volume = OpenMftTree();
  const NTFS_BROWSER_NAMESPACE::Mft::MftTree source(*volume);

  NTFS_BROWSER_NAMESPACE::Mft::MftTree copied(source);
  CHECK(copied.Entries().size() == source.Entries().size());

  NTFS_BROWSER_NAMESPACE::Mft::MftTree assigned(*volume);
  assigned = copied;
  CHECK(assigned.Entries().size() == source.Entries().size());

  NTFS_BROWSER_NAMESPACE::Mft::MftTree moved(std::move(copied));
  CHECK(moved.Entries().size() == source.Entries().size());

  NTFS_BROWSER_NAMESPACE::Mft::MftTree target(*volume);
  target = std::move(moved);
  CHECK(target.Find(NTFS_BROWSER_NAMESPACE::Tests::mft_tree_docs_idx) !=
        nullptr);
}

TEST_CASE("MftTree skips a file name with no characters", "[cov-vol]") {
  std::vector<BYTE> image =
      NTFS_BROWSER_NAMESPACE::Tests::BuildFakeNtfsImageWithMftTree();
  // Only the first $FILE_NAME of report.txt loses its name; its DOS alias
  // stays.
  Store<BYTE>(
      image,
      ResidentBody(image,
                   FirstAttr(image,
                             NTFS_BROWSER_NAMESPACE::Tests::mft_tree_report_idx,
                             Attr::Type::FileName)) +
          filename_name_length_field,
      0);
  const auto volume = std::make_unique<NtfsVolume<Cache::Strategy::FullCache>>(
      std::make_unique<MemoryDiskReader>(std::move(image)));
  REQUIRE(volume->IsVolumeOK());

  const NTFS_BROWSER_NAMESPACE::Mft::MftTree tree(*volume);

  const NTFS_BROWSER_NAMESPACE::Mft::MftTree::Entry* entry =
      tree.Find(NTFS_BROWSER_NAMESPACE::Tests::mft_tree_report_idx);
  REQUIRE(entry != nullptr);
  CHECK(entry->names.size() == 1);
}

TEST_CASE("MftTree keeps a record whose names are all DOS aliases",
          "[cov-vol]") {
  std::vector<BYTE> image =
      NTFS_BROWSER_NAMESPACE::Tests::BuildFakeNtfsImageWithMftTree();
  for (const size_t attr :
       AttrOffsets(image, NTFS_BROWSER_NAMESPACE::Tests::mft_tree_report_idx,
                   Attr::Type::FileName)) {
    Store<BYTE>(image, ResidentBody(image, attr) + filename_name_space_field,
                filename_dos_namespace);
  }
  const auto volume = std::make_unique<NtfsVolume<Cache::Strategy::FullCache>>(
      std::make_unique<MemoryDiskReader>(std::move(image)));
  REQUIRE(volume->IsVolumeOK());

  const NTFS_BROWSER_NAMESPACE::Mft::MftTree tree(*volume);

  CHECK_FALSE(
      tree.GetPath(NTFS_BROWSER_NAMESPACE::Tests::mft_tree_report_idx).empty());
}

TEST_CASE("MftTree reports a lost ancestor that has no usable name",
          "[cov-vol]") {
  std::vector<BYTE> image =
      NTFS_BROWSER_NAMESPACE::Tests::BuildFakeNtfsImageWithMftTree();
  // Docs keeps its directory flag and sequence, but loses every name.
  for (const size_t attr :
       AttrOffsets(image, NTFS_BROWSER_NAMESPACE::Tests::mft_tree_docs_idx,
                   Attr::Type::FileName)) {
    Store<BYTE>(image, ResidentBody(image, attr) + filename_name_length_field,
                0);
  }
  const auto volume = std::make_unique<NtfsVolume<Cache::Strategy::FullCache>>(
      std::make_unique<MemoryDiskReader>(std::move(image)));
  REQUIRE(volume->IsVolumeOK());

  const NTFS_BROWSER_NAMESPACE::Mft::MftTree tree(*volume);

  std::optional<ULONGLONG> lost;
  static_cast<void>(
      tree.GetPath(NTFS_BROWSER_NAMESPACE::Tests::mft_tree_report_idx, &lost));
  CHECK(lost == NTFS_BROWSER_NAMESPACE::Tests::mft_tree_docs_idx);
}

TEST_CASE("MftTree marks a name invalid when its parent is not a directory",
          "[cov-vol]") {
  std::vector<BYTE> image =
      NTFS_BROWSER_NAMESPACE::Tests::BuildFakeNtfsImageWithMftTree();
  SetFileNameParents(image, NTFS_BROWSER_NAMESPACE::Tests::mft_tree_report_idx,
                     NTFS_BROWSER_NAMESPACE::Tests::mft_tree_hard_link_idx);
  const auto volume = std::make_unique<NtfsVolume<Cache::Strategy::FullCache>>(
      std::make_unique<MemoryDiskReader>(std::move(image)));
  REQUIRE(volume->IsVolumeOK());

  const NTFS_BROWSER_NAMESPACE::Mft::MftTree tree(*volume);

  const NTFS_BROWSER_NAMESPACE::Mft::MftTree::Entry* entry =
      tree.Find(NTFS_BROWSER_NAMESPACE::Tests::mft_tree_report_idx);
  REQUIRE(entry != nullptr);
  for (const NTFS_BROWSER_NAMESPACE::Mft::MftTree::Name& name : entry->names) {
    CHECK_FALSE(name.parent_valid);
  }
  CHECK_FALSE(
      tree.IsReachable(NTFS_BROWSER_NAMESPACE::Tests::mft_tree_report_idx));
}

TEST_CASE("MftTree marks a name invalid when its parent is not in the scan",
          "[cov-vol]") {
  std::vector<BYTE> image =
      NTFS_BROWSER_NAMESPACE::Tests::BuildFakeNtfsImageWithMftTree();
  SetFileNameParents(image, NTFS_BROWSER_NAMESPACE::Tests::mft_tree_report_idx,
                     NTFS_BROWSER_NAMESPACE::Tests::mft_tree_zeroed_idx);
  const auto volume = std::make_unique<NtfsVolume<Cache::Strategy::FullCache>>(
      std::make_unique<MemoryDiskReader>(std::move(image)));
  REQUIRE(volume->IsVolumeOK());

  const NTFS_BROWSER_NAMESPACE::Mft::MftTree tree(*volume);

  const NTFS_BROWSER_NAMESPACE::Mft::MftTree::Entry* entry =
      tree.Find(NTFS_BROWSER_NAMESPACE::Tests::mft_tree_report_idx);
  REQUIRE(entry != nullptr);
  for (const NTFS_BROWSER_NAMESPACE::Mft::MftTree::Name& name : entry->names) {
    CHECK_FALSE(name.parent_valid);
  }
  std::optional<ULONGLONG> lost;
  static_cast<void>(
      tree.GetPath(NTFS_BROWSER_NAMESPACE::Tests::mft_tree_report_idx, &lost));
  CHECK(lost == NTFS_BROWSER_NAMESPACE::Tests::mft_tree_zeroed_idx);
}

TEST_CASE("MftTree files a record once when two of its names share a parent",
          "[cov-vol]") {
  std::vector<BYTE> image =
      NTFS_BROWSER_NAMESPACE::Tests::BuildFakeNtfsImageWithMftTree();
  // link-b moves from Docs to the root, so both names of record 18 name the
  // root.
  RedirectFileNames(image,
                    NTFS_BROWSER_NAMESPACE::Tests::mft_tree_hard_link_idx,
                    NTFS_BROWSER_NAMESPACE::Tests::mft_tree_docs_idx,
                    MakeRef(root_idx, mft_tree_root_sequence));
  const auto volume = std::make_unique<NtfsVolume<Cache::Strategy::FullCache>>(
      std::make_unique<MemoryDiskReader>(std::move(image)));
  REQUIRE(volume->IsVolumeOK());

  const NTFS_BROWSER_NAMESPACE::Mft::MftTree tree(*volume);

  const auto children = tree.Children(root_idx);
  CHECK(std::count(children.begin(), children.end(),
                   NTFS_BROWSER_NAMESPACE::Tests::mft_tree_hard_link_idx) == 1);
}

TEST_CASE("MftTree GetPath reports nothing for an unknown record or name",
          "[cov-vol]") {
  const auto volume = OpenMftTree();
  const NTFS_BROWSER_NAMESPACE::Mft::MftTree tree(*volume);

  CHECK_FALSE(tree.IsReachable(999));

  std::optional<ULONGLONG> lost = root_idx;
  CHECK(tree.GetPath(999, &lost).empty());
  CHECK_FALSE(lost.has_value());

  lost = root_idx;
  CHECK(
      tree.GetPath(NTFS_BROWSER_NAMESPACE::Tests::mft_tree_docs_idx, 99, &lost)
          .empty());
  CHECK_FALSE(lost.has_value());
}

TEST_CASE("MftTree stops a path at a loop of parent references", "[cov-vol]") {
  // Sequence numbers the fixture gives Docs and NewDir.
  constexpr WORD docs_sequence = 1;
  constexpr WORD new_dir_sequence = 7;

  std::vector<BYTE> image =
      NTFS_BROWSER_NAMESPACE::Tests::BuildFakeNtfsImageWithMftTree();
  // Docs and NewDir each name the other as parent, so neither reaches the root.
  RedirectFileNames(
      image, NTFS_BROWSER_NAMESPACE::Tests::mft_tree_docs_idx, root_idx,
      MakeRef(NTFS_BROWSER_NAMESPACE::Tests::mft_tree_reused_dir_idx,
              new_dir_sequence));
  RedirectFileNames(
      image, NTFS_BROWSER_NAMESPACE::Tests::mft_tree_reused_dir_idx, root_idx,
      MakeRef(NTFS_BROWSER_NAMESPACE::Tests::mft_tree_docs_idx, docs_sequence));
  const auto volume = std::make_unique<NtfsVolume<Cache::Strategy::FullCache>>(
      std::make_unique<MemoryDiskReader>(std::move(image)));
  REQUIRE(volume->IsVolumeOK());

  const NTFS_BROWSER_NAMESPACE::Mft::MftTree tree(*volume);

  std::optional<ULONGLONG> lost;
  const std::wstring path =
      tree.GetPath(NTFS_BROWSER_NAMESPACE::Tests::mft_tree_docs_idx, &lost);
  // The walk stops where the loop closes, at the record it re-enters.
  CHECK(lost == NTFS_BROWSER_NAMESPACE::Tests::mft_tree_docs_idx);
  CHECK(path == L"NewDir\\Docs");
  CHECK_FALSE(
      tree.IsReachable(NTFS_BROWSER_NAMESPACE::Tests::mft_tree_docs_idx));
}
