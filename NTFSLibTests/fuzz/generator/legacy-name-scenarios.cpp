#include "legacy-name-scenarios.h"

#include <algorithm>
#include <cstddef>
#include <optional>
#include <string>
#include <vector>

#include <ntfs-browser/namespace.h>
#include <ntfs-browser/test/fake-image/fake-ntfs-image.h>
#include <ntfs-browser/tool/fuzzgen/build-features.h>
#include <ntfs-browser/tool/fuzzgen/recorded-stream.h>
#include <ntfs-browser/tool/fuzzgen/scenario-registry.h>

#include "data/header-non-resident.h"
#include "legacy-fixtures.h"
#include "legacy-index.h"
#include "legacy-record.h"
#include "record-edit.h"
#include "upcase-table.h"
#include "win-types.h"

namespace NTFS_BROWSER_NAMESPACE::Tool::FuzzGen {

namespace {

namespace FakeImage = Test::FakeImage;

using Image = std::vector<BYTE>;
using Attribute = LegacyRecord::Attribute;
using AttributeList = std::vector<std::vector<BYTE>>;

// The root directory record, and the $UpCase record.
constexpr ULONGLONG root_record = 5;
constexpr ULONGLONG upcase_record = 10;

// The cluster of the index block of the surrogate volume.
constexpr ULONGLONG index_block_lcn = 30;

// The $UpCase file holds the 65536 words of the table: 128 clusters.
constexpr ULONGLONG upcase_clusters =
    UpcaseTable::file_bytes / FakeImage::fake_cluster_size;

// Where the inputs put the table, behind the records and the index block.
constexpr ULONGLONG table_lcn_near = 200;
constexpr ULONGLONG table_lcn_far = 301;

// The $MFT of these volumes maps 26 clusters, one for each file record. The
// fold input maps 11: the records up to the $UpCase file.
constexpr ULONGLONG mft_clusters = 26;
constexpr ULONGLONG fold_mft_clusters = 11;

// The cluster where every $MFT starts.
constexpr ULONGLONG mft_lcn = 1;

// The record numbers of the six names of the fold input. The name in the
// index block of the other input has the number 0x46.
constexpr ULONGLONG first_name_record = 0x3D;
constexpr ULONGLONG block_name_record = 0x46;

// A high and a low surrogate. Alone, none of them is a character.
constexpr char16_t high_surrogate = 0xD800;
constexpr char16_t low_surrogate = 0xDC00;

// The letter x, the acute e, and the euro sign: units of one, two and three
// bytes in UTF-8.
constexpr char16_t letter_x = 0x0078;
constexpr char16_t e_acute = 0x00E9;
constexpr char16_t euro_sign = 0x20AC;

// The letters that the table folds those two surrogates to.
constexpr WORD capital_a = u'A';
constexpr WORD capital_b = u'B';

// The $MFT record has its update sequence array at the end of the record, with
// the number 0.
constexpr WORD array_at_record_end = 0x3FA;

// The run list of a stream that starts at the last cluster an address of 53
// bits can hold: a length of 128 clusters in one byte, then seven bytes of
// offset.
const std::vector<BYTE> run_at_address_limit = {0x71, 0x80, 0xFF, 0xFF, 0xFF,
                                                0xFF, 0xFF, 0xFF, 0x1F, 0x00};

// The run list of the fold input: 128 clusters at the cluster 200, with the
// length in two bytes.
const std::vector<BYTE> run_with_two_length_bytes = {0x22, 0x80, 0x00,
                                                     0xC8, 0x00, 0x00};

// The volume of the surrogate pair names, with its index allocation made an
// ordinary one: the volume reads it as a list of plain blocks.
Image UncompressedSurrogateNames() {
  Image image = FakeImage::BuildFakeNtfsImageWithSurrogatePairNames();
  RecordEdit::Edit<Data::HeaderNonResident>(
      image,
      RecordEdit::Attribute(image, root_record,
                            RecordEdit::type_index_allocation),
      [](auto& header) {
        header.header.flags = 0;
        header.comp_unit_size = 0;
      });
  return image;
}

// Writes bytes at the start of the cluster lcn.
void PutBytes(Image& image, ULONGLONG lcn, const std::vector<BYTE>& bytes) {
  const std::size_t first =
      static_cast<std::size_t>(lcn) * FakeImage::fake_cluster_size;
  image.resize(std::max(image.size(), first + bytes.size()));
  std::ranges::copy(bytes, image.begin() + static_cast<std::ptrdiff_t>(first));
}

// Replaces the $INDEX_ROOT of the root directory by one with root_value.
void SetRootIndex(Image& image, const std::vector<BYTE>& root_value) {
  AttributeList attributes = RecordEdit::Attributes(image, root_record);
  attributes.at(1) = LegacyRecord::AttributeBytes(
      {.type = RecordEdit::type_index_root, .value = root_value});
  RecordEdit::SetAttributes(image, root_record, attributes);
}

// A $FILE_NAME with no name in the record of the root directory.
Image FileNameOfZeroLength() {
  Image image = UncompressedSurrogateNames();
  AttributeList attributes = RecordEdit::Attributes(image, root_record);
  attributes.insert(
      attributes.begin() + 1,
      LegacyRecord::AttributeBytes({.type = RecordEdit::type_file_name,
                                    .value = LegacyIndex::FileNameValue({})}));
  RecordEdit::SetAttributes(image, root_record, attributes);
  return image;
}

// A last entry that flags a sub-node and has no room for its VCN, in an index
// block that the root points at.
Image SubNodePointerTooSmall() {
  Image image = UncompressedSurrogateNames();
  SetRootIndex(image,
               LegacyIndex::RootValue(LegacyIndex::LastEntry(true, 0), true));
  std::vector<BYTE> entries = LegacyIndex::Entry(block_name_record, u"BB");
  const std::vector<BYTE> last = LegacyIndex::LastEntry(true);
  entries.insert(entries.end(), last.begin(), last.end());
  PutBytes(image, index_block_lcn, LegacyIndex::Block(0, entries));
  return image;
}

// The $MFT record of the hand-made volumes: one run of clusters clusters from
// the cluster of the $MFT on.
void MapRecords(Image& image, ULONGLONG clusters) {
  const ULONGLONG bytes = clusters * FakeImage::fake_cluster_size;
  LegacyRecord::Put(
      image, 0,
      {.sequence = 0,
       .links = 0,
       .array_offset = array_at_record_end,
       .update_sequence_number = 0,
       .attributes = {{.type = LegacyRecord::type_data,
                       .name_offset = 0,
                       .non_resident = true,
                       .last_vcn = clusters - 1,
                       .allocated_size = bytes,
                       .real_size = bytes,
                       .initialized_size = bytes,
                       .runs = {{.lcn = mft_lcn, .clusters = clusters}}}}});
}

// The $UpCase stream: 128 clusters, whose run list the caller sets.
Attribute UpcaseStream() {
  constexpr ULONGLONG bytes = UpcaseTable::file_bytes;
  return {.type = LegacyRecord::type_data,
          .non_resident = true,
          .last_vcn = upcase_clusters - 1,
          .allocated_size = bytes,
          .real_size = bytes,
          .initialized_size = bytes};
}

// The $UpCase record of the inputs about the table.
void PutUpcaseRecord(Image& image, Attribute stream) {
  stream.name_offset = 0;
  LegacyRecord::Put(image, upcase_record,
                    {.sequence = 1,
                     .links = 0,
                     .update_sequence_number = 0,
                     .attributes = {stream}});
}

// The six names of the fold input, in the order of the index.
std::vector<BYTE> FoldNames() {
  const std::vector<std::u16string> names = {u"A",
                                             {high_surrogate, low_surrogate},
                                             u"ABC",
                                             {e_acute, euro_sign},
                                             {high_surrogate, letter_x},
                                             {low_surrogate}};
  std::vector<BYTE> entries;
  ULONGLONG record = first_name_record;
  for (const std::u16string& name : names) {
    const std::vector<BYTE> entry = LegacyIndex::Entry(record++, name);
    entries.insert(entries.end(), entry.begin(), entry.end());
  }
  const std::vector<BYTE> last = LegacyIndex::LastEntry(false);
  entries.insert(entries.end(), last.begin(), last.end());
  return entries;
}

// A $UpCase table that folds the two surrogates to letters, and the names
// that need it.
Image SurrogatesFoldedToLetters() {
  Image image = UncompressedSurrogateNames();
  SetRootIndex(image, LegacyIndex::RootValue(FoldNames(), false));
  LegacyRecord::Put(
      image, 0,
      {.sequence = 0,
       .links = 0,
       .array_offset = array_at_record_end,
       .update_sequence_number = 0,
       .first_attribute_offset = 0x30,
       .allocated_size = 0,
       .attributes = {
           {.type = LegacyRecord::type_data,
            .non_resident = true,
            .last_vcn = fold_mft_clusters - 1,
            .allocated_size = fold_mft_clusters * FakeImage::fake_cluster_size,
            .real_size = fold_mft_clusters * FakeImage::fake_cluster_size,
            .initialized_size =
                fold_mft_clusters * FakeImage::fake_cluster_size,
            .runs = {{.lcn = mft_lcn, .clusters = fold_mft_clusters}}}}});
  Attribute stream = UpcaseStream();
  stream.name_offset = std::nullopt;
  stream.raw_runs = run_with_two_length_bytes;
  LegacyRecord::Put(image, upcase_record,
                    {.update_sequence_number = 0,
                     .next_attribute_id = 1,
                     .record_number = static_cast<DWORD>(upcase_record),
                     .attributes = {stream}});
  std::vector<BYTE> table = UpcaseTable::LettersFolded();
  UpcaseTable::Map(table, high_surrogate, capital_a);
  UpcaseTable::Map(table, low_surrogate, capital_b);
  PutBytes(image, table_lcn_near, table);
  return image;
}

// A run list whose only run starts at the last cluster an address can hold:
// the table cannot be read.
Image RunAtAddressSpaceLimit() {
  Image image = FakeImage::BuildFakeNtfsImageWithSurrogatePairNames();
  MapRecords(image, mft_clusters);
  Attribute stream = UpcaseStream();
  stream.raw_runs = run_at_address_limit;
  PutUpcaseRecord(image, stream);
  return image;
}

// A table of 128 clusters that the library reads in several blocks.
Image TableInManyBlocks() {
  Image image = UncompressedSurrogateNames();
  MapRecords(image, mft_clusters);
  Attribute stream = UpcaseStream();
  stream.runs = {{.lcn = table_lcn_far, .clusters = upcase_clusters}};
  PutUpcaseRecord(image, stream);
  PutBytes(image, table_lcn_far, UpcaseTable::LettersFolded());
  return image;
}

}  // namespace

void LegacyNameScenarios::Register(ScenarioRegistry& registry) {
  LegacyFixtures::Register(
      registry, {{.name = "names_filename_zero_length_name",
                  .make = FileNameOfZeroLength},
                 {.name = "names_index_block_subnode_vcn_too_small",
                  .make = SubNodePointerTooSmall},
                 {.name = "names_upcase_fold_surrogates_and_utf8",
                  .make = SurrogatesFoldedToLetters},
                 {.name = "upcase_run_lcn_at_address_space_limit",
                  .make = RunAtAddressSpaceLimit,
                  .boot = LegacyFixtures::Boot::PatchedTotalSectors,
                  .probe = RecordedStream::Probe::FullCache,
                  .needs = Feature::Decompression},
                 {.name = "upcase_table_read_crosses_64k_blocks",
                  .make = TableInManyBlocks,
                  .boot = LegacyFixtures::Boot::PatchedTotalSectors}});
}

}  // namespace NTFS_BROWSER_NAMESPACE::Tool::FuzzGen
