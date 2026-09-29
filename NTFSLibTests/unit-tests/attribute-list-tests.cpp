#include <ntfs-browser/win-types.h>

#include <array>
#include <cstddef>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

#include <catch2/catch_template_test_macros.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <ntfs-browser/attr-base.h>
#include <ntfs-browser/data/attr-type.h>
#include <ntfs-browser/disk-reader.h>
#include <ntfs-browser/file-record.h>
#include <ntfs-browser/index-entry.h>
#include <ntfs-browser/mask.h>
#include <ntfs-browser/mft-idx.h>
#include <ntfs-browser/ntfs-volume.h>
#include <ntfs-browser/strategy.h>
#include <ntfs-browser/volume-options.h>

#include "fake-ntfs-image.h"
#include "memory-disk-reader.h"

using NtfsBrowser::AttrType;
using NtfsBrowser::FileRecord;
using NtfsBrowser::IndexEntry;
using NtfsBrowser::Mask;
using NtfsBrowser::NtfsVolume;
using NtfsBrowser::Strategy;
using NtfsBrowser::VolumeOptions;
using NtfsBrowser::Enum::MftIdx;

TEST_CASE("FindSubEntry follows $ATTRIBUTE_LIST to a relocated $INDEX_ROOT",
          "[file-record][regression]")
{
  auto reader = std::make_unique<NtfsBrowserTests::MemoryDiskReader>(
      NtfsBrowserTests::BuildFakeNtfsImageWithAttributeListDirectory());

  NtfsVolume<Strategy::FULL_CACHE> volume(std::move(reader));
  REQUIRE(volume.IsVolumeOK());

  FileRecord<Strategy::FULL_CACHE> dir(volume);
  dir.SetAttrMask(Mask::INDEX_ROOT | Mask::INDEX_ALLOCATION);

  REQUIRE(dir.ParseFileRecord(NtfsBrowserTests::kAttributeListDirIdx));
  REQUIRE(dir.ParseAttrs());

  CHECK_FALSE(dir.getAttr(AttrType::INDEX_ROOT).empty());

  const std::optional<IndexEntry> found = dir.FindSubEntry(L"Foo");
  REQUIRE(found.has_value());
  CHECK(found->GetFileReference() == 20);
}

TEST_CASE(
    "AttrList merges every attribute type relocated into the same "
    "extension record",
    "[file-record][regression]")
{
  auto reader = std::make_unique<NtfsBrowserTests::MemoryDiskReader>(
      NtfsBrowserTests::
          BuildFakeNtfsImageWithMultiTypeAttributeListDirectory());

  NtfsVolume<Strategy::FULL_CACHE> volume(std::move(reader));
  REQUIRE(volume.IsVolumeOK());

  FileRecord<Strategy::FULL_CACHE> dir(volume);
  dir.SetAttrMask(Mask::INDEX_ROOT | Mask::INDEX_ALLOCATION);

  REQUIRE(dir.ParseFileRecord(NtfsBrowserTests::kAttrListMultiTypeDirIdx));
  REQUIRE(dir.ParseAttrs());

  CHECK_FALSE(dir.getAttr(AttrType::INDEX_ROOT).empty());

  // Both entries name the same extension record, but different types.
  CHECK_FALSE(dir.getAttr(AttrType::INDEX_ALLOCATION).empty());
}

TEST_CASE(
    "AttrList chain state does not leak across FileRecord::ParseFileRecord "
    "calls on a reused FileRecord",
    "[file-record][regression]")
{
  auto reader = std::make_unique<NtfsBrowserTests::MemoryDiskReader>(
      NtfsBrowserTests::
          BuildFakeNtfsImageWithAttributeListDirectoryChainReused());

  NtfsVolume<Strategy::FULL_CACHE> volume(std::move(reader));
  REQUIRE(volume.IsVolumeOK());

  FileRecord<Strategy::FULL_CACHE> dir(volume);
  dir.SetAttrMask(Mask::INDEX_ROOT | Mask::INDEX_ALLOCATION);

  REQUIRE(dir.ParseFileRecord(NtfsBrowserTests::kAttributeListDirIdx));
  REQUIRE(dir.ParseAttrs());
  CHECK_FALSE(dir.getAttr(AttrType::INDEX_ROOT).empty());

  // Same FileRecord, unrelated directory relocating to the same record.
  REQUIRE(dir.ParseFileRecord(NtfsBrowserTests::kAttributeListDirIdx2));
  REQUIRE(dir.ParseAttrs());
  CHECK_FALSE(dir.getAttr(AttrType::INDEX_ROOT).empty());
}

TEST_CASE(
    "AttrList's FileRecord vector growth does not invalidate "
    "already-resolved extension records' attributes (FULL_CACHE)",
    "[file-record][regression]")
{
  auto reader = std::make_unique<NtfsBrowserTests::MemoryDiskReader>(
      NtfsBrowserTests::
          BuildFakeNtfsImageWithFragmentedAttributeListDirectory());

  NtfsVolume<Strategy::FULL_CACHE> volume(std::move(reader));
  REQUIRE(volume.IsVolumeOK());

  FileRecord<Strategy::FULL_CACHE> dir(volume);
  dir.SetAttrMask(Mask::INDEX_ALLOCATION);

  REQUIRE(dir.ParseFileRecord(NtfsBrowserTests::kUafAttrListDirIdx));
  REQUIRE(dir.ParseAttrs());

  const auto& allocAttrs = dir.getAttr(AttrType::INDEX_ALLOCATION);
  REQUIRE(allocAttrs.size() == NtfsBrowserTests::kUafRealSizeSentinels.size());

  // Each GetDataSize() reads through a reference bound at that extension
  // record's construction time, so a moved FileRecord reads stale memory.
  for (size_t i = 0; i < allocAttrs.size(); i++)
  {
    CHECK(allocAttrs[i]->GetDataSize() ==
          NtfsBrowserTests::kUafRealSizeSentinels[i]);
  }
}

TEST_CASE(
    "AttrList stops cleanly on a resident $ATTRIBUTE_LIST whose size isn't "
    "a multiple of the entry size, when recovering",
    "[attr-list][regression]")
{
  auto reader = std::make_unique<NtfsBrowserTests::MemoryDiskReader>(
      NtfsBrowserTests::BuildFakeNtfsImageWithAttributeListShortRead());

  NtfsVolume<Strategy::NO_CACHE> volume(std::move(reader),
                                        VolumeOptions{.recover_errors = true});
  REQUIRE(volume.IsVolumeOK());

  FileRecord<Strategy::NO_CACHE> record(volume);
  REQUIRE(record.ParseFileRecord(static_cast<ULONGLONG>(MftIdx::ROOT)));
  CHECK(record.ParseAttrs());
}

TEST_CASE(
    "AttrList stops cleanly on a resident $ATTRIBUTE_LIST whose size isn't "
    "a multiple of the entry size, when recovering (FULL_CACHE)",
    "[attr-list][regression]")
{
  auto reader = std::make_unique<NtfsBrowserTests::MemoryDiskReader>(
      NtfsBrowserTests::BuildFakeNtfsImageWithAttributeListShortRead());

  NtfsVolume<Strategy::FULL_CACHE> volume(
      std::move(reader), VolumeOptions{.recover_errors = true});
  REQUIRE(volume.IsVolumeOK());

  FileRecord<Strategy::FULL_CACHE> record(volume);
  REQUIRE(record.ParseFileRecord(static_cast<ULONGLONG>(MftIdx::ROOT)));
  CHECK(record.ParseAttrs());
}

TEST_CASE(
    "A resident $ATTRIBUTE_LIST whose size isn't a multiple of the entry "
    "size rejects the record by default",
    "[attr-list][regression]")
{
  auto reader = std::make_unique<NtfsBrowserTests::MemoryDiskReader>(
      NtfsBrowserTests::BuildFakeNtfsImageWithAttributeListShortRead());

  NtfsVolume<Strategy::NO_CACHE> volume(std::move(reader));
  REQUIRE(volume.IsVolumeOK());

  FileRecord<Strategy::NO_CACHE> record(volume);
  REQUIRE(record.ParseFileRecord(static_cast<ULONGLONG>(MftIdx::ROOT)));
  CHECK_FALSE(record.ParseAttrs());
}

TEST_CASE(
    "A resident $ATTRIBUTE_LIST whose size isn't a multiple of the entry "
    "size rejects the record by default (FULL_CACHE)",
    "[attr-list][regression]")
{
  auto reader = std::make_unique<NtfsBrowserTests::MemoryDiskReader>(
      NtfsBrowserTests::BuildFakeNtfsImageWithAttributeListShortRead());

  NtfsVolume<Strategy::FULL_CACHE> volume(std::move(reader));
  REQUIRE(volume.IsVolumeOK());

  FileRecord<Strategy::FULL_CACHE> record(volume);
  REQUIRE(record.ParseFileRecord(static_cast<ULONGLONG>(MftIdx::ROOT)));
  CHECK_FALSE(record.ParseAttrs());
}

TEST_CASE(
    "AttrList stops cleanly on an entry whose record_size is smaller than "
    "the entry header, when recovering",
    "[attr-list][regression]")
{
  auto reader = std::make_unique<NtfsBrowserTests::MemoryDiskReader>(
      NtfsBrowserTests::
          BuildFakeNtfsImageWithAttributeListRecordSizeTooSmall());

  NtfsVolume<Strategy::NO_CACHE> volume(std::move(reader),
                                        VolumeOptions{.recover_errors = true});
  REQUIRE(volume.IsVolumeOK());

  FileRecord<Strategy::NO_CACHE> dir(volume);
  dir.SetAttrMask(Mask::INDEX_ROOT);
  REQUIRE(dir.ParseFileRecord(NtfsBrowserTests::kAttributeListDirIdx));
  CHECK(dir.ParseAttrs());
  CHECK_FALSE(dir.getAttr(AttrType::INDEX_ROOT).empty());
}

TEST_CASE(
    "AttrList stops cleanly on an entry whose record_size is smaller than "
    "the entry header, when recovering (FULL_CACHE)",
    "[attr-list][regression]")
{
  auto reader = std::make_unique<NtfsBrowserTests::MemoryDiskReader>(
      NtfsBrowserTests::
          BuildFakeNtfsImageWithAttributeListRecordSizeTooSmall());

  NtfsVolume<Strategy::FULL_CACHE> volume(
      std::move(reader), VolumeOptions{.recover_errors = true});
  REQUIRE(volume.IsVolumeOK());

  FileRecord<Strategy::FULL_CACHE> dir(volume);
  dir.SetAttrMask(Mask::INDEX_ROOT);
  REQUIRE(dir.ParseFileRecord(NtfsBrowserTests::kAttributeListDirIdx));
  CHECK(dir.ParseAttrs());
  CHECK_FALSE(dir.getAttr(AttrType::INDEX_ROOT).empty());
}

TEST_CASE(
    "An $ATTRIBUTE_LIST entry whose record_size is smaller than the entry "
    "header rejects the record by default",
    "[attr-list][regression]")
{
  auto reader = std::make_unique<NtfsBrowserTests::MemoryDiskReader>(
      NtfsBrowserTests::
          BuildFakeNtfsImageWithAttributeListRecordSizeTooSmall());

  NtfsVolume<Strategy::NO_CACHE> volume(std::move(reader));
  REQUIRE(volume.IsVolumeOK());

  FileRecord<Strategy::NO_CACHE> dir(volume);
  dir.SetAttrMask(Mask::INDEX_ROOT);
  REQUIRE(dir.ParseFileRecord(NtfsBrowserTests::kAttributeListDirIdx));
  CHECK_FALSE(dir.ParseAttrs());
}

TEST_CASE(
    "An $ATTRIBUTE_LIST entry whose record_size is smaller than the entry "
    "header rejects the record by default (FULL_CACHE)",
    "[attr-list][regression]")
{
  auto reader = std::make_unique<NtfsBrowserTests::MemoryDiskReader>(
      NtfsBrowserTests::
          BuildFakeNtfsImageWithAttributeListRecordSizeTooSmall());

  NtfsVolume<Strategy::FULL_CACHE> volume(std::move(reader));
  REQUIRE(volume.IsVolumeOK());

  FileRecord<Strategy::FULL_CACHE> dir(volume);
  dir.SetAttrMask(Mask::INDEX_ROOT);
  REQUIRE(dir.ParseFileRecord(NtfsBrowserTests::kAttributeListDirIdx));
  CHECK_FALSE(dir.ParseAttrs());
}

TEST_CASE(
    "AttrList stops cleanly when an entry's record_size overshoots the "
    "attribute's declared size, when recovering",
    "[attr-list][regression]")
{
  auto reader = std::make_unique<NtfsBrowserTests::MemoryDiskReader>(
      NtfsBrowserTests::BuildFakeNtfsImageWithAttributeListOffsetMismatch());

  NtfsVolume<Strategy::NO_CACHE> volume(std::move(reader),
                                        VolumeOptions{.recover_errors = true});
  REQUIRE(volume.IsVolumeOK());

  FileRecord<Strategy::NO_CACHE> dir(volume);
  dir.SetAttrMask(Mask::INDEX_ROOT);
  REQUIRE(dir.ParseFileRecord(NtfsBrowserTests::kAttributeListDirIdx));
  CHECK(dir.ParseAttrs());
  CHECK_FALSE(dir.getAttr(AttrType::INDEX_ROOT).empty());
}

TEST_CASE(
    "AttrList stops cleanly when an entry's record_size overshoots the "
    "attribute's declared size, when recovering (FULL_CACHE)",
    "[attr-list][regression]")
{
  auto reader = std::make_unique<NtfsBrowserTests::MemoryDiskReader>(
      NtfsBrowserTests::BuildFakeNtfsImageWithAttributeListOffsetMismatch());

  NtfsVolume<Strategy::FULL_CACHE> volume(
      std::move(reader), VolumeOptions{.recover_errors = true});
  REQUIRE(volume.IsVolumeOK());

  FileRecord<Strategy::FULL_CACHE> dir(volume);
  dir.SetAttrMask(Mask::INDEX_ROOT);
  REQUIRE(dir.ParseFileRecord(NtfsBrowserTests::kAttributeListDirIdx));
  CHECK(dir.ParseAttrs());
  CHECK_FALSE(dir.getAttr(AttrType::INDEX_ROOT).empty());
}

TEST_CASE(
    "An $ATTRIBUTE_LIST entry whose record_size overshoots the attribute's "
    "declared size rejects the record by default",
    "[attr-list][regression]")
{
  auto reader = std::make_unique<NtfsBrowserTests::MemoryDiskReader>(
      NtfsBrowserTests::BuildFakeNtfsImageWithAttributeListOffsetMismatch());

  NtfsVolume<Strategy::NO_CACHE> volume(std::move(reader));
  REQUIRE(volume.IsVolumeOK());

  FileRecord<Strategy::NO_CACHE> dir(volume);
  dir.SetAttrMask(Mask::INDEX_ROOT);
  REQUIRE(dir.ParseFileRecord(NtfsBrowserTests::kAttributeListDirIdx));
  CHECK_FALSE(dir.ParseAttrs());
}

TEST_CASE(
    "An $ATTRIBUTE_LIST entry whose record_size overshoots the attribute's "
    "declared size rejects the record by default (FULL_CACHE)",
    "[attr-list][regression]")
{
  auto reader = std::make_unique<NtfsBrowserTests::MemoryDiskReader>(
      NtfsBrowserTests::BuildFakeNtfsImageWithAttributeListOffsetMismatch());

  NtfsVolume<Strategy::FULL_CACHE> volume(std::move(reader));
  REQUIRE(volume.IsVolumeOK());

  FileRecord<Strategy::FULL_CACHE> dir(volume);
  dir.SetAttrMask(Mask::INDEX_ROOT);
  REQUIRE(dir.ParseFileRecord(NtfsBrowserTests::kAttributeListDirIdx));
  CHECK_FALSE(dir.ParseAttrs());
}

TEST_CASE(
    "AttrList's cycle guard stops a two-record $ATTRIBUTE_LIST resolution "
    "cycle instead of recursing without bound",
    "[attr-list][regression]")
{
  auto reader = std::make_unique<NtfsBrowserTests::MemoryDiskReader>(
      NtfsBrowserTests::BuildFakeNtfsImageWithAttributeListCycle());

  NtfsVolume<Strategy::NO_CACHE> volume(std::move(reader));
  REQUIRE(volume.IsVolumeOK());

  FileRecord<Strategy::NO_CACHE> record(volume);
  REQUIRE(record.ParseFileRecord(static_cast<ULONGLONG>(MftIdx::ROOT)));
  CHECK(record.ParseAttrs());
}

TEST_CASE(
    "AttrList's cycle guard stops a two-record $ATTRIBUTE_LIST resolution "
    "cycle instead of recursing without bound (FULL_CACHE)",
    "[attr-list][regression]")
{
  auto reader = std::make_unique<NtfsBrowserTests::MemoryDiskReader>(
      NtfsBrowserTests::BuildFakeNtfsImageWithAttributeListCycle());

  NtfsVolume<Strategy::FULL_CACHE> volume(std::move(reader));
  REQUIRE(volume.IsVolumeOK());

  FileRecord<Strategy::FULL_CACHE> record(volume);
  REQUIRE(record.ParseFileRecord(static_cast<ULONGLONG>(MftIdx::ROOT)));
  CHECK(record.ParseAttrs());
}

TEST_CASE(
    "AttrList resolves every entry in a densely-packed (real 26-byte "
    "stride) $ATTRIBUTE_LIST, not just those a multiple of "
    "sizeof(Attr::AttributeList) apart",
    "[attr-list][regression]")
{
  auto reader = std::make_unique<NtfsBrowserTests::MemoryDiskReader>(
      NtfsBrowserTests::
          BuildFakeNtfsImageWithTightlyPackedAttributeListDirectory());

  NtfsVolume<Strategy::NO_CACHE> volume(std::move(reader));
  REQUIRE(volume.IsVolumeOK());

  FileRecord<Strategy::NO_CACHE> dir(volume);
  dir.SetAttrMask(Mask::INDEX_ROOT | Mask::INDEX_ALLOCATION);

  REQUIRE(dir.ParseFileRecord(NtfsBrowserTests::kAttrListTightPackDirIdx));
  REQUIRE(dir.ParseAttrs());

  const std::optional<IndexEntry> found = dir.FindSubEntry(L"Foo");
  REQUIRE(found.has_value());
  CHECK(found->GetFileReference() == 20);

  const auto& allocAttrs = dir.getAttr(AttrType::INDEX_ALLOCATION);
  REQUIRE(allocAttrs.size() == 1);
  CHECK(allocAttrs[0]->GetDataSize() ==
        NtfsBrowserTests::kAttrListTightPackRealSize);
}

TEST_CASE(
    "ReadFileRecord() resolves a record through $MFT's own DATA "
    "continuation, reached via $MFT's own $ATTRIBUTE_LIST (FULL_CACHE)",
    "[ntfs-volume][regression]")
{
  auto reader = std::make_unique<NtfsBrowserTests::MemoryDiskReader>(
      NtfsBrowserTests::
          BuildFakeNtfsImageWithMftDataSplitAcrossAttributeList());

  NtfsVolume<Strategy::FULL_CACHE> volume(std::move(reader));
  REQUIRE(volume.IsVolumeOK());
  // mft_data_ must be the base extent, not whichever instance parsed first.
  CHECK(volume.GetRecordsCount() == 1);

  FileRecord<Strategy::FULL_CACHE> record(volume);
  CHECK(record.ParseFileRecord(NtfsBrowserTests::kMftDataSplitTargetIdx));
}

TEST_CASE(
    "ReadFileRecord() resolves a record through $MFT's own DATA "
    "continuation, reached via $MFT's own $ATTRIBUTE_LIST (NO_CACHE)",
    "[ntfs-volume][regression]")
{
  auto reader = std::make_unique<NtfsBrowserTests::MemoryDiskReader>(
      NtfsBrowserTests::
          BuildFakeNtfsImageWithMftDataSplitAcrossAttributeList());

  NtfsVolume<Strategy::NO_CACHE> volume(std::move(reader));
  REQUIRE(volume.IsVolumeOK());
  CHECK(volume.GetRecordsCount() == 1);

  FileRecord<Strategy::NO_CACHE> record(volume);
  CHECK(record.ParseFileRecord(NtfsBrowserTests::kMftDataSplitTargetIdx));
}

TEST_CASE(
    "ReadFileRecord() resolves a two-hop $MFT DATA continuation chain, "
    "where an earlier $ATTRIBUTE_LIST entry depends on a later one "
    "(FULL_CACHE)",
    "[ntfs-volume][regression]")
{
  auto reader = std::make_unique<NtfsBrowserTests::MemoryDiskReader>(
      NtfsBrowserTests::BuildFakeNtfsImageWithMftDataExtentChain());

  NtfsVolume<Strategy::FULL_CACHE> volume(std::move(reader));
  REQUIRE(volume.IsVolumeOK());

  FileRecord<Strategy::FULL_CACHE> record(volume);
  CHECK(record.ParseFileRecord(NtfsBrowserTests::kMftChainExtA));
  CHECK(record.ParseFileRecord(NtfsBrowserTests::kMftChainExtAStartVcn));
}

TEST_CASE(
    "ReadFileRecord() resolves a two-hop $MFT DATA continuation chain, "
    "where an earlier $ATTRIBUTE_LIST entry depends on a later one "
    "(NO_CACHE)",
    "[ntfs-volume][regression]")
{
  auto reader = std::make_unique<NtfsBrowserTests::MemoryDiskReader>(
      NtfsBrowserTests::BuildFakeNtfsImageWithMftDataExtentChain());

  NtfsVolume<Strategy::NO_CACHE> volume(std::move(reader));
  REQUIRE(volume.IsVolumeOK());

  FileRecord<Strategy::NO_CACHE> record(volume);
  CHECK(record.ParseFileRecord(NtfsBrowserTests::kMftChainExtA));
  CHECK(record.ParseFileRecord(NtfsBrowserTests::kMftChainExtAStartVcn));
}

TEST_CASE(
    "A permanently unresolvable $MFT DATA continuation does not take down "
    "an earlier, resolvable one in the same $ATTRIBUTE_LIST (FULL_CACHE)",
    "[ntfs-volume][regression]")
{
  auto reader = std::make_unique<NtfsBrowserTests::MemoryDiskReader>(
      NtfsBrowserTests::BuildFakeNtfsImageWithUnresolvableMftDataExtent());

  NtfsVolume<Strategy::FULL_CACHE> volume(std::move(reader));
  REQUIRE(volume.IsVolumeOK());

  FileRecord<Strategy::FULL_CACHE> record(volume);
  CHECK(record.ParseFileRecord(NtfsBrowserTests::kMftUnresolvableGoodRecord));
  CHECK_FALSE(record.ParseFileRecord(NtfsBrowserTests::kMftUnresolvableExtIdx));
}

TEST_CASE(
    "A permanently unresolvable $MFT DATA continuation does not take down "
    "an earlier, resolvable one in the same $ATTRIBUTE_LIST (NO_CACHE)",
    "[ntfs-volume][regression]")
{
  auto reader = std::make_unique<NtfsBrowserTests::MemoryDiskReader>(
      NtfsBrowserTests::BuildFakeNtfsImageWithUnresolvableMftDataExtent());

  NtfsVolume<Strategy::NO_CACHE> volume(std::move(reader));
  REQUIRE(volume.IsVolumeOK());

  FileRecord<Strategy::NO_CACHE> record(volume);
  CHECK(record.ParseFileRecord(NtfsBrowserTests::kMftUnresolvableGoodRecord));
  CHECK_FALSE(record.ParseFileRecord(NtfsBrowserTests::kMftUnresolvableExtIdx));
}

namespace
{

// Reads the first "size" bytes of "attr"; empty when the read fails.
template <Strategy S>
std::vector<BYTE> ReadFirstBytes(const NtfsBrowser::AttrBase<S>& attr,
                                 size_t size)
{
  std::vector<BYTE> buffer(size, 0xCC);
  const std::optional<ULONGLONG> read = attr.ReadData(0, buffer);
  if (!read)
  {
    return {};
  }
  buffer.resize(static_cast<size_t>(*read));
  return buffer;
}

// Allocates blocks of every size a freed file record could have had, filled
// with a byte that is not part of the expected content: whatever a stale read
// meets there is then wrong, on any heap that recycles freed blocks.
std::vector<std::vector<BYTE>> ScribbleOverFreedMemory()
{
  // Smallest and largest block: a 4 KiB record buffer or header, and slack.
  constexpr size_t kMinBlock = 64;
  constexpr size_t kMaxBlock = 8192;
  // The heap's size-class granularity, so every class is hit.
  constexpr size_t kBlockStep = 16;
  // A class may hold several free blocks.
  constexpr size_t kBlocksPerSize = 4;
  // Not in kAttrListLifetimeDataContent, and not the debug heap's 0xDD.
  constexpr BYTE kFill = 0xEE;

  std::vector<std::vector<BYTE>> blocks;
  for (size_t size = kMinBlock; size <= kMaxBlock; size += kBlockStep)
  {
    for (size_t i = 0; i < kBlocksPerSize; i++)
    {
      blocks.emplace_back(size, kFill);
    }
  }
  return blocks;
}

const std::vector<BYTE> kExpectedLifetimeContent(
    NtfsBrowserTests::kAttrListLifetimeDataContent.begin(),
    NtfsBrowserTests::kAttrListLifetimeDataContent.end());

}  // namespace

TEMPLATE_TEST_CASE_SIG(
    "An $ATTRIBUTE_LIST that fails after importing an attribute leaves it "
    "readable, when recovering",
    "[attr-list][regression]", ((Strategy S), S), Strategy::NO_CACHE,
    Strategy::FULL_CACHE)
{
  auto reader = std::make_unique<NtfsBrowserTests::MemoryDiskReader>(
      NtfsBrowserTests::
          BuildFakeNtfsImageWithAttributeListImportThenZeroRecordSize());

  NtfsVolume<S> volume(std::move(reader),
                       VolumeOptions{.recover_errors = true});
  REQUIRE(volume.IsVolumeOK());

  FileRecord<S> record(volume);
  REQUIRE(record.ParseFileRecord(NtfsBrowserTests::kAttrListLifetimeBaseIdx));
  CHECK_FALSE(record.ParseAttrs());

  const auto scribbled = ScribbleOverFreedMemory();
  // The imported attribute's bytes live in the extension record.
  const auto& data = record.getAttr(AttrType::DATA);
  REQUIRE(data.size() == 1);
  CHECK(data.front()->GetAttrType() == AttrType::DATA);
  CHECK(ReadFirstBytes<S>(*data.front(), kExpectedLifetimeContent.size()) ==
        kExpectedLifetimeContent);
}

TEMPLATE_TEST_CASE_SIG(
    "Two $ATTRIBUTE_LIST attributes with contiguous VCNs leave the imported "
    "attribute readable",
    "[attr-list][regression]", ((Strategy S), S), Strategy::NO_CACHE,
    Strategy::FULL_CACHE)
{
  auto reader = std::make_unique<NtfsBrowserTests::MemoryDiskReader>(
      NtfsBrowserTests::BuildFakeNtfsImageWithSplitAttributeListAttribute());

  NtfsVolume<S> volume(std::move(reader));
  REQUIRE(volume.IsVolumeOK());

  FileRecord<S> record(volume);
  REQUIRE(record.ParseFileRecord(NtfsBrowserTests::kAttrListLifetimeBaseIdx));
  REQUIRE(record.ParseAttrs());

  const auto scribbled = ScribbleOverFreedMemory();
  // The imported attribute's bytes live in the extension record.
  const auto& data = record.getAttr(AttrType::DATA);
  REQUIRE(data.size() == 1);
  CHECK(data.front()->GetAttrType() == AttrType::DATA);
  CHECK(ReadFirstBytes<S>(*data.front(), kExpectedLifetimeContent.size()) ==
        kExpectedLifetimeContent);
}

TEMPLATE_TEST_CASE_SIG(
    "A recovering parse that stops on a malformed attribute still merges "
    "a split attribute",
    "[attr-list][regression]", ((Strategy S), S), Strategy::NO_CACHE,
    Strategy::FULL_CACHE)
{
  const auto defect =
      GENERATE(NtfsBrowserTests::FakeTrailingDefect::UndersizedHeader,
               NtfsBrowserTests::FakeTrailingDefect::UndersizedCompressedField,
               NtfsBrowserTests::FakeTrailingDefect::RejectedAttribute);

  auto reader = std::make_unique<NtfsBrowserTests::MemoryDiskReader>(
      NtfsBrowserTests::BuildFakeNtfsImageWithSplitDataAndTrailingDefect(
          defect));

  NtfsVolume<S> volume(std::move(reader),
                       VolumeOptions{.recover_errors = true});
  REQUIRE(volume.IsVolumeOK());

  FileRecord<S> record(volume);
  REQUIRE(record.ParseFileRecord(NtfsBrowserTests::kAttrListLifetimeBaseIdx));
  CHECK_FALSE(record.ParseAttrs());

  // One stream, not its VCN 0 and VCN 1 halves.
  const auto& data = record.getAttr(AttrType::DATA);
  REQUIRE(data.size() == 1);
  CHECK(data.front()->GetDataSize() == NtfsBrowserTests::kFakeClusterSize);
}
