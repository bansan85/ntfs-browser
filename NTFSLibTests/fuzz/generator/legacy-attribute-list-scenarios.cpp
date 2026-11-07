#include <ntfs-browser/win-types.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <optional>
#include <vector>

#include <ntfs-browser/namespace.h>
#include <ntfs-browser/test/fake-image/fake-ntfs-image.h>
#include <ntfs-browser/tool/fuzzgen/legacy-attribute-list-scenarios.h>
#include <ntfs-browser/tool/fuzzgen/legacy-fixtures.h>
#include <ntfs-browser/tool/fuzzgen/record-edit.h>
#include <ntfs-browser/tool/fuzzgen/scenario-registry.h>

#include "data/attribute-list.h"
#include "data/file-record-header.h"
#include "data/header-non-resident.h"
#include "data/header-resident.h"
#include "data/index-entry-flag.h"
#include "data/index-entry.h"
#include "data/index-root.h"

namespace NTFS_BROWSER_NAMESPACE::Tool::FuzzGen {

namespace {

namespace FakeImage = Test::FakeImage;

using Image = std::vector<BYTE>;

// The records the volume open reads before it reaches the root: the $MFT and
// the $Volume. The root holds the list.
constexpr ULONGLONG mft_record = 0;
constexpr ULONGLONG volume_record = 3;
constexpr ULONGLONG root_record = 5;

// The records an entry may name as the holder of its attribute.
constexpr ULONGLONG holder_record_6 = 6;
constexpr ULONGLONG holder_record_7 = 7;

// Attribute types of the hand-made attributes.
constexpr DWORD type_attribute_list = 0x20;
constexpr DWORD type_data = 0x80;
constexpr DWORD type_unknown = 0xC0;

// A type code that is not an attribute type: its low nibble is set.
constexpr DWORD invalid_attribute_type = 0x15;

// The sizes of one entry in the list: the 26 bytes of its header, and the 40
// the first builders gave it when they padded it to 8 bytes.
constexpr WORD packed_entry_size = 26;
constexpr WORD padded_entry_size = 40;

// The sizes a damaged entry declares: none, less than its header, and four
// bytes more than the list holds.
constexpr WORD entry_size_zero = 0;
constexpr WORD entry_size_below_header = 5;
constexpr WORD entry_size_past_list = 30;

// Size an attribute declares when it is shorter than its own fixed header,
// the first byte of an end marker the fuzzer left cut, and the length of the
// fixed header of a resident attribute.
constexpr DWORD total_size_below_header = 0x11;
constexpr DWORD end_marker_first_byte = 0xFF;
constexpr std::size_t resident_header_bytes = 24;

// The base image. The list replaces the attributes of the root.
Image Base() { return FakeImage::BuildFakeNtfsImage(); }

// One entry of a list: the type of the attribute it describes and the record
// that holds it. The size is the stride of the list unless size says
// otherwise.
struct Entry {
  DWORD type;
  ULONGLONG holder;
  std::optional<WORD> size;
};

// Puts into record idx a list whose entries take stride bytes each.
void PutList(Image& image, ULONGLONG idx, WORD stride,
             const std::vector<Entry>& entries) {
  RecordEdit::ClearAttributes(image, idx);
  const std::size_t attr =
      RecordEdit::AppendResident(image, idx, type_attribute_list, 0,
                                 std::vector<BYTE>(entries.size() * stride, 0));
  const std::size_t body = RecordEdit::ValueOffset(image, attr);
  for (std::size_t i = 0; i < entries.size(); ++i) {
    RecordEdit::Edit<Data::AttributeList>(
        image, body + (i * stride), [&](auto& entry) {
          entry.attr_type = static_cast<Attr::Type>(entries[i].type);
          entry.record_size = entries[i].size.value_or(stride);
          entry.base_ref.segment_number = entries[i].holder;
        });
  }
}

// Makes each of records name base as its base record.
void SetBase(Image& image, std::initializer_list<ULONGLONG> records,
             ULONGLONG base) {
  for (const ULONGLONG idx : records) {
    RecordEdit::Edit<Data::FileRecordHeader>(
        image, RecordEdit::RecordOffset(idx),
        [&](auto& header) { header.ref_to_base = base; });
  }
}

// Gives record idx the form the old builders wrote for an extension record:
// a record in use whose only attribute is of unknown type, cut to the size of
// its header and followed by a cut end marker. The record names base as its
// base record.
void MakeUnparsableExtension(Image& image, ULONGLONG idx, ULONGLONG base) {
  RecordEdit::CopyRecord(image, volume_record, idx);
  RecordEdit::ClearAttributes(image, idx);
  SetBase(image, {idx}, base);
  const std::size_t attr =
      RecordEdit::AppendResident(image, idx, type_unknown, 0, {});
  RecordEdit::SetTotalSize(image, attr, total_size_below_header);
  RecordEdit::Edit<Data::HeaderResident>(
      image, attr, [](auto& header) { header.attr_offset = 0; });
  RecordEdit::StoreDword(image, attr + resident_header_bytes,
                         end_marker_first_byte);
}

// The index root of an empty directory, and its size in the packed form: the
// 16 bytes of the root header, the 16 of the index header, and one end entry
// of 16 bytes.
constexpr WORD empty_root_value_size = 48;
constexpr DWORD end_entry_size = 16;

// Block size of the index, in bytes and in clusters.
constexpr DWORD index_block_bytes = 0x400;
constexpr BYTE index_block_clusters = 1;

// Gives record idx the form of an extension that holds an empty $INDEX_ROOT
// of file names, packed, and an empty non-resident $INDEX_ALLOCATION. The
// record names no base.
void MakeEmptyIndexExtension(Image& image, ULONGLONG idx) {
  RecordEdit::CopyRecord(image, volume_record, idx);
  RecordEdit::ClearAttributes(image, idx);
  SetBase(image, {idx}, 0);
  const std::size_t root =
      RecordEdit::AppendResident(image, idx, RecordEdit::type_index_root, 0,
                                 std::vector<BYTE>(empty_root_value_size, 0));
  const std::size_t value = RecordEdit::ValueOffset(image, root);
  RecordEdit::Edit<Data::IndexRoot>(image, value, [](auto& header) {
    header.attr_type = static_cast<Attr::Type>(RecordEdit::type_file_name);
    header.ib_size = index_block_bytes;
    header.clusters_per_ib = index_block_clusters;
    header.entry_offset = end_entry_size;
    header.total_entry_size = end_entry_size;
    header.alloc_entry_size = end_entry_size;
  });
  RecordEdit::Edit<Data::IndexEntry>(
      image, value + sizeof(Data::IndexRoot), [](auto& entry) {
        entry.size = static_cast<WORD>(end_entry_size);
        entry.flags = Data::IndexEntryFlag::Last;
      });
  RecordEdit::AppendNonResident(image, idx, RecordEdit::type_index_allocation,
                                std::vector<BYTE>(8, 0), 0, 0, 0);
}

// The directory whose list names an index root and an index allocation in an
// extension record, placed the way the first builders did: the list in the
// root, the extension in record 6. A third entry names the index root again.
Image MultiType() {
  Image image =
      FakeImage::BuildFakeNtfsImageWithMultiTypeAttributeListDirectory();
  MakeEmptyIndexExtension(image, holder_record_6);
  PutList(
      image, root_record, padded_entry_size,
      {{.type = RecordEdit::type_index_root, .holder = holder_record_6},
       {.type = RecordEdit::type_index_allocation, .holder = holder_record_6},
       {.type = RecordEdit::type_index_root, .holder = holder_record_6}});
  return image;
}

// The list in the root names the index root in record 7, with an entry size
// that overshoots the list.
Image WithIndexRootExtension() {
  Image image = FakeImage::BuildFakeNtfsImageWithAttributeListOffsetMismatch();
  const Image donor = image;
  PutList(image, root_record, packed_entry_size,
          {{.type = RecordEdit::type_index_root,
            .holder = holder_record_7,
            .size = entry_size_past_list}});
  RecordEdit::CopyRecordFrom(image, donor, FakeImage::index_extension_idx,
                             holder_record_7);
  SetBase(image, {holder_record_7}, 0);
  return image;
}

// The cycle of two lists that name each other, in a volume where every record
// that the open reads names the root as its base.
Image Cycle() {
  Image image = FakeImage::BuildFakeNtfsImageWithAttributeListCycle();
  PutList(image, root_record, padded_entry_size,
          {{.type = type_attribute_list, .holder = holder_record_6}});
  PutList(image, holder_record_6, padded_entry_size,
          {{.type = type_attribute_list, .holder = root_record}});
  SetBase(image, {mft_record, volume_record, root_record}, root_record);
  return image;
}

// The $MFT of the first fragmented volume is 24 clusters from cluster 24 on.
// Record 16 is the forged one, the first record that lies beyond the first
// cluster of the $MFT.
constexpr ULONGLONG mft_clusters = 24;
constexpr ULONGLONG mft_first_cluster = 24;
constexpr ULONGLONG forged_record = 16;
constexpr ULONGLONG mft_size_bytes = mft_clusters * 1024;

// The size of the header of the $DATA of the $MFT, to which the run list adds.
constexpr DWORD mft_attribute_header_bytes = 0x40;

// The run list that maps the 24 clusters, and the last VCN of the stream.
const std::vector<BYTE> mft_runs = {0x11, mft_clusters, mft_first_cluster, 0};
constexpr ULONGLONG mft_last_vcn = mft_clusters - 1;

// Other $DATA of the $MFT: the last VCN of a stream of 20 clusters, a run
// header that no run can have (15 bytes of length and of offset), and a run of
// 20 clusters whose offset of 8 bytes holds a cluster number so high that its
// byte address overflows.
constexpr ULONGLONG short_mft_last_vcn = 0x13;
const std::vector<BYTE> undecodable_runs = {0xFF, 0x14, 0,    0, 0, 0,
                                            0,    0,    0x20, 0, 0};
const std::vector<BYTE> overflowing_runs = {0x81, 0x14, 0,    0, 0, 0,
                                            0,    0,    0x20, 0, 0};

// The forged record still declares the first attribute where a record puts
// it, and that it is in use.
constexpr WORD first_attribute_offset = 0x30;
constexpr WORD flag_in_use = 0x01;

// Words of the update sequence array of a record of one sector: the sequence
// number and one saved word.
constexpr WORD one_sector_array_size = 2;

// The root holds an $ATTRIBUTE_LIST that sends the walk to the forged record
// of a fragmented $MFT, whose update sequence array starts past the record.
Image FragmentedMft(const std::vector<BYTE>& runs, ULONGLONG last_vcn) {
  Image image = FakeImage::BuildFakeNtfsImageWithFragmentedMftInvalidRecord();
  const std::size_t forged_from =
      (static_cast<std::size_t>(FakeImage::fragmented_mft_data_run_lcn) +
       forged_record) *
      FakeImage::fake_cluster_size;
  const std::size_t forged_to =
      (static_cast<std::size_t>(mft_first_cluster) + forged_record) *
      FakeImage::fake_cluster_size;
  image.resize(
      std::max(image.size(), forged_to + FakeImage::fake_file_record_size));
  std::copy_n(image.begin() + static_cast<std::ptrdiff_t>(forged_from),
              FakeImage::fake_file_record_size,
              image.begin() + static_cast<std::ptrdiff_t>(forged_to));
  std::fill_n(image.begin() + static_cast<std::ptrdiff_t>(forged_from),
              FakeImage::fake_file_record_size, BYTE{0});
  const std::size_t data =
      RecordEdit::Attribute(image, mft_record, RecordEdit::type_data);
  RecordEdit::SetRunList(image, data, runs);
  RecordEdit::Edit<Data::HeaderNonResident>(image, data, [&](auto& header) {
    header.last_vcn = last_vcn;
    header.alloc_size = mft_size_bytes;
    header.real_size = mft_size_bytes;
    header.ini_size = mft_size_bytes;
  });
  RecordEdit::ShrinkLastAttribute(
      image, data,
      static_cast<DWORD>(mft_attribute_header_bytes + runs.size()));
  PutList(
      image, root_record, padded_entry_size,
      {{.type = RecordEdit::type_index_allocation, .holder = forged_record}});
  return image;
}

// Gives the forged record the form it had: a record in use, with the array of
// a one-sector record. It runs after the old form is applied, which leaves a
// record alone while its first attribute offset is zero.
void SealForgedRecord(Image& image) {
  RecordEdit::Edit<Data::FileRecordHeader>(
      image, (mft_first_cluster + forged_record) * FakeImage::fake_cluster_size,
      [](auto& header) {
        header.size_of_us = one_sector_array_size;
        header.offset_of_attr = first_attribute_offset;
        header.flags = static_cast<Data::FileRecordFlag>(flag_in_use);
      });
}

// The last VCN that the $DATA of the $MFT declares, and the record its list
// names as the holder of the next extent. A byte offset cannot reach that VCN.
constexpr ULONGLONG overflowing_last_vcn = 0x3FFFFFFFFFFFFF;
constexpr ULONGLONG next_extent_record = 16;
constexpr ULONGLONG one_cluster_bytes = 0x400;

// The $MFT lists one extent in a record that follows, and its own $DATA
// claims a last VCN that no byte offset can hold.
void MakeMftWithHugeLastVcn(Image& image) {
  PutList(image, mft_record, packed_entry_size,
          {{.type = type_data, .holder = next_extent_record}});
  RecordEdit::AppendNonResident(image, mft_record, type_data,
                                std::vector<BYTE>(8, 0), overflowing_last_vcn,
                                one_cluster_bytes, one_cluster_bytes);
}

// The first record that holds an extension of the split list, and the number
// of extensions: four, one VCN 0 each, of growing size.
constexpr ULONGLONG first_split_extension = 6;
constexpr ULONGLONG split_extensions = 4;

// The directory whose index allocation is split over four extension records
// that each start at VCN 0, with the list in the root and the extensions in
// the records 6 to 9. Every record that the open reads names the root as its
// base.
Image SplitIndexAllocation() {
  Image image =
      FakeImage::BuildFakeNtfsImageWithFragmentedAttributeListDirectory();
  const Image donor = image;
  const std::array<ULONGLONG, split_extensions> from = {
      FakeImage::uaf_extension_idx0, FakeImage::uaf_extension_idx1,
      FakeImage::uaf_extension_idx2, FakeImage::uaf_extension_idx3};
  std::vector<Entry> entries;
  std::vector<ULONGLONG> records = {mft_record, volume_record};
  for (std::size_t i = 0; i < from.size(); ++i) {
    const ULONGLONG to = first_split_extension + i;
    RecordEdit::CopyRecordFrom(image, donor, from[i], to);
    entries.push_back(
        {.type = RecordEdit::type_index_allocation, .holder = to});
    records.push_back(to);
  }
  PutList(image, root_record, padded_entry_size, entries);
  for (const ULONGLONG idx : records) {
    SetBase(image, {idx}, root_record);
  }
  return image;
}

const std::vector<LegacyFixtures::Damaged>& Inputs() {
  static const std::vector<LegacyFixtures::Damaged> inputs = {
      {.name = "attribute_list_zero_record_size",
       .base = Base,
       .damage =
           [](Image& image) {
             PutList(image, root_record, packed_entry_size,
                     {{.type = type_data,
                       .holder = root_record,
                       .size = entry_size_zero}});
           }},
      {.name = "attribute_list_record_size_too_small_on_root",
       .base = Base,
       .damage =
           [](Image& image) {
             PutList(image, root_record, packed_entry_size,
                     {{.type = type_data,
                       .holder = root_record,
                       .size = entry_size_below_header}});
           }},
      {.name = "attribute_list_invalid_attr_type",
       .base = Base,
       .damage =
           [](Image& image) {
             PutList(image, root_record, packed_entry_size,
                     {{.type = invalid_attribute_type, .holder = root_record}});
           }},
      {.name = "attribute_list_extension_foreign_record",
       .base = Base,
       .damage =
           [](Image& image) {
             PutList(image, root_record, packed_entry_size,
                     {{.type = type_data, .holder = holder_record_6}});
             MakeUnparsableExtension(image, holder_record_6, 0);
           }},
      {.name = "attribute_list_extension_parse_attrs_fail",
       .base = Base,
       .damage =
           [](Image& image) {
             PutList(image, root_record, packed_entry_size,
                     {{.type = type_data, .holder = holder_record_6}});
             MakeUnparsableExtension(image, holder_record_6, root_record);
             SetBase(image, {mft_record, volume_record}, root_record);
           }},
      {.name = "attribute_list_offset_mismatch_on_root",
       .base = WithIndexRootExtension},
      {.name = "attribute_list_extension_record_cycle",
       .base = Cycle,
       .early = true},
      {.name = "attribute_list_multi_type_same_record",
       .base = MultiType,
       .early = true},
      {.name = "full_cache_attribute_list_record_growth",
       .base = SplitIndexAllocation,
       .early = true},
      {.name = "mft_data_last_vcn_overflow",
       .base = Base,
       .damage = MakeMftWithHugeLastVcn},
      {.name = "fragmented_record_header_factory_throw",
       .base = [] { return FragmentedMft(mft_runs, mft_last_vcn); },
       .damage = SealForgedRecord,
       .early = true},
      {.name = "data_run_decode_error_size_byte",
       .base =
           [] { return FragmentedMft(undecodable_runs, short_mft_last_vcn); },
       .damage = SealForgedRecord,
       .early = true},
      {.name = "mft_data_run_cluster_lcn_narrowing_error",
       .base =
           [] { return FragmentedMft(overflowing_runs, short_mft_last_vcn); },
       .damage = SealForgedRecord,
       .early = true},
  };
  return inputs;
}

}  // namespace

void LegacyAttributeListScenarios::Register(ScenarioRegistry& registry) {
  LegacyFixtures::RegisterDamaged(registry, Inputs());
}

}  // namespace NTFS_BROWSER_NAMESPACE::Tool::FuzzGen
