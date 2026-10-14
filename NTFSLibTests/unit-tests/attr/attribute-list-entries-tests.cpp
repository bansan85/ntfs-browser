#include <ntfs-browser/win-types.h>

#include <array>
#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <catch2/catch_template_test_macros.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <gsl/narrow>

#include <ntfs-browser/attr/mask.h>
#include <ntfs-browser/attr/type.h>
#include <ntfs-browser/cache/strategy.h>
#include <ntfs-browser/io/file-record.h>
#include <ntfs-browser/mft/idx.h>
#include <ntfs-browser/ntfs-volume.h>
#include <ntfs-browser/volume-options.h>

#include "attribute-list-support.h"
#include "fake-ntfs-image.h"
#include "memory-disk-reader.h"
#include "optional-access.h"
#include "test-log-sink.h"

namespace Attr = NtfsBrowser::Attr;
namespace Cache = NtfsBrowser::Cache;
namespace Mft = NtfsBrowser::Mft;
using NtfsBrowser::NtfsVolume;
using NtfsBrowser::VolumeOptions;

namespace {

// Message ParseAttrs() logs when an $ATTRIBUTE_LIST stops it short.
constexpr std::string_view truncated_message = "Attribute List is truncated.";

// Fill that tells an unread byte from a read one.
constexpr BYTE unread_fill = 0xCC;

// Reads the first "size" bytes of "attr"; empty when the read fails.
template <Cache::Strategy S>
std::vector<BYTE> ReadFirstBytes(const NtfsBrowser::Attr::AttrBase<S>& attr,
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

// Scribbles over freed memory, then checks the lone $DATA of record still
// reads its expected content.
template <Cache::Strategy S>
void CheckImportedDataReadable(NtfsBrowser::Io::FileRecord<S>& record) {
  const auto scribbled = ScribbleOverFreedMemory();
  // The imported attribute's bytes live in the extension record.
  const auto& data = record.GetAttr(Attr::Type::Data);
  REQUIRE(data.size() == 1);
  CHECK(data.front()->GetAttrType() == Attr::Type::Data);
  CHECK(ReadFirstBytes<S>(*data.front(), expected_lifetime_content.size()) ==
        expected_lifetime_content);
}

// Opens attr_list_lifetime_base_idx of the image with link as its extension.
template <Cache::Strategy S>
ParsedRecord<S> OpenWithExtensionLink(NtfsBrowserTests::FakeExtensionLink link,
                                      VolumeOptions options = {}) {
  return OpenRecord<S>(
      NtfsBrowserTests::BuildFakeNtfsImageWithExtensionLink(link),
      NtfsBrowserTests::attr_list_lifetime_base_idx, options);
}

// Checks a parse: its ParseAttrs() result, and that its log holds message.
void CheckListParse(const ListParse& outcome, bool parsed,
                    std::string_view message) {
  CHECK(outcome.parsed == parsed);
  CHECK_THAT(outcome.log,
             Catch::Matchers::ContainsSubstring(std::string(message)));
}

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
    "AttrList stops cleanly on a resident $ATTRIBUTE_LIST whose size isn't "
    "a multiple of the entry size, when recovering",
    "[attr-list][regression]", ((Cache::Strategy S), S),
    Cache::Strategy::NoCache, Cache::Strategy::FullCache) {
  const ParsedRecord<S> record = OpenRecord<S>(
      NtfsBrowserTests::BuildFakeNtfsImageWithAttributeListShortRead(),
      static_cast<ULONGLONG>(Mft::Idx::Root),
      VolumeOptions{.recover_errors = true});
  CHECK(record.record->ParseAttrs());
}

TEMPLATE_TEST_CASE_SIG(
    "A resident $ATTRIBUTE_LIST whose size isn't a multiple of the entry "
    "size rejects the record by default",
    "[attr-list][regression]", ((Cache::Strategy S), S),
    Cache::Strategy::NoCache, Cache::Strategy::FullCache) {
  const ParsedRecord<S> record = OpenRecord<S>(
      NtfsBrowserTests::BuildFakeNtfsImageWithAttributeListShortRead(),
      static_cast<ULONGLONG>(Mft::Idx::Root));
  CHECK_FALSE(record.record->ParseAttrs());
}

TEMPLATE_TEST_CASE_SIG(
    "AttrList stops cleanly on an entry whose record_size is smaller than "
    "the entry header, when recovering",
    "[attr-list][regression]", ((Cache::Strategy S), S),
    Cache::Strategy::NoCache, Cache::Strategy::FullCache) {
  const ParsedRecord<S> dir = OpenListRecord<S>(
      NtfsBrowserTests::BuildFakeNtfsImageWithAttributeListRecordSizeTooSmall(),
      NtfsBrowserTests::attribute_list_dir_idx, Attr::Mask::IndexRoot,
      VolumeOptions{.recover_errors = true});
  CHECK(dir.record->ParseAttrs());
  CHECK_FALSE(dir.record->GetAttr(Attr::Type::IndexRoot).empty());
}

TEMPLATE_TEST_CASE_SIG(
    "An $ATTRIBUTE_LIST entry whose record_size is smaller than the entry "
    "header rejects the record by default",
    "[attr-list][regression]", ((Cache::Strategy S), S),
    Cache::Strategy::NoCache, Cache::Strategy::FullCache) {
  const ParsedRecord<S> dir = OpenListRecord<S>(
      NtfsBrowserTests::BuildFakeNtfsImageWithAttributeListRecordSizeTooSmall(),
      NtfsBrowserTests::attribute_list_dir_idx, Attr::Mask::IndexRoot);
  CHECK_FALSE(dir.record->ParseAttrs());
}

TEMPLATE_TEST_CASE_SIG(
    "AttrList stops cleanly when an entry's record_size overshoots the "
    "attribute's declared size, when recovering",
    "[attr-list][regression]", ((Cache::Strategy S), S),
    Cache::Strategy::NoCache, Cache::Strategy::FullCache) {
  const ParsedRecord<S> dir = OpenListRecord<S>(
      NtfsBrowserTests::BuildFakeNtfsImageWithAttributeListOffsetMismatch(),
      NtfsBrowserTests::attribute_list_dir_idx, Attr::Mask::IndexRoot,
      VolumeOptions{.recover_errors = true});
  CHECK(dir.record->ParseAttrs());
  CHECK_FALSE(dir.record->GetAttr(Attr::Type::IndexRoot).empty());
}

TEMPLATE_TEST_CASE_SIG(
    "An $ATTRIBUTE_LIST entry whose record_size overshoots the attribute's "
    "declared size rejects the record by default",
    "[attr-list][regression]", ((Cache::Strategy S), S),
    Cache::Strategy::NoCache, Cache::Strategy::FullCache) {
  const ParsedRecord<S> dir = OpenListRecord<S>(
      NtfsBrowserTests::BuildFakeNtfsImageWithAttributeListOffsetMismatch(),
      NtfsBrowserTests::attribute_list_dir_idx, Attr::Mask::IndexRoot);
  CHECK_FALSE(dir.record->ParseAttrs());
}

TEMPLATE_TEST_CASE_SIG(
    "An $ATTRIBUTE_LIST that fails after importing an attribute leaves it "
    "readable, when recovering",
    "[attr-list][regression]", ((Cache::Strategy S), S),
    Cache::Strategy::NoCache, Cache::Strategy::FullCache) {
  const ParsedRecord<S> record = OpenRecord<S>(
      NtfsBrowserTests::
          BuildFakeNtfsImageWithAttributeListImportThenZeroRecordSize(),
      NtfsBrowserTests::attr_list_lifetime_base_idx,
      VolumeOptions{.recover_errors = true});
  CHECK_FALSE(record.record->ParseAttrs());

  CheckImportedDataReadable<S>(*record.record);
}

TEMPLATE_TEST_CASE_SIG(
    "Two $ATTRIBUTE_LIST attributes with contiguous VCNs leave the imported "
    "attribute readable",
    "[attr-list][regression]", ((Cache::Strategy S), S),
    Cache::Strategy::NoCache, Cache::Strategy::FullCache) {
  const ParsedRecord<S> record = OpenRecord<S>(
      NtfsBrowserTests::BuildFakeNtfsImageWithSplitAttributeListAttribute(),
      NtfsBrowserTests::attr_list_lifetime_base_idx);
  REQUIRE(record.record->ParseAttrs());

  CheckImportedDataReadable<S>(*record.record);
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

  const ParsedRecord<S> record = OpenRecord<S>(
      NtfsBrowserTests::BuildFakeNtfsImageWithSplitDataAndTrailingDefect(
          defect),
      NtfsBrowserTests::attr_list_lifetime_base_idx,
      VolumeOptions{.recover_errors = true});
  CHECK_FALSE(record.record->ParseAttrs());

  // One stream, not its VCN 0 and VCN 1 halves.
  const auto& data = record.record->GetAttr(Attr::Type::Data);
  REQUIRE(data.size() == 1);
  CHECK(data.front()->GetDataSize() == NtfsBrowserTests::fake_cluster_size);
}

TEMPLATE_TEST_CASE_SIG(
    "An $ATTRIBUTE_LIST entry naming another file's record rejects the "
    "record by default",
    "[attr-list][regression]", ((Cache::Strategy S), S),
    Cache::Strategy::NoCache, Cache::Strategy::FullCache) {
  const NtfsBrowserTests::FakeExtensionLink link =
      foreign_links.at(GENERATE(size_t{0}, size_t{1}, size_t{2}));

  const ParsedRecord<S> record = OpenWithExtensionLink<S>(link);
  CHECK_FALSE(record.record->ParseAttrs());
  CHECK(record.record->GetAttr(Attr::Type::Data).empty());
}

TEMPLATE_TEST_CASE_SIG(
    "An $ATTRIBUTE_LIST entry naming another file's record is skipped when "
    "recovering",
    "[attr-list][regression]", ((Cache::Strategy S), S),
    Cache::Strategy::NoCache, Cache::Strategy::FullCache) {
  const NtfsBrowserTests::FakeExtensionLink link =
      foreign_links.at(GENERATE(size_t{0}, size_t{1}, size_t{2}));

  const ParsedRecord<S> record =
      OpenWithExtensionLink<S>(link, VolumeOptions{.recover_errors = true});
  CHECK(record.record->ParseAttrs());
  CHECK(record.record->GetAttr(Attr::Type::Data).empty());
}

TEMPLATE_TEST_CASE_SIG(
    "An $ATTRIBUTE_LIST entry naming a genuine extension record still "
    "imports its attribute, sequence numbers included",
    "[attr-list][regression]", ((Cache::Strategy S), S),
    Cache::Strategy::NoCache, Cache::Strategy::FullCache) {
  const ParsedRecord<S> record =
      OpenWithExtensionLink<S>(NtfsBrowserTests::genuine_extension_link);
  REQUIRE(record.record->ParseAttrs());

  const auto& data = record.record->GetAttr(Attr::Type::Data);
  REQUIRE(data.size() == 1);
  CHECK(ReadFirstBytes<S>(*data.front(), expected_lifetime_content.size()) ==
        expected_lifetime_content);
}

TEMPLATE_TEST_CASE_SIG(
    "A raw attribute callback installed on a record can discard an "
    "attribute imported from an extension record",
    "[attr-list][regression]", ((Cache::Strategy S), S),
    Cache::Strategy::NoCache, Cache::Strategy::FullCache) {
  const ParsedRecord<S> record =
      OpenUnparsed<S>(NtfsBrowserTests::BuildFakeNtfsImageWithExtensionLink(
          NtfsBrowserTests::genuine_extension_link));

  REQUIRE(record.record->InstallAttrRawCB(
      Attr::Type::Data, [](const NtfsBrowser::Attr::HeaderCommon&,
                           bool& discard) { discard = true; }));
  REQUIRE(record.record->ParseFileRecord(
      NtfsBrowserTests::attr_list_lifetime_base_idx));
  REQUIRE(record.record->ParseAttrs());

  CHECK(record.record->GetAttr(Attr::Type::Data).empty());
}

TEMPLATE_TEST_CASE_SIG(
    "A truncated resident $ATTRIBUTE_LIST logs its short read, in both modes",
    "[attr-list][regression]", ((Cache::Strategy S), S),
    Cache::Strategy::NoCache, Cache::Strategy::FullCache) {
  const std::string short_read =
      "Attribute List: ReadData returned 10 bytes, expected 26 - stopping";

  SECTION("strict") {
    const ListParse outcome = ParseListLog<S>(
        NtfsBrowserTests::BuildFakeNtfsImageWithAttributeListShortRead(),
        static_cast<ULONGLONG>(Mft::Idx::Root), false, Attr::Mask::All);
    CheckListParse(outcome, false, short_read);
    CHECK_THAT(outcome.log, Catch::Matchers::ContainsSubstring(
                                std::string(truncated_message)));
  }

  SECTION("recovering") {
    const ListParse outcome = ParseListLog<S>(
        NtfsBrowserTests::BuildFakeNtfsImageWithAttributeListShortRead(),
        static_cast<ULONGLONG>(Mft::Idx::Root), true, Attr::Mask::All);
    CheckListParse(outcome, true, short_read);
    CHECK(outcome.log.find(truncated_message) == std::string::npos);
  }
}

TEMPLATE_TEST_CASE_SIG(
    "A $ATTRIBUTE_LIST entry with a record_size below the entry header logs "
    "it, in both modes",
    "[attr-list][regression]", ((Cache::Strategy S), S),
    Cache::Strategy::NoCache, Cache::Strategy::FullCache) {
  const std::string too_small =
      "Attribute List: record_size 5 is smaller than the entry header 26 - "
      "stopping";

  SECTION("strict") {
    const ListParse outcome = ParseListLog<S>(
        NtfsBrowserTests::
            BuildFakeNtfsImageWithAttributeListRecordSizeTooSmall(),
        NtfsBrowserTests::attribute_list_dir_idx, false, Attr::Mask::IndexRoot);
    CheckListParse(outcome, false, too_small);
    CHECK_THAT(outcome.log, Catch::Matchers::ContainsSubstring(
                                std::string(truncated_message)));
  }

  SECTION("recovering") {
    CheckListParse(
        ParseListLog<S>(
            NtfsBrowserTests::
                BuildFakeNtfsImageWithAttributeListRecordSizeTooSmall(),
            NtfsBrowserTests::attribute_list_dir_idx, true,
            Attr::Mask::IndexRoot),
        true, too_small);
  }
}

TEMPLATE_TEST_CASE_SIG(
    "A $ATTRIBUTE_LIST whose walk ends past its declared size logs both "
    "offsets, in both modes",
    "[attr-list][regression]", ((Cache::Strategy S), S),
    Cache::Strategy::NoCache, Cache::Strategy::FullCache) {
  const std::string overshoot =
      "Attribute List ended at offset 36 instead of its declared size 32.";

  SECTION("strict") {
    const ListParse outcome = ParseListLog<S>(
        NtfsBrowserTests::BuildFakeNtfsImageWithAttributeListOffsetMismatch(),
        NtfsBrowserTests::attribute_list_dir_idx, false, Attr::Mask::IndexRoot);
    CheckListParse(outcome, false, overshoot);
    CHECK_THAT(outcome.log, Catch::Matchers::ContainsSubstring(
                                std::string(truncated_message)));
  }

  SECTION("recovering") {
    CheckListParse(
        ParseListLog<S>(NtfsBrowserTests::
                            BuildFakeNtfsImageWithAttributeListOffsetMismatch(),
                        NtfsBrowserTests::attribute_list_dir_idx, true,
                        Attr::Mask::IndexRoot),
        true, overshoot);
  }
}

TEMPLATE_TEST_CASE_SIG(
    "An $ATTRIBUTE_LIST entry of an invalid attribute type rejects the record "
    "in both modes",
    "[attr-list][regression]", ((Cache::Strategy S), S),
    Cache::Strategy::NoCache, Cache::Strategy::FullCache) {
  const std::string message =
      "Attribute List parse error (al_record.attr_type).";

  SECTION("strict") {
    CheckListParse(
        ParseListLog<S>(
            NtfsBrowserTests::BuildFakeNtfsImageWithAttributeListInvalidType(),
            NtfsBrowserTests::attribute_list_dir_idx, false,
            Attr::Mask::IndexRoot),
        false, message);
  }

  SECTION("recovering") {
    CheckListParse(
        ParseListLog<S>(
            NtfsBrowserTests::BuildFakeNtfsImageWithAttributeListInvalidType(),
            NtfsBrowserTests::attribute_list_dir_idx, true,
            Attr::Mask::IndexRoot),
        false, message);
  }
}

TEMPLATE_TEST_CASE_SIG(
    "A repeated $ATTRIBUTE_LIST entry for one record and type is resolved "
    "once, in both modes",
    "[attr-list][regression]", ((Cache::Strategy S), S),
    Cache::Strategy::NoCache, Cache::Strategy::FullCache) {
  const std::string skipped = "already resolved in this chain, skipping";

  SECTION("strict") {
    const ParsedRecord<S> dir = OpenListRecord<S>(
        NtfsBrowserTests::BuildFakeNtfsImageWithAttributeListDuplicateEntry(),
        NtfsBrowserTests::attribute_list_dir_idx, Attr::Mask::IndexRoot);
    (void)NtfsBrowserTests::TakeCapturedLog();

    CHECK(dir.record->ParseAttrs());
    CHECK(dir.record->GetAttr(Attr::Type::IndexRoot).size() == 1);
    CHECK_THAT(NtfsBrowserTests::TakeCapturedLog(),
               Catch::Matchers::ContainsSubstring(skipped));
  }

  SECTION("recovering") {
    CheckListParse(
        ParseListLog<S>(NtfsBrowserTests::
                            BuildFakeNtfsImageWithAttributeListDuplicateEntry(),
                        NtfsBrowserTests::attribute_list_dir_idx, true,
                        Attr::Mask::IndexRoot),
        true, skipped);
  }
}

TEMPLATE_TEST_CASE_SIG(
    "An $ATTRIBUTE_LIST entry with a zero record_size rejects the record in "
    "both modes, with its endless-loop message",
    "[attr-list][regression]", ((Cache::Strategy S), S),
    Cache::Strategy::NoCache, Cache::Strategy::FullCache) {
  const std::string message =
      "Attribute List with zero record size has endless loop.";

  SECTION("strict") {
    CheckListParse(
        ParseListLog<S>(
            NtfsBrowserTests::
                BuildFakeNtfsImageWithAttributeListImportThenZeroRecordSize(),
            NtfsBrowserTests::attr_list_lifetime_base_idx, false,
            Attr::Mask::All),
        false, message);
  }

  SECTION("recovering") {
    CheckListParse(
        ParseListLog<S>(
            NtfsBrowserTests::
                BuildFakeNtfsImageWithAttributeListImportThenZeroRecordSize(),
            NtfsBrowserTests::attr_list_lifetime_base_idx, true,
            Attr::Mask::All),
        false, message);
  }
}
