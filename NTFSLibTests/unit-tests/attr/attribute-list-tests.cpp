#include <ntfs-browser/win-types.h>

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

#include <ntfs-browser/attr/mask.h>
#include <ntfs-browser/attr/type.h>
#include <ntfs-browser/cache/strategy.h>
#include <ntfs-browser/index-entry.h>
#include <ntfs-browser/io/file-record.h>
#include <ntfs-browser/mft/idx.h>
#include <ntfs-browser/ntfs-volume.h>

#include "attribute-list-support.h"
#include "fake-ntfs-image.h"
#include "memory-disk-reader.h"
#include "optional-access.h"

namespace Attr = NtfsBrowser::Attr;
using NtfsBrowser::IndexEntry;
using NtfsBrowser::NtfsVolume;
namespace Cache = NtfsBrowser::Cache;
namespace Mft = NtfsBrowser::Mft;

TEMPLATE_TEST_CASE_SIG(
    "FindSubEntry follows $ATTRIBUTE_LIST to a relocated $INDEX_ROOT",
    "[file-record][regression]", ((Cache::Strategy S), S),
    Cache::Strategy::NoCache, Cache::Strategy::FullCache) {
  const ParsedRecord<S> dir = OpenListRecord<S>(
      NtfsBrowserTests::BuildFakeNtfsImageWithAttributeListDirectory(),
      NtfsBrowserTests::attribute_list_dir_idx,
      Attr::Mask::IndexRoot | Attr::Mask::IndexAllocation,
      VolumeOptions{.recover_errors = true});
  REQUIRE(dir.record->ParseAttrs());

  CHECK_FALSE(dir.record->GetAttr(Attr::Type::IndexRoot).empty());

  const std::optional<IndexEntry> found = dir.record->FindSubEntry(L"Foo");
  REQUIRE(found.has_value());
  CHECK(NtfsBrowserTests::Unwrap(found).GetFileReference() == 20);
}

TEMPLATE_TEST_CASE_SIG(
    "AttrList merges every attribute type relocated into the same "
    "extension record",
    "[file-record][regression]", ((Cache::Strategy S), S),
    Cache::Strategy::NoCache, Cache::Strategy::FullCache) {
  const ParsedRecord<S> dir = OpenListRecord<S>(
      NtfsBrowserTests::BuildFakeNtfsImageWithMultiTypeAttributeListDirectory(),
      NtfsBrowserTests::attr_list_multi_type_dir_idx,
      Attr::Mask::IndexRoot | Attr::Mask::IndexAllocation);
  REQUIRE(dir.record->ParseAttrs());

  CHECK_FALSE(dir.record->GetAttr(Attr::Type::IndexRoot).empty());

  // Both entries name the same extension record, but different types.
  CHECK_FALSE(dir.record->GetAttr(Attr::Type::IndexAllocation).empty());
}

TEMPLATE_TEST_CASE_SIG(
    "AttrList chain state does not leak across FileRecord::ParseFileRecord "
    "calls on a reused FileRecord",
    "[file-record][regression]", ((Cache::Strategy S), S),
    Cache::Strategy::NoCache, Cache::Strategy::FullCache) {
  const ParsedRecord<S> dir = OpenListRecord<S>(
      NtfsBrowserTests::BuildFakeNtfsImageWithAttributeListDirectory(),
      NtfsBrowserTests::attribute_list_dir_idx,
      Attr::Mask::IndexRoot | Attr::Mask::IndexAllocation);
  REQUIRE(dir.record->ParseAttrs());
  CHECK_FALSE(dir.record->GetAttr(Attr::Type::IndexRoot).empty());

  // Same FileRecord, same directory: it relocates to the same record again.
  REQUIRE(
      dir.record->ParseFileRecord(NtfsBrowserTests::attribute_list_dir_idx));
  REQUIRE(dir.record->ParseAttrs());
  CHECK_FALSE(dir.record->GetAttr(Attr::Type::IndexRoot).empty());
}

TEMPLATE_TEST_CASE_SIG(
    "AttrList's FileRecord vector growth does not invalidate "
    "already-resolved extension records' attributes",
    "[file-record][regression]", ((Cache::Strategy S), S),
    Cache::Strategy::NoCache, Cache::Strategy::FullCache) {
  const ParsedRecord<S> dir = OpenListRecord<S>(
      NtfsBrowserTests::
          BuildFakeNtfsImageWithFragmentedAttributeListDirectory(),
      NtfsBrowserTests::uaf_attr_list_dir_idx, Attr::Mask::IndexAllocation);
  REQUIRE(dir.record->ParseAttrs());

  const auto& alloc_attrs = dir.record->GetAttr(Attr::Type::IndexAllocation);
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
    "AttrList's cycle guard stops a two-record $ATTRIBUTE_LIST resolution "
    "cycle instead of recursing without bound",
    "[attr-list][regression]", ((Cache::Strategy S), S),
    Cache::Strategy::NoCache, Cache::Strategy::FullCache) {
  const ParsedRecord<S> record = OpenRecord<S>(
      NtfsBrowserTests::BuildFakeNtfsImageWithAttributeListCycle(),
      static_cast<ULONGLONG>(Mft::Idx::Root));
  CHECK(record.record->ParseAttrs());
}

TEMPLATE_TEST_CASE_SIG(
    "AttrList resolves every entry in a densely-packed (real 26-byte "
    "stride) $ATTRIBUTE_LIST, not just those a multiple of "
    "sizeof(Data::AttributeList) apart",
    "[attr-list][regression]", ((Cache::Strategy S), S),
    Cache::Strategy::NoCache, Cache::Strategy::FullCache) {
  const ParsedRecord<S> dir = OpenListRecord<S>(
      NtfsBrowserTests::
          BuildFakeNtfsImageWithTightlyPackedAttributeListDirectory(),
      NtfsBrowserTests::attr_list_tight_pack_dir_idx,
      Attr::Mask::IndexRoot | Attr::Mask::IndexAllocation,
      VolumeOptions{.recover_errors = true});
  REQUIRE(dir.record->ParseAttrs());

  const std::optional<IndexEntry> found = dir.record->FindSubEntry(L"Foo");
  REQUIRE(found.has_value());
  CHECK(NtfsBrowserTests::Unwrap(found).GetFileReference() == 20);

  const auto& alloc_attrs = dir.record->GetAttr(Attr::Type::IndexAllocation);
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
  const ParsedRecord<S> opened = OpenUnparsed<S>(
      NtfsBrowserTests::
          BuildFakeNtfsImageWithMftDataSplitAcrossAttributeList());
  // mft_data_ must be the base extent, not whichever instance parsed first.
  CHECK(opened.volume->GetRecordsCount() == 1);

  CHECK(opened.record->ParseFileRecord(
      NtfsBrowserTests::mft_data_split_target_idx));
}

TEMPLATE_TEST_CASE_SIG(
    "ReadFileRecord() resolves a two-hop $MFT DATA continuation chain, "
    "where an earlier $ATTRIBUTE_LIST entry depends on a later one",
    "[ntfs-volume][regression]", ((Cache::Strategy S), S),
    Cache::Strategy::NoCache, Cache::Strategy::FullCache) {
  const ParsedRecord<S> opened = OpenUnparsed<S>(
      NtfsBrowserTests::BuildFakeNtfsImageWithMftDataExtentChain());

  CHECK(opened.record->ParseFileRecord(NtfsBrowserTests::mft_chain_ext_a));
  CHECK(opened.record->ParseFileRecord(
      NtfsBrowserTests::mft_chain_ext_a_start_vcn));
}

TEMPLATE_TEST_CASE_SIG(
    "A permanently unresolvable $MFT DATA continuation does not take down "
    "an earlier, resolvable one in the same $ATTRIBUTE_LIST",
    "[ntfs-volume][regression]", ((Cache::Strategy S), S),
    Cache::Strategy::NoCache, Cache::Strategy::FullCache) {
  const ParsedRecord<S> opened = OpenUnparsed<S>(
      NtfsBrowserTests::BuildFakeNtfsImageWithUnresolvableMftDataExtent());

  CHECK(opened.record->ParseFileRecord(
      NtfsBrowserTests::mft_unresolvable_good_record));
  CHECK_FALSE(opened.record->ParseFileRecord(
      NtfsBrowserTests::mft_unresolvable_ext_idx));
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

TEMPLATE_TEST_CASE_SIG(
    "$MFT's own DATA continuation is ignored when its extension record was "
    "reused under another sequence number",
    "[ntfs-volume][regression]", ((Cache::Strategy S), S),
    Cache::Strategy::NoCache, Cache::Strategy::FullCache) {
  const ParsedRecord<S> opened =
      OpenUnparsed<S>(NtfsBrowserTests::BuildFakeNtfsImageWithMftDataSplitLink(
          {.entry_sequence = 3, .record_sequence = 4}));

  CHECK_FALSE(opened.record->ParseFileRecord(
      NtfsBrowserTests::mft_data_split_target_idx));
}

TEMPLATE_TEST_CASE_SIG(
    "$MFT's own DATA continuation is followed when its extension record "
    "carries the sequence number its list entry names",
    "[ntfs-volume][regression]", ((Cache::Strategy S), S),
    Cache::Strategy::NoCache, Cache::Strategy::FullCache) {
  const ParsedRecord<S> opened =
      OpenUnparsed<S>(NtfsBrowserTests::BuildFakeNtfsImageWithMftDataSplitLink(
          {.entry_sequence = 3, .record_sequence = 3}));

  CHECK(opened.record->ParseFileRecord(
      NtfsBrowserTests::mft_data_split_target_idx));
}

TEMPLATE_TEST_CASE_SIG(
    "Two $MFT DATA extents held by one extension record are both mapped",
    "[ntfs-volume][regression]", ((Cache::Strategy S), S),
    Cache::Strategy::NoCache, Cache::Strategy::FullCache) {
  const ParsedRecord<S> opened = OpenUnparsed<S>(
      NtfsBrowserTests::BuildFakeNtfsImageWithMftDataTwoExtentsInOneRecord());

  CHECK(opened.record->ParseFileRecord(
      NtfsBrowserTests::mft_two_extents_first_vcn));
  CHECK(opened.record->ParseFileRecord(
      NtfsBrowserTests::mft_two_extents_second_vcn));
}
