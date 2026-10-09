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
#include "optional-access.h"

namespace Attr = NtfsBrowser::Attr;
using NtfsBrowser::FileRecord;
using NtfsBrowser::IndexEntry;
using NtfsBrowser::NtfsVolume;
namespace Cache = NtfsBrowser::Cache;
using NtfsBrowser::VolumeOptions;
namespace Mft = NtfsBrowser::Mft;

TEMPLATE_TEST_CASE_SIG(
    "FindSubEntry follows $ATTRIBUTE_LIST to a relocated $INDEX_ROOT",
    "[file-record][regression]", ((Cache::Strategy S), S),
    Cache::Strategy::NoCache, Cache::Strategy::FullCache) {
  auto reader = std::make_unique<NtfsBrowserTests::MemoryDiskReader>(
      NtfsBrowserTests::BuildFakeNtfsImageWithAttributeListDirectory());

  const NtfsVolume<S> volume(std::move(reader));
  REQUIRE(volume.IsVolumeOK());

  FileRecord<S> dir(volume);
  dir.SetAttrMask(Attr::Mask::IndexRoot | Attr::Mask::IndexAllocation);

  REQUIRE(dir.ParseFileRecord(NtfsBrowserTests::attribute_list_dir_idx));
  REQUIRE(dir.ParseAttrs());

  CHECK_FALSE(dir.GetAttr(Attr::Type::IndexRoot).empty());

  const std::optional<IndexEntry> found = dir.FindSubEntry(L"Foo");
  REQUIRE(found.has_value());
  CHECK(NtfsBrowserTests::Unwrap(found).GetFileReference() == 20);
}

TEMPLATE_TEST_CASE_SIG(
    "AttrList merges every attribute type relocated into the same "
    "extension record",
    "[file-record][regression]", ((Cache::Strategy S), S),
    Cache::Strategy::NoCache, Cache::Strategy::FullCache) {
  auto reader = std::make_unique<NtfsBrowserTests::MemoryDiskReader>(
      NtfsBrowserTests::
          BuildFakeNtfsImageWithMultiTypeAttributeListDirectory());

  const NtfsVolume<S> volume(std::move(reader));
  REQUIRE(volume.IsVolumeOK());

  FileRecord<S> dir(volume);
  dir.SetAttrMask(Attr::Mask::IndexRoot | Attr::Mask::IndexAllocation);

  REQUIRE(dir.ParseFileRecord(NtfsBrowserTests::attr_list_multi_type_dir_idx));
  REQUIRE(dir.ParseAttrs());

  CHECK_FALSE(dir.GetAttr(Attr::Type::IndexRoot).empty());

  // Both entries name the same extension record, but different types.
  CHECK_FALSE(dir.GetAttr(Attr::Type::IndexAllocation).empty());
}

TEMPLATE_TEST_CASE_SIG(
    "AttrList chain state does not leak across FileRecord::ParseFileRecord "
    "calls on a reused FileRecord",
    "[file-record][regression]", ((Cache::Strategy S), S),
    Cache::Strategy::NoCache, Cache::Strategy::FullCache) {
  auto reader = std::make_unique<NtfsBrowserTests::MemoryDiskReader>(
      NtfsBrowserTests::BuildFakeNtfsImageWithAttributeListDirectory());

  const NtfsVolume<S> volume(std::move(reader));
  REQUIRE(volume.IsVolumeOK());

  FileRecord<S> dir(volume);
  dir.SetAttrMask(Attr::Mask::IndexRoot | Attr::Mask::IndexAllocation);

  REQUIRE(dir.ParseFileRecord(NtfsBrowserTests::attribute_list_dir_idx));
  REQUIRE(dir.ParseAttrs());
  CHECK_FALSE(dir.GetAttr(Attr::Type::IndexRoot).empty());

  // Same FileRecord, same directory: it relocates to the same record again.
  REQUIRE(dir.ParseFileRecord(NtfsBrowserTests::attribute_list_dir_idx));
  REQUIRE(dir.ParseAttrs());
  CHECK_FALSE(dir.GetAttr(Attr::Type::IndexRoot).empty());
}

TEMPLATE_TEST_CASE_SIG(
    "AttrList's FileRecord vector growth does not invalidate "
    "already-resolved extension records' attributes",
    "[file-record][regression]", ((Cache::Strategy S), S),
    Cache::Strategy::NoCache, Cache::Strategy::FullCache) {
  auto reader = std::make_unique<NtfsBrowserTests::MemoryDiskReader>(
      NtfsBrowserTests::
          BuildFakeNtfsImageWithFragmentedAttributeListDirectory());

  const NtfsVolume<S> volume(std::move(reader));
  REQUIRE(volume.IsVolumeOK());

  FileRecord<S> dir(volume);
  dir.SetAttrMask(Attr::Mask::IndexAllocation);

  REQUIRE(dir.ParseFileRecord(NtfsBrowserTests::uaf_attr_list_dir_idx));
  REQUIRE(dir.ParseAttrs());

  const auto& alloc_attrs = dir.GetAttr(Attr::Type::IndexAllocation);
  REQUIRE(alloc_attrs.size() ==
          NtfsBrowserTests::uaf_real_size_sentinels.size());

  // Each GetDataSize() reads through a reference bound at that extension
  // record's construction time, so a moved FileRecord reads stale memory.
  for (size_t i = 0; i < alloc_attrs.size(); i++) {
    // The REQUIRE above checks the size of allocAttrs.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    CHECK(
        alloc_attrs[i]->GetDataSize() ==
        // The REQUIRE above makes allocAttrs as long as
        // uaf_real_size_sentinels.
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        NtfsBrowserTests::uaf_real_size_sentinels[i]);
  }
}

TEMPLATE_TEST_CASE_SIG(
    "AttrList stops cleanly on a resident $ATTRIBUTE_LIST whose size isn't "
    "a multiple of the entry size, when recovering",
    "[attr-list][regression]", ((Cache::Strategy S), S),
    Cache::Strategy::NoCache, Cache::Strategy::FullCache) {
  auto reader = std::make_unique<NtfsBrowserTests::MemoryDiskReader>(
      NtfsBrowserTests::BuildFakeNtfsImageWithAttributeListShortRead());

  const NtfsVolume<S> volume(std::move(reader),
                             VolumeOptions{.recover_errors = true});
  REQUIRE(volume.IsVolumeOK());

  FileRecord<S> record(volume);
  REQUIRE(record.ParseFileRecord(static_cast<ULONGLONG>(Mft::Idx::Root)));
  CHECK(record.ParseAttrs());
}

TEMPLATE_TEST_CASE_SIG(
    "A resident $ATTRIBUTE_LIST whose size isn't a multiple of the entry "
    "size rejects the record by default",
    "[attr-list][regression]", ((Cache::Strategy S), S),
    Cache::Strategy::NoCache, Cache::Strategy::FullCache) {
  auto reader = std::make_unique<NtfsBrowserTests::MemoryDiskReader>(
      NtfsBrowserTests::BuildFakeNtfsImageWithAttributeListShortRead());

  const NtfsVolume<S> volume(std::move(reader));
  REQUIRE(volume.IsVolumeOK());

  FileRecord<S> record(volume);
  REQUIRE(record.ParseFileRecord(static_cast<ULONGLONG>(Mft::Idx::Root)));
  CHECK_FALSE(record.ParseAttrs());
}

TEMPLATE_TEST_CASE_SIG(
    "AttrList stops cleanly on an entry whose record_size is smaller than "
    "the entry header, when recovering",
    "[attr-list][regression]", ((Cache::Strategy S), S),
    Cache::Strategy::NoCache, Cache::Strategy::FullCache) {
  auto reader = std::make_unique<NtfsBrowserTests::MemoryDiskReader>(
      NtfsBrowserTests::
          BuildFakeNtfsImageWithAttributeListRecordSizeTooSmall());

  const NtfsVolume<S> volume(std::move(reader),
                             VolumeOptions{.recover_errors = true});
  REQUIRE(volume.IsVolumeOK());

  FileRecord<S> dir(volume);
  dir.SetAttrMask(Attr::Mask::IndexRoot);
  REQUIRE(dir.ParseFileRecord(NtfsBrowserTests::attribute_list_dir_idx));
  CHECK(dir.ParseAttrs());
  CHECK_FALSE(dir.GetAttr(Attr::Type::IndexRoot).empty());
}

TEMPLATE_TEST_CASE_SIG(
    "An $ATTRIBUTE_LIST entry whose record_size is smaller than the entry "
    "header rejects the record by default",
    "[attr-list][regression]", ((Cache::Strategy S), S),
    Cache::Strategy::NoCache, Cache::Strategy::FullCache) {
  auto reader = std::make_unique<NtfsBrowserTests::MemoryDiskReader>(
      NtfsBrowserTests::
          BuildFakeNtfsImageWithAttributeListRecordSizeTooSmall());

  const NtfsVolume<S> volume(std::move(reader));
  REQUIRE(volume.IsVolumeOK());

  FileRecord<S> dir(volume);
  dir.SetAttrMask(Attr::Mask::IndexRoot);
  REQUIRE(dir.ParseFileRecord(NtfsBrowserTests::attribute_list_dir_idx));
  CHECK_FALSE(dir.ParseAttrs());
}

TEMPLATE_TEST_CASE_SIG(
    "AttrList stops cleanly when an entry's record_size overshoots the "
    "attribute's declared size, when recovering",
    "[attr-list][regression]", ((Cache::Strategy S), S),
    Cache::Strategy::NoCache, Cache::Strategy::FullCache) {
  auto reader = std::make_unique<NtfsBrowserTests::MemoryDiskReader>(
      NtfsBrowserTests::BuildFakeNtfsImageWithAttributeListOffsetMismatch());

  const NtfsVolume<S> volume(std::move(reader),
                             VolumeOptions{.recover_errors = true});
  REQUIRE(volume.IsVolumeOK());

  FileRecord<S> dir(volume);
  dir.SetAttrMask(Attr::Mask::IndexRoot);
  REQUIRE(dir.ParseFileRecord(NtfsBrowserTests::attribute_list_dir_idx));
  CHECK(dir.ParseAttrs());
  CHECK_FALSE(dir.GetAttr(Attr::Type::IndexRoot).empty());
}

TEMPLATE_TEST_CASE_SIG(
    "An $ATTRIBUTE_LIST entry whose record_size overshoots the attribute's "
    "declared size rejects the record by default",
    "[attr-list][regression]", ((Cache::Strategy S), S),
    Cache::Strategy::NoCache, Cache::Strategy::FullCache) {
  auto reader = std::make_unique<NtfsBrowserTests::MemoryDiskReader>(
      NtfsBrowserTests::BuildFakeNtfsImageWithAttributeListOffsetMismatch());

  const NtfsVolume<S> volume(std::move(reader));
  REQUIRE(volume.IsVolumeOK());

  FileRecord<S> dir(volume);
  dir.SetAttrMask(Attr::Mask::IndexRoot);
  REQUIRE(dir.ParseFileRecord(NtfsBrowserTests::attribute_list_dir_idx));
  CHECK_FALSE(dir.ParseAttrs());
}

TEMPLATE_TEST_CASE_SIG(
    "AttrList's cycle guard stops a two-record $ATTRIBUTE_LIST resolution "
    "cycle instead of recursing without bound",
    "[attr-list][regression]", ((Cache::Strategy S), S),
    Cache::Strategy::NoCache, Cache::Strategy::FullCache) {
  auto reader = std::make_unique<NtfsBrowserTests::MemoryDiskReader>(
      NtfsBrowserTests::BuildFakeNtfsImageWithAttributeListCycle());

  const NtfsVolume<S> volume(std::move(reader));
  REQUIRE(volume.IsVolumeOK());

  FileRecord<S> record(volume);
  REQUIRE(record.ParseFileRecord(static_cast<ULONGLONG>(Mft::Idx::Root)));
  CHECK(record.ParseAttrs());
}

TEMPLATE_TEST_CASE_SIG(
    "AttrList resolves every entry in a densely-packed (real 26-byte "
    "stride) $ATTRIBUTE_LIST, not just those a multiple of "
    "sizeof(Data::AttributeList) apart",
    "[attr-list][regression]", ((Cache::Strategy S), S),
    Cache::Strategy::NoCache, Cache::Strategy::FullCache) {
  auto reader = std::make_unique<NtfsBrowserTests::MemoryDiskReader>(
      NtfsBrowserTests::
          BuildFakeNtfsImageWithTightlyPackedAttributeListDirectory());

  const NtfsVolume<S> volume(std::move(reader));
  REQUIRE(volume.IsVolumeOK());

  FileRecord<S> dir(volume);
  dir.SetAttrMask(Attr::Mask::IndexRoot | Attr::Mask::IndexAllocation);

  REQUIRE(dir.ParseFileRecord(NtfsBrowserTests::attr_list_tight_pack_dir_idx));
  REQUIRE(dir.ParseAttrs());

  const std::optional<IndexEntry> found = dir.FindSubEntry(L"Foo");
  REQUIRE(found.has_value());
  CHECK(NtfsBrowserTests::Unwrap(found).GetFileReference() == 20);

  const auto& alloc_attrs = dir.GetAttr(Attr::Type::IndexAllocation);
  REQUIRE(alloc_attrs.size() == 1);
  // The REQUIRE above checks the size of allocAttrs.
  // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
  CHECK(alloc_attrs[0]->GetDataSize() ==
        NtfsBrowserTests::attr_list_tight_pack_real_size);
}

TEMPLATE_TEST_CASE_SIG(
    "ReadFileRecord() resolves a record through $MFT's own DATA "
    "continuation, reached via $MFT's own $ATTRIBUTE_LIST",
    "[ntfs-volume][regression]", ((Cache::Strategy S), S),
    Cache::Strategy::NoCache, Cache::Strategy::FullCache) {
  auto reader = std::make_unique<NtfsBrowserTests::MemoryDiskReader>(
      NtfsBrowserTests::
          BuildFakeNtfsImageWithMftDataSplitAcrossAttributeList());

  const NtfsVolume<S> volume(std::move(reader));
  REQUIRE(volume.IsVolumeOK());
  // mft_data_ must be the base extent, not whichever instance parsed first.
  CHECK(volume.GetRecordsCount() == 1);

  FileRecord<S> record(volume);
  CHECK(record.ParseFileRecord(NtfsBrowserTests::mft_data_split_target_idx));
}

TEMPLATE_TEST_CASE_SIG(
    "ReadFileRecord() resolves a two-hop $MFT DATA continuation chain, "
    "where an earlier $ATTRIBUTE_LIST entry depends on a later one",
    "[ntfs-volume][regression]", ((Cache::Strategy S), S),
    Cache::Strategy::NoCache, Cache::Strategy::FullCache) {
  auto reader = std::make_unique<NtfsBrowserTests::MemoryDiskReader>(
      NtfsBrowserTests::BuildFakeNtfsImageWithMftDataExtentChain());

  const NtfsVolume<S> volume(std::move(reader));
  REQUIRE(volume.IsVolumeOK());

  FileRecord<S> record(volume);
  CHECK(record.ParseFileRecord(NtfsBrowserTests::mft_chain_ext_a));
  CHECK(record.ParseFileRecord(NtfsBrowserTests::mft_chain_ext_a_start_vcn));
}

TEMPLATE_TEST_CASE_SIG(
    "A permanently unresolvable $MFT DATA continuation does not take down "
    "an earlier, resolvable one in the same $ATTRIBUTE_LIST",
    "[ntfs-volume][regression]", ((Cache::Strategy S), S),
    Cache::Strategy::NoCache, Cache::Strategy::FullCache) {
  auto reader = std::make_unique<NtfsBrowserTests::MemoryDiskReader>(
      NtfsBrowserTests::BuildFakeNtfsImageWithUnresolvableMftDataExtent());

  const NtfsVolume<S> volume(std::move(reader));
  REQUIRE(volume.IsVolumeOK());

  FileRecord<S> record(volume);
  CHECK(record.ParseFileRecord(NtfsBrowserTests::mft_unresolvable_good_record));
  CHECK_FALSE(
      record.ParseFileRecord(NtfsBrowserTests::mft_unresolvable_ext_idx));
}

namespace {

// Fill that tells an unread byte from a read one.
constexpr BYTE unread_fill = 0xCC;

// Reads the first "size" bytes of "attr"; empty when the read fails.
template <Cache::Strategy S>
std::vector<BYTE> ReadFirstBytes(const NtfsBrowser::AttrBase<S>& attr,
                                 size_t size) {
  std::vector<BYTE> buffer(size, unread_fill);
  const std::optional<ULONGLONG> read = attr.ReadData(0, buffer);
  if (!read) {
    return {};
  }
  buffer.resize(gsl::narrow<size_t>(*read));
  return buffer;
}

// Allocates blocks of every size a freed file record could have had, filled
// with a byte that is not part of the expected content: whatever a stale read
// meets there is then wrong, on any heap that recycles freed blocks.
std::vector<std::vector<BYTE>> ScribbleOverFreedMemory() {
  // Smallest and largest block: a 4 KiB record buffer or header, and slack.
  constexpr size_t min_block = 64;
  constexpr size_t max_block = 8192;
  // The heap's size-class granularity, so every class is hit.
  constexpr size_t block_step = 16;
  // A class may hold several free blocks.
  constexpr size_t blocks_per_size = 4;
  // Not in attr_list_lifetime_data_content, and not the debug heap's 0xDD.
  constexpr BYTE fill = 0xEE;

  std::vector<std::vector<BYTE>> blocks;
  for (size_t size = min_block; size <= max_block; size += block_step) {
    for (size_t i = 0; i < blocks_per_size; i++) {
      blocks.emplace_back(size, fill);
    }
  }
  return blocks;
}

const std::vector<BYTE> expected_lifetime_content(
    NtfsBrowserTests::attr_list_lifetime_data_content.begin(),
    NtfsBrowserTests::attr_list_lifetime_data_content.end());

}  // namespace

TEMPLATE_TEST_CASE_SIG(
    "An $ATTRIBUTE_LIST that fails after importing an attribute leaves it "
    "readable, when recovering",
    "[attr-list][regression]", ((Cache::Strategy S), S),
    Cache::Strategy::NoCache, Cache::Strategy::FullCache) {
  auto reader = std::make_unique<NtfsBrowserTests::MemoryDiskReader>(
      NtfsBrowserTests::
          BuildFakeNtfsImageWithAttributeListImportThenZeroRecordSize());

  const NtfsVolume<S> volume(std::move(reader),
                             VolumeOptions{.recover_errors = true});
  REQUIRE(volume.IsVolumeOK());

  FileRecord<S> record(volume);
  REQUIRE(
      record.ParseFileRecord(NtfsBrowserTests::attr_list_lifetime_base_idx));
  CHECK_FALSE(record.ParseAttrs());

  const auto scribbled = ScribbleOverFreedMemory();
  // The imported attribute's bytes live in the extension record.
  const auto& data = record.GetAttr(Attr::Type::Data);
  REQUIRE(data.size() == 1);
  CHECK(data.front()->GetAttrType() == Attr::Type::Data);
  CHECK(ReadFirstBytes<S>(*data.front(), expected_lifetime_content.size()) ==
        expected_lifetime_content);
}

TEMPLATE_TEST_CASE_SIG(
    "Two $ATTRIBUTE_LIST attributes with contiguous VCNs leave the imported "
    "attribute readable",
    "[attr-list][regression]", ((Cache::Strategy S), S),
    Cache::Strategy::NoCache, Cache::Strategy::FullCache) {
  auto reader = std::make_unique<NtfsBrowserTests::MemoryDiskReader>(
      NtfsBrowserTests::BuildFakeNtfsImageWithSplitAttributeListAttribute());

  const NtfsVolume<S> volume(std::move(reader));
  REQUIRE(volume.IsVolumeOK());

  FileRecord<S> record(volume);
  REQUIRE(
      record.ParseFileRecord(NtfsBrowserTests::attr_list_lifetime_base_idx));
  REQUIRE(record.ParseAttrs());

  const auto scribbled = ScribbleOverFreedMemory();
  // The imported attribute's bytes live in the extension record.
  const auto& data = record.GetAttr(Attr::Type::Data);
  REQUIRE(data.size() == 1);
  CHECK(data.front()->GetAttrType() == Attr::Type::Data);
  CHECK(ReadFirstBytes<S>(*data.front(), expected_lifetime_content.size()) ==
        expected_lifetime_content);
}

TEMPLATE_TEST_CASE_SIG(
    "A recovering parse that stops on a malformed attribute still merges "
    "a split attribute",
    "[attr-list][regression]", ((Cache::Strategy S), S),
    Cache::Strategy::NoCache, Cache::Strategy::FullCache) {
  const auto defect =
      GENERATE(NtfsBrowserTests::FakeTrailingDefect::UndersizedHeader,
               NtfsBrowserTests::FakeTrailingDefect::UndersizedCompressedField,
               NtfsBrowserTests::FakeTrailingDefect::RejectedAttribute);

  auto reader = std::make_unique<NtfsBrowserTests::MemoryDiskReader>(
      NtfsBrowserTests::BuildFakeNtfsImageWithSplitDataAndTrailingDefect(
          defect));

  const NtfsVolume<S> volume(std::move(reader),
                             VolumeOptions{.recover_errors = true});
  REQUIRE(volume.IsVolumeOK());

  FileRecord<S> record(volume);
  REQUIRE(
      record.ParseFileRecord(NtfsBrowserTests::attr_list_lifetime_base_idx));
  CHECK_FALSE(record.ParseAttrs());

  // One stream, not its VCN 0 and VCN 1 halves.
  const auto& data = record.GetAttr(Attr::Type::Data);
  REQUIRE(data.size() == 1);
  CHECK(data.front()->GetDataSize() == NtfsBrowserTests::fake_cluster_size);
}

namespace {

// Longest a volume open may take before it counts as hung. A healthy open of
// a fake image takes milliseconds.
constexpr std::chrono::seconds open_timeout{10};

// Opens a volume over image on a worker thread. Returns false if that did not
// finish within open_timeout. The worker is then detached: it keeps spinning
// until the process exits, since a hung constructor cannot be cancelled.
template <Cache::Strategy S>
bool OpensWithinTimeout(std::vector<BYTE> image) {
  const auto done = std::make_shared<std::promise<void>>();
  std::future<void> finished = done->get_future();

  std::thread worker([done, image = std::move(image)]() mutable {
    try {
      const NtfsVolume<S> volume(
          std::make_unique<NtfsBrowserTests::MemoryDiskReader>(
              std::move(image)));
      done->set_value();
    } catch (...) {
      done->set_exception(std::current_exception());
    }
  });

  if (finished.wait_for(open_timeout) != std::future_status::ready) {
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
    "[ntfs-volume][regression]", ((Cache::Strategy S), S),
    Cache::Strategy::NoCache, Cache::Strategy::FullCache) {
  CHECK(OpensWithinTimeout<S>(
      NtfsBrowserTests::BuildFakeNtfsImageWithMftDataLastVcnOverflow()));
}

namespace {

// Extension links that do not belong to attr_list_lifetime_base_idx's list
// entry: a reused record, or a record of another file.
constexpr auto foreign_links =
    std::to_array<NtfsBrowserTests::FakeExtensionLink>({
        // Same base, but the record has since been reused (sequence 3 -> 4).
        {.entry_sequence = 3, .record_sequence = 4, .base_ref = 6},
        // The record was reused by a live file of its own: no base at all.
        {.entry_sequence = 3, .record_sequence = 4, .base_ref = 0},
        // Same sequence, but the record is an extension of another file.
        {.entry_sequence = 0, .record_sequence = 0, .base_ref = 8},
    });

}  // namespace

TEMPLATE_TEST_CASE_SIG(
    "An $ATTRIBUTE_LIST entry naming another file's record rejects the "
    "record by default",
    "[attr-list][regression]", ((Cache::Strategy S), S),
    Cache::Strategy::NoCache, Cache::Strategy::FullCache) {
  const NtfsBrowserTests::FakeExtensionLink link =
      foreign_links.at(GENERATE(size_t{0}, size_t{1}, size_t{2}));

  auto reader = std::make_unique<NtfsBrowserTests::MemoryDiskReader>(
      NtfsBrowserTests::BuildFakeNtfsImageWithExtensionLink(link));

  const NtfsVolume<S> volume(std::move(reader));
  REQUIRE(volume.IsVolumeOK());

  FileRecord<S> record(volume);
  REQUIRE(
      record.ParseFileRecord(NtfsBrowserTests::attr_list_lifetime_base_idx));
  CHECK_FALSE(record.ParseAttrs());
  CHECK(record.GetAttr(Attr::Type::Data).empty());
}

TEMPLATE_TEST_CASE_SIG(
    "An $ATTRIBUTE_LIST entry naming another file's record is skipped when "
    "recovering",
    "[attr-list][regression]", ((Cache::Strategy S), S),
    Cache::Strategy::NoCache, Cache::Strategy::FullCache) {
  const NtfsBrowserTests::FakeExtensionLink link =
      foreign_links.at(GENERATE(size_t{0}, size_t{1}, size_t{2}));

  auto reader = std::make_unique<NtfsBrowserTests::MemoryDiskReader>(
      NtfsBrowserTests::BuildFakeNtfsImageWithExtensionLink(link));

  const NtfsVolume<S> volume(std::move(reader),
                             VolumeOptions{.recover_errors = true});
  REQUIRE(volume.IsVolumeOK());

  FileRecord<S> record(volume);
  REQUIRE(
      record.ParseFileRecord(NtfsBrowserTests::attr_list_lifetime_base_idx));
  CHECK(record.ParseAttrs());
  CHECK(record.GetAttr(Attr::Type::Data).empty());
}

TEMPLATE_TEST_CASE_SIG(
    "An $ATTRIBUTE_LIST entry naming a genuine extension record still "
    "imports its attribute, sequence numbers included",
    "[attr-list][regression]", ((Cache::Strategy S), S),
    Cache::Strategy::NoCache, Cache::Strategy::FullCache) {
  auto reader = std::make_unique<NtfsBrowserTests::MemoryDiskReader>(
      NtfsBrowserTests::BuildFakeNtfsImageWithExtensionLink(
          NtfsBrowserTests::genuine_extension_link));

  const NtfsVolume<S> volume(std::move(reader));
  REQUIRE(volume.IsVolumeOK());

  FileRecord<S> record(volume);
  REQUIRE(
      record.ParseFileRecord(NtfsBrowserTests::attr_list_lifetime_base_idx));
  REQUIRE(record.ParseAttrs());

  const auto& data = record.GetAttr(Attr::Type::Data);
  REQUIRE(data.size() == 1);
  CHECK(ReadFirstBytes<S>(*data.front(), expected_lifetime_content.size()) ==
        expected_lifetime_content);
}

TEMPLATE_TEST_CASE_SIG(
    "A raw attribute callback installed on a record can discard an "
    "attribute imported from an extension record",
    "[attr-list][regression]", ((Cache::Strategy S), S),
    Cache::Strategy::NoCache, Cache::Strategy::FullCache) {
  auto reader = std::make_unique<NtfsBrowserTests::MemoryDiskReader>(
      NtfsBrowserTests::BuildFakeNtfsImageWithExtensionLink(
          NtfsBrowserTests::genuine_extension_link));

  const NtfsVolume<S> volume(std::move(reader));
  REQUIRE(volume.IsVolumeOK());

  FileRecord<S> record(volume);
  REQUIRE(record.InstallAttrRawCB(Attr::Type::Data,
                                  [](const NtfsBrowser::Attr::HeaderCommon&,
                                     bool& discard) { discard = true; }));
  REQUIRE(
      record.ParseFileRecord(NtfsBrowserTests::attr_list_lifetime_base_idx));
  REQUIRE(record.ParseAttrs());

  CHECK(record.GetAttr(Attr::Type::Data).empty());
}

TEMPLATE_TEST_CASE_SIG(
    "$MFT's own DATA continuation is ignored when its extension record was "
    "reused under another sequence number",
    "[ntfs-volume][regression]", ((Cache::Strategy S), S),
    Cache::Strategy::NoCache, Cache::Strategy::FullCache) {
  auto reader = std::make_unique<NtfsBrowserTests::MemoryDiskReader>(
      NtfsBrowserTests::BuildFakeNtfsImageWithMftDataSplitLink(
          {.entry_sequence = 3, .record_sequence = 4}));

  const NtfsVolume<S> volume(std::move(reader));
  REQUIRE(volume.IsVolumeOK());

  FileRecord<S> record(volume);
  CHECK_FALSE(
      record.ParseFileRecord(NtfsBrowserTests::mft_data_split_target_idx));
}

TEMPLATE_TEST_CASE_SIG(
    "$MFT's own DATA continuation is followed when its extension record "
    "carries the sequence number its list entry names",
    "[ntfs-volume][regression]", ((Cache::Strategy S), S),
    Cache::Strategy::NoCache, Cache::Strategy::FullCache) {
  auto reader = std::make_unique<NtfsBrowserTests::MemoryDiskReader>(
      NtfsBrowserTests::BuildFakeNtfsImageWithMftDataSplitLink(
          {.entry_sequence = 3, .record_sequence = 3}));

  const NtfsVolume<S> volume(std::move(reader));
  REQUIRE(volume.IsVolumeOK());

  FileRecord<S> record(volume);
  CHECK(record.ParseFileRecord(NtfsBrowserTests::mft_data_split_target_idx));
}

TEMPLATE_TEST_CASE_SIG(
    "Two $MFT DATA extents held by one extension record are both mapped",
    "[ntfs-volume][regression]", ((Cache::Strategy S), S),
    Cache::Strategy::NoCache, Cache::Strategy::FullCache) {
  auto reader = std::make_unique<NtfsBrowserTests::MemoryDiskReader>(
      NtfsBrowserTests::BuildFakeNtfsImageWithMftDataTwoExtentsInOneRecord());

  const NtfsVolume<S> volume(std::move(reader));
  REQUIRE(volume.IsVolumeOK());

  FileRecord<S> record(volume);
  CHECK(record.ParseFileRecord(NtfsBrowserTests::mft_two_extents_first_vcn));
  CHECK(record.ParseFileRecord(NtfsBrowserTests::mft_two_extents_second_vcn));
}
