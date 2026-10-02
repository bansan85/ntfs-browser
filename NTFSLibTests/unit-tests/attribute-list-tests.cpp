#include <ntfs-browser/win-types.h>

#include <array>
#include <chrono>
#include <cstddef>
#include <exception>
#include <future>
#include <memory>
#include <optional>
#include <thread>
#include <utility>
#include <vector>

#include <catch2/catch_template_test_macros.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <gsl/narrow>

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

TEMPLATE_TEST_CASE_SIG(
    "FindSubEntry follows $ATTRIBUTE_LIST to a relocated $INDEX_ROOT",
    "[file-record][regression]", ((Strategy S), S), Strategy::NO_CACHE,
    Strategy::FULL_CACHE)
{
  auto reader = std::make_unique<NtfsBrowserTests::MemoryDiskReader>(
      NtfsBrowserTests::BuildFakeNtfsImageWithAttributeListDirectory());

  NtfsVolume<S> const volume(std::move(reader));
  REQUIRE(volume.IsVolumeOK());

  FileRecord<S> dir(volume);
  dir.SetAttrMask(Mask::INDEX_ROOT | Mask::INDEX_ALLOCATION);

  REQUIRE(dir.ParseFileRecord(NtfsBrowserTests::kAttributeListDirIdx));
  REQUIRE(dir.ParseAttrs());

  CHECK_FALSE(dir.getAttr(AttrType::INDEX_ROOT).empty());

  const std::optional<IndexEntry> found = dir.FindSubEntry(L"Foo");
  REQUIRE(found.has_value());
  CHECK(found->GetFileReference() == 20);
}

TEMPLATE_TEST_CASE_SIG(
    "AttrList merges every attribute type relocated into the same "
    "extension record",
    "[file-record][regression]", ((Strategy S), S), Strategy::NO_CACHE,
    Strategy::FULL_CACHE)
{
  auto reader = std::make_unique<NtfsBrowserTests::MemoryDiskReader>(
      NtfsBrowserTests::
          BuildFakeNtfsImageWithMultiTypeAttributeListDirectory());

  NtfsVolume<S> const volume(std::move(reader));
  REQUIRE(volume.IsVolumeOK());

  FileRecord<S> dir(volume);
  dir.SetAttrMask(Mask::INDEX_ROOT | Mask::INDEX_ALLOCATION);

  REQUIRE(dir.ParseFileRecord(NtfsBrowserTests::kAttrListMultiTypeDirIdx));
  REQUIRE(dir.ParseAttrs());

  CHECK_FALSE(dir.getAttr(AttrType::INDEX_ROOT).empty());

  // Both entries name the same extension record, but different types.
  CHECK_FALSE(dir.getAttr(AttrType::INDEX_ALLOCATION).empty());
}

TEMPLATE_TEST_CASE_SIG(
    "AttrList chain state does not leak across FileRecord::ParseFileRecord "
    "calls on a reused FileRecord",
    "[file-record][regression]", ((Strategy S), S), Strategy::NO_CACHE,
    Strategy::FULL_CACHE)
{
  auto reader = std::make_unique<NtfsBrowserTests::MemoryDiskReader>(
      NtfsBrowserTests::BuildFakeNtfsImageWithAttributeListDirectory());

  NtfsVolume<S> const volume(std::move(reader));
  REQUIRE(volume.IsVolumeOK());

  FileRecord<S> dir(volume);
  dir.SetAttrMask(Mask::INDEX_ROOT | Mask::INDEX_ALLOCATION);

  REQUIRE(dir.ParseFileRecord(NtfsBrowserTests::kAttributeListDirIdx));
  REQUIRE(dir.ParseAttrs());
  CHECK_FALSE(dir.getAttr(AttrType::INDEX_ROOT).empty());

  // Same FileRecord, same directory: it relocates to the same record again.
  REQUIRE(dir.ParseFileRecord(NtfsBrowserTests::kAttributeListDirIdx));
  REQUIRE(dir.ParseAttrs());
  CHECK_FALSE(dir.getAttr(AttrType::INDEX_ROOT).empty());
}

TEMPLATE_TEST_CASE_SIG(
    "AttrList's FileRecord vector growth does not invalidate "
    "already-resolved extension records' attributes",
    "[file-record][regression]", ((Strategy S), S), Strategy::NO_CACHE,
    Strategy::FULL_CACHE)
{
  auto reader = std::make_unique<NtfsBrowserTests::MemoryDiskReader>(
      NtfsBrowserTests::
          BuildFakeNtfsImageWithFragmentedAttributeListDirectory());

  NtfsVolume<S> const volume(std::move(reader));
  REQUIRE(volume.IsVolumeOK());

  FileRecord<S> dir(volume);
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

TEMPLATE_TEST_CASE_SIG(
    "AttrList stops cleanly on a resident $ATTRIBUTE_LIST whose size isn't "
    "a multiple of the entry size, when recovering",
    "[attr-list][regression]", ((Strategy S), S), Strategy::NO_CACHE,
    Strategy::FULL_CACHE)
{
  auto reader = std::make_unique<NtfsBrowserTests::MemoryDiskReader>(
      NtfsBrowserTests::BuildFakeNtfsImageWithAttributeListShortRead());

  NtfsVolume<S> const volume(std::move(reader),
                             VolumeOptions{.recover_errors = true});
  REQUIRE(volume.IsVolumeOK());

  FileRecord<S> record(volume);
  REQUIRE(record.ParseFileRecord(static_cast<ULONGLONG>(MftIdx::ROOT)));
  CHECK(record.ParseAttrs());
}

TEMPLATE_TEST_CASE_SIG(
    "A resident $ATTRIBUTE_LIST whose size isn't a multiple of the entry "
    "size rejects the record by default",
    "[attr-list][regression]", ((Strategy S), S), Strategy::NO_CACHE,
    Strategy::FULL_CACHE)
{
  auto reader = std::make_unique<NtfsBrowserTests::MemoryDiskReader>(
      NtfsBrowserTests::BuildFakeNtfsImageWithAttributeListShortRead());

  NtfsVolume<S> const volume(std::move(reader));
  REQUIRE(volume.IsVolumeOK());

  FileRecord<S> record(volume);
  REQUIRE(record.ParseFileRecord(static_cast<ULONGLONG>(MftIdx::ROOT)));
  CHECK_FALSE(record.ParseAttrs());
}

TEMPLATE_TEST_CASE_SIG(
    "AttrList stops cleanly on an entry whose record_size is smaller than "
    "the entry header, when recovering",
    "[attr-list][regression]", ((Strategy S), S), Strategy::NO_CACHE,
    Strategy::FULL_CACHE)
{
  auto reader = std::make_unique<NtfsBrowserTests::MemoryDiskReader>(
      NtfsBrowserTests::
          BuildFakeNtfsImageWithAttributeListRecordSizeTooSmall());

  NtfsVolume<S> const volume(std::move(reader),
                             VolumeOptions{.recover_errors = true});
  REQUIRE(volume.IsVolumeOK());

  FileRecord<S> dir(volume);
  dir.SetAttrMask(Mask::INDEX_ROOT);
  REQUIRE(dir.ParseFileRecord(NtfsBrowserTests::kAttributeListDirIdx));
  CHECK(dir.ParseAttrs());
  CHECK_FALSE(dir.getAttr(AttrType::INDEX_ROOT).empty());
}

TEMPLATE_TEST_CASE_SIG(
    "An $ATTRIBUTE_LIST entry whose record_size is smaller than the entry "
    "header rejects the record by default",
    "[attr-list][regression]", ((Strategy S), S), Strategy::NO_CACHE,
    Strategy::FULL_CACHE)
{
  auto reader = std::make_unique<NtfsBrowserTests::MemoryDiskReader>(
      NtfsBrowserTests::
          BuildFakeNtfsImageWithAttributeListRecordSizeTooSmall());

  NtfsVolume<S> const volume(std::move(reader));
  REQUIRE(volume.IsVolumeOK());

  FileRecord<S> dir(volume);
  dir.SetAttrMask(Mask::INDEX_ROOT);
  REQUIRE(dir.ParseFileRecord(NtfsBrowserTests::kAttributeListDirIdx));
  CHECK_FALSE(dir.ParseAttrs());
}

TEMPLATE_TEST_CASE_SIG(
    "AttrList stops cleanly when an entry's record_size overshoots the "
    "attribute's declared size, when recovering",
    "[attr-list][regression]", ((Strategy S), S), Strategy::NO_CACHE,
    Strategy::FULL_CACHE)
{
  auto reader = std::make_unique<NtfsBrowserTests::MemoryDiskReader>(
      NtfsBrowserTests::BuildFakeNtfsImageWithAttributeListOffsetMismatch());

  NtfsVolume<S> const volume(std::move(reader),
                             VolumeOptions{.recover_errors = true});
  REQUIRE(volume.IsVolumeOK());

  FileRecord<S> dir(volume);
  dir.SetAttrMask(Mask::INDEX_ROOT);
  REQUIRE(dir.ParseFileRecord(NtfsBrowserTests::kAttributeListDirIdx));
  CHECK(dir.ParseAttrs());
  CHECK_FALSE(dir.getAttr(AttrType::INDEX_ROOT).empty());
}

TEMPLATE_TEST_CASE_SIG(
    "An $ATTRIBUTE_LIST entry whose record_size overshoots the attribute's "
    "declared size rejects the record by default",
    "[attr-list][regression]", ((Strategy S), S), Strategy::NO_CACHE,
    Strategy::FULL_CACHE)
{
  auto reader = std::make_unique<NtfsBrowserTests::MemoryDiskReader>(
      NtfsBrowserTests::BuildFakeNtfsImageWithAttributeListOffsetMismatch());

  NtfsVolume<S> const volume(std::move(reader));
  REQUIRE(volume.IsVolumeOK());

  FileRecord<S> dir(volume);
  dir.SetAttrMask(Mask::INDEX_ROOT);
  REQUIRE(dir.ParseFileRecord(NtfsBrowserTests::kAttributeListDirIdx));
  CHECK_FALSE(dir.ParseAttrs());
}

TEMPLATE_TEST_CASE_SIG(
    "AttrList's cycle guard stops a two-record $ATTRIBUTE_LIST resolution "
    "cycle instead of recursing without bound",
    "[attr-list][regression]", ((Strategy S), S), Strategy::NO_CACHE,
    Strategy::FULL_CACHE)
{
  auto reader = std::make_unique<NtfsBrowserTests::MemoryDiskReader>(
      NtfsBrowserTests::BuildFakeNtfsImageWithAttributeListCycle());

  NtfsVolume<S> const volume(std::move(reader));
  REQUIRE(volume.IsVolumeOK());

  FileRecord<S> record(volume);
  REQUIRE(record.ParseFileRecord(static_cast<ULONGLONG>(MftIdx::ROOT)));
  CHECK(record.ParseAttrs());
}

TEMPLATE_TEST_CASE_SIG(
    "AttrList resolves every entry in a densely-packed (real 26-byte "
    "stride) $ATTRIBUTE_LIST, not just those a multiple of "
    "sizeof(Attr::AttributeList) apart",
    "[attr-list][regression]", ((Strategy S), S), Strategy::NO_CACHE,
    Strategy::FULL_CACHE)
{
  auto reader = std::make_unique<NtfsBrowserTests::MemoryDiskReader>(
      NtfsBrowserTests::
          BuildFakeNtfsImageWithTightlyPackedAttributeListDirectory());

  NtfsVolume<S> const volume(std::move(reader));
  REQUIRE(volume.IsVolumeOK());

  FileRecord<S> dir(volume);
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

TEMPLATE_TEST_CASE_SIG(
    "ReadFileRecord() resolves a record through $MFT's own DATA "
    "continuation, reached via $MFT's own $ATTRIBUTE_LIST",
    "[ntfs-volume][regression]", ((Strategy S), S), Strategy::NO_CACHE,
    Strategy::FULL_CACHE)
{
  auto reader = std::make_unique<NtfsBrowserTests::MemoryDiskReader>(
      NtfsBrowserTests::
          BuildFakeNtfsImageWithMftDataSplitAcrossAttributeList());

  NtfsVolume<S> const volume(std::move(reader));
  REQUIRE(volume.IsVolumeOK());
  // mft_data_ must be the base extent, not whichever instance parsed first.
  CHECK(volume.GetRecordsCount() == 1);

  FileRecord<S> record(volume);
  CHECK(record.ParseFileRecord(NtfsBrowserTests::kMftDataSplitTargetIdx));
}

TEMPLATE_TEST_CASE_SIG(
    "ReadFileRecord() resolves a two-hop $MFT DATA continuation chain, "
    "where an earlier $ATTRIBUTE_LIST entry depends on a later one",
    "[ntfs-volume][regression]", ((Strategy S), S), Strategy::NO_CACHE,
    Strategy::FULL_CACHE)
{
  auto reader = std::make_unique<NtfsBrowserTests::MemoryDiskReader>(
      NtfsBrowserTests::BuildFakeNtfsImageWithMftDataExtentChain());

  NtfsVolume<S> const volume(std::move(reader));
  REQUIRE(volume.IsVolumeOK());

  FileRecord<S> record(volume);
  CHECK(record.ParseFileRecord(NtfsBrowserTests::kMftChainExtA));
  CHECK(record.ParseFileRecord(NtfsBrowserTests::kMftChainExtAStartVcn));
}

TEMPLATE_TEST_CASE_SIG(
    "A permanently unresolvable $MFT DATA continuation does not take down "
    "an earlier, resolvable one in the same $ATTRIBUTE_LIST",
    "[ntfs-volume][regression]", ((Strategy S), S), Strategy::NO_CACHE,
    Strategy::FULL_CACHE)
{
  auto reader = std::make_unique<NtfsBrowserTests::MemoryDiskReader>(
      NtfsBrowserTests::BuildFakeNtfsImageWithUnresolvableMftDataExtent());

  NtfsVolume<S> const volume(std::move(reader));
  REQUIRE(volume.IsVolumeOK());

  FileRecord<S> record(volume);
  CHECK(record.ParseFileRecord(NtfsBrowserTests::kMftUnresolvableGoodRecord));
  CHECK_FALSE(record.ParseFileRecord(NtfsBrowserTests::kMftUnresolvableExtIdx));
}

namespace
{

// Fill that tells an unread byte from a read one.
constexpr BYTE kUnreadFill = 0xCC;

// Reads the first "size" bytes of "attr"; empty when the read fails.
template <Strategy S>
std::vector<BYTE> ReadFirstBytes(const NtfsBrowser::AttrBase<S>& attr,
                                 size_t size)
{
  std::vector<BYTE> buffer(size, kUnreadFill);
  const std::optional<ULONGLONG> read = attr.ReadData(0, buffer);
  if (!read)
  {
    return {};
  }
  buffer.resize(gsl::narrow<size_t>(*read));
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

  NtfsVolume<S> const volume(std::move(reader),
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

  NtfsVolume<S> const volume(std::move(reader));
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

  NtfsVolume<S> const volume(std::move(reader),
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

namespace
{

// Longest a volume open may take before it counts as hung. A healthy open of
// a fake image takes milliseconds.
constexpr std::chrono::seconds kOpenTimeout{10};

// Opens a volume over image on a worker thread. Returns false if that did not
// finish within kOpenTimeout. The worker is then detached: it keeps spinning
// until the process exits, since a hung constructor cannot be cancelled.
template <Strategy S>
bool OpensWithinTimeout(std::vector<BYTE> image)
{
  auto const done = std::make_shared<std::promise<void>>();
  std::future<void> finished = done->get_future();

  std::thread worker(
      [done, image = std::move(image)]() mutable
      {
        try
        {
          NtfsVolume<S> const volume(
              std::make_unique<NtfsBrowserTests::MemoryDiskReader>(
                  std::move(image)));
          done->set_value();
        }
        catch (...)
        {
          done->set_exception(std::current_exception());
        }
      });

  if (finished.wait_for(kOpenTimeout) != std::future_status::ready)
  {
    worker.detach();
    return false;
  }
  worker.join();
  finished.get();
  return true;
}

}  // namespace

TEMPLATE_TEST_CASE_SIG(
    "A $MFT DATA extent whose last VCN overflows a byte offset does not hang "
    "the volume constructor",
    "[ntfs-volume][regression]", ((Strategy S), S), Strategy::NO_CACHE,
    Strategy::FULL_CACHE)
{
  CHECK(OpensWithinTimeout<S>(
      NtfsBrowserTests::BuildFakeNtfsImageWithMftDataLastVcnOverflow()));
}

namespace
{

// Extension links that do not belong to kAttrListLifetimeBaseIdx's list
// entry: a reused record, or a record of another file.
constexpr NtfsBrowserTests::FakeExtensionLink kForeignLinks[] = {
    // Same base, but the record has since been reused (sequence 3 -> 4).
    {.entry_sequence = 3, .record_sequence = 4, .base_ref = 6},
    // The record was reused by a live file of its own: no base at all.
    {.entry_sequence = 3, .record_sequence = 4, .base_ref = 0},
    // Same sequence, but the record is an extension of another file.
    {.entry_sequence = 0, .record_sequence = 0, .base_ref = 8},
};

}  // namespace

TEMPLATE_TEST_CASE_SIG(
    "An $ATTRIBUTE_LIST entry naming another file's record rejects the "
    "record by default",
    "[attr-list][regression]", ((Strategy S), S), Strategy::NO_CACHE,
    Strategy::FULL_CACHE)
{
  const NtfsBrowserTests::FakeExtensionLink link =
      kForeignLinks[GENERATE(size_t{0}, size_t{1}, size_t{2})];

  auto reader = std::make_unique<NtfsBrowserTests::MemoryDiskReader>(
      NtfsBrowserTests::BuildFakeNtfsImageWithExtensionLink(link));

  NtfsVolume<S> const volume(std::move(reader));
  REQUIRE(volume.IsVolumeOK());

  FileRecord<S> record(volume);
  REQUIRE(record.ParseFileRecord(NtfsBrowserTests::kAttrListLifetimeBaseIdx));
  CHECK_FALSE(record.ParseAttrs());
  CHECK(record.getAttr(AttrType::DATA).empty());
}

TEMPLATE_TEST_CASE_SIG(
    "An $ATTRIBUTE_LIST entry naming another file's record is skipped when "
    "recovering",
    "[attr-list][regression]", ((Strategy S), S), Strategy::NO_CACHE,
    Strategy::FULL_CACHE)
{
  const NtfsBrowserTests::FakeExtensionLink link =
      kForeignLinks[GENERATE(size_t{0}, size_t{1}, size_t{2})];

  auto reader = std::make_unique<NtfsBrowserTests::MemoryDiskReader>(
      NtfsBrowserTests::BuildFakeNtfsImageWithExtensionLink(link));

  NtfsVolume<S> const volume(std::move(reader),
                             VolumeOptions{.recover_errors = true});
  REQUIRE(volume.IsVolumeOK());

  FileRecord<S> record(volume);
  REQUIRE(record.ParseFileRecord(NtfsBrowserTests::kAttrListLifetimeBaseIdx));
  CHECK(record.ParseAttrs());
  CHECK(record.getAttr(AttrType::DATA).empty());
}

TEMPLATE_TEST_CASE_SIG(
    "An $ATTRIBUTE_LIST entry naming a genuine extension record still "
    "imports its attribute, sequence numbers included",
    "[attr-list][regression]", ((Strategy S), S), Strategy::NO_CACHE,
    Strategy::FULL_CACHE)
{
  auto reader = std::make_unique<NtfsBrowserTests::MemoryDiskReader>(
      NtfsBrowserTests::BuildFakeNtfsImageWithExtensionLink(
          NtfsBrowserTests::kGenuineExtensionLink));

  NtfsVolume<S> const volume(std::move(reader));
  REQUIRE(volume.IsVolumeOK());

  FileRecord<S> record(volume);
  REQUIRE(record.ParseFileRecord(NtfsBrowserTests::kAttrListLifetimeBaseIdx));
  REQUIRE(record.ParseAttrs());

  const auto& data = record.getAttr(AttrType::DATA);
  REQUIRE(data.size() == 1);
  CHECK(ReadFirstBytes<S>(*data.front(), kExpectedLifetimeContent.size()) ==
        kExpectedLifetimeContent);
}

TEMPLATE_TEST_CASE_SIG(
    "A raw attribute callback installed on a record can discard an "
    "attribute imported from an extension record",
    "[attr-list][regression]", ((Strategy S), S), Strategy::NO_CACHE,
    Strategy::FULL_CACHE)
{
  auto reader = std::make_unique<NtfsBrowserTests::MemoryDiskReader>(
      NtfsBrowserTests::BuildFakeNtfsImageWithExtensionLink(
          NtfsBrowserTests::kGenuineExtensionLink));

  NtfsVolume<S> const volume(std::move(reader));
  REQUIRE(volume.IsVolumeOK());

  FileRecord<S> record(volume);
  REQUIRE(record.InstallAttrRawCB(
      AttrType::DATA, [](const NtfsBrowser::AttrHeaderCommon&, bool& discard)
      { discard = true; }));
  REQUIRE(record.ParseFileRecord(NtfsBrowserTests::kAttrListLifetimeBaseIdx));
  REQUIRE(record.ParseAttrs());

  CHECK(record.getAttr(AttrType::DATA).empty());
}

TEMPLATE_TEST_CASE_SIG(
    "$MFT's own DATA continuation is ignored when its extension record was "
    "reused under another sequence number",
    "[ntfs-volume][regression]", ((Strategy S), S), Strategy::NO_CACHE,
    Strategy::FULL_CACHE)
{
  auto reader = std::make_unique<NtfsBrowserTests::MemoryDiskReader>(
      NtfsBrowserTests::BuildFakeNtfsImageWithMftDataSplitLink(
          {.entry_sequence = 3, .record_sequence = 4}));

  NtfsVolume<S> const volume(std::move(reader));
  REQUIRE(volume.IsVolumeOK());

  FileRecord<S> record(volume);
  CHECK_FALSE(record.ParseFileRecord(NtfsBrowserTests::kMftDataSplitTargetIdx));
}

TEMPLATE_TEST_CASE_SIG(
    "$MFT's own DATA continuation is followed when its extension record "
    "carries the sequence number its list entry names",
    "[ntfs-volume][regression]", ((Strategy S), S), Strategy::NO_CACHE,
    Strategy::FULL_CACHE)
{
  auto reader = std::make_unique<NtfsBrowserTests::MemoryDiskReader>(
      NtfsBrowserTests::BuildFakeNtfsImageWithMftDataSplitLink(
          {.entry_sequence = 3, .record_sequence = 3}));

  NtfsVolume<S> const volume(std::move(reader));
  REQUIRE(volume.IsVolumeOK());

  FileRecord<S> record(volume);
  CHECK(record.ParseFileRecord(NtfsBrowserTests::kMftDataSplitTargetIdx));
}

TEMPLATE_TEST_CASE_SIG(
    "Two $MFT DATA extents held by one extension record are both mapped",
    "[ntfs-volume][regression]", ((Strategy S), S), Strategy::NO_CACHE,
    Strategy::FULL_CACHE)
{
  auto reader = std::make_unique<NtfsBrowserTests::MemoryDiskReader>(
      NtfsBrowserTests::BuildFakeNtfsImageWithMftDataTwoExtentsInOneRecord());

  NtfsVolume<S> const volume(std::move(reader));
  REQUIRE(volume.IsVolumeOK());

  FileRecord<S> record(volume);
  CHECK(record.ParseFileRecord(NtfsBrowserTests::kMftTwoExtentsFirstVcn));
  CHECK(record.ParseFileRecord(NtfsBrowserTests::kMftTwoExtentsSecondVcn));
}
