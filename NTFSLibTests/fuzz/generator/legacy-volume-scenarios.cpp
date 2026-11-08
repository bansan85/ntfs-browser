#include <ntfs-browser/win-types.h>

#include <algorithm>
#include <cstddef>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <ntfs-browser/namespace.h>
#include <ntfs-browser/test/fake-image/fake-ntfs-image.h>
#include <ntfs-browser/tool/fuzzgen/hex-bytes.h>
#include <ntfs-browser/tool/fuzzgen/legacy-fixtures.h>
#include <ntfs-browser/tool/fuzzgen/legacy-record.h>
#include <ntfs-browser/tool/fuzzgen/legacy-volume-scenarios.h>
#include <ntfs-browser/tool/fuzzgen/scenario-registry.h>

namespace NTFS_BROWSER_NAMESPACE::Tool::FuzzGen {

namespace {

namespace FakeImage = Test::FakeImage;

using Image = std::vector<BYTE>;
using Attribute = LegacyRecord::Attribute;
using Run = LegacyRecord::Run;

// The $MFT of the hand-made volume: 16 clusters, one file record in each.
constexpr ULONGLONG mft_clusters = 16;

// The record of the file with the damaged stream, and the one that extends it.
constexpr ULONGLONG file_record = 10;
constexpr ULONGLONG extension_record = 11;

// The reference of the file record, with its sequence number 1, as the
// extension record names its base.
constexpr ULONGLONG file_reference = (1ULL << 48U) | file_record;

// The bytes of a cluster of data: seed plus 7 per byte, so that a misplaced
// slice does not pass for the right one. The pattern repeats every 256 bytes.
constexpr BYTE pattern_step = 7;

// A hole of the run list.
Run Hole(ULONGLONG clusters) {
  return {.lcn = std::nullopt, .clusters = clusters};
}

// Fills clusters clusters from the cluster lcn on with the pattern of seed.
void FillClusters(Image& image, ULONGLONG lcn, ULONGLONG clusters, BYTE seed) {
  const std::size_t first =
      static_cast<std::size_t>(lcn) * FakeImage::fake_cluster_size;
  const std::size_t bytes =
      static_cast<std::size_t>(clusters) * FakeImage::fake_cluster_size;
  image.resize(std::max(image.size(), first + bytes));
  for (std::size_t i = 0; i < bytes; ++i) {
    image[first + i] = static_cast<BYTE>(seed + (pattern_step * i));
  }
}

// The size of an entry of an $ATTRIBUTE_LIST that names no stream.
constexpr std::size_t entry_bytes = 0x20;

// One entry of an $ATTRIBUTE_LIST: a $DATA, from vcn on, held by record. The
// entry declares size bytes, and has as many.
std::vector<BYTE> DataEntry(ULONGLONG vcn, ULONGLONG record,
                            std::size_t size = entry_bytes) {
  constexpr BYTE name_offset = 0x1A;
  std::vector<BYTE> entry(size, 0);
  entry[0] = LegacyRecord::type_data;
  entry[4] = static_cast<BYTE>(size);
  entry[7] = name_offset;
  for (std::size_t i = 0; i < sizeof(ULONGLONG); ++i) {
    entry[8 + i] = static_cast<BYTE>(vcn >> (8 * i));
    entry[16 + i] = static_cast<BYTE>(record >> (8 * i));
  }
  return entry;
}

// Writes bytes at the start of the cluster lcn.
void PutBytes(Image& image, ULONGLONG lcn, const std::vector<BYTE>& bytes) {
  const std::size_t first =
      static_cast<std::size_t>(lcn) * FakeImage::fake_cluster_size;
  image.resize(std::max(image.size(), first + bytes.size()));
  std::ranges::copy(bytes, image.begin() + static_cast<std::ptrdiff_t>(first));
}

Image HandMade() {
  Image image = FakeImage::BuildFakeNtfsImage();
  LegacyRecord::PutMft(image, mft_clusters);
  return image;
}

// A compressed file whose first two clusters are mapped, then a hole of six
// clusters, then an extension record that maps VCN 8 and 9. The unit at VCN 2
// has no cluster of its own.
Image SplitRuns() {
  Image image = HandMade();
  std::vector<BYTE> list = DataEntry(0, file_record);
  const std::vector<BYTE> second = DataEntry(8, extension_record);
  list.insert(list.end(), second.begin(), second.end());
  constexpr ULONGLONG file_clusters = 8;
  constexpr ULONGLONG file_bytes = file_clusters * FakeImage::fake_cluster_size;
  LegacyRecord::Put(image, file_record,
                    {.attributes = {{.type = LegacyRecord::type_attribute_list,
                                     .value = list},
                                    {.type = LegacyRecord::type_data,
                                     .flags = LegacyRecord::flag_compressed,
                                     .non_resident = true,
                                     .last_vcn = file_clusters - 1,
                                     .compression_unit = 1,
                                     .allocated_size = file_bytes,
                                     .real_size = file_bytes,
                                     .initialized_size = file_bytes,
                                     .runs = {{.lcn = 46, .clusters = 2}}}}});
  constexpr ULONGLONG extension_clusters = 2;
  LegacyRecord::Put(
      image, extension_record,
      {.base_reference = file_reference,
       .attributes = {
           {.type = LegacyRecord::type_data,
            .flags = LegacyRecord::flag_compressed,
            .non_resident = true,
            .start_vcn = file_clusters,
            .last_vcn = file_clusters + extension_clusters - 1,
            .compression_unit = 1,
            .allocated_size = extension_clusters * FakeImage::fake_cluster_size,
            .compressed_size = 0,
            .runs = {{.lcn = 48, .clusters = 2}}}}});
  FillClusters(image, 46, 2, 0x31);
  return image;
}

// A bitmap of six clusters of which three are mapped: 2 clusters, a hole of 2,
// one cluster, a hole of 1.
Image SparseBitmap() {
  Image image = HandMade();
  constexpr ULONGLONG bitmap_clusters = 6;
  constexpr ULONGLONG bitmap_bytes =
      bitmap_clusters * FakeImage::fake_cluster_size;
  LegacyRecord::Put(image, file_record,
                    {.attributes = {{.type = LegacyRecord::type_bitmap,
                                     .non_resident = true,
                                     .last_vcn = bitmap_clusters - 1,
                                     .allocated_size = bitmap_bytes,
                                     .real_size = bitmap_bytes,
                                     .initialized_size = bitmap_bytes,
                                     .runs = {{.lcn = 40, .clusters = 2},
                                              Hole(2),
                                              {.lcn = 44, .clusters = 1},
                                              Hole(1)}}}});
  FillClusters(image, 40, 2, 0x11);
  FillClusters(image, 44, 2, 0x55);
  return image;
}

// A non-resident $ATTRIBUTE_LIST of 48 bytes whose only entry declares 64.
Image ListEntryPastEnd() {
  Image image = HandMade();
  constexpr ULONGLONG list_cluster = 47;
  constexpr ULONGLONG list_real_size = 0x30;
  constexpr std::size_t entry_past_end = 0x40;
  LegacyRecord::Put(
      image, file_record,
      {.attributes = {{.type = LegacyRecord::type_attribute_list,
                       .non_resident = true,
                       .allocated_size = FakeImage::fake_cluster_size,
                       .real_size = list_real_size,
                       .initialized_size = list_real_size,
                       .runs = {{.lcn = list_cluster, .clusters = 1}}}}});
  PutBytes(image, list_cluster, DataEntry(0, file_record, entry_past_end));
  return image;
}

// An encrypted stream of one cluster, and an $EFS stream that holds no byte.
Image ZeroSizeEfs() {
  Image image = HandMade();
  constexpr ULONGLONG stream_cluster = 44;
  constexpr ULONGLONG cluster_bytes = FakeImage::fake_cluster_size;
  LegacyRecord::Put(
      image, file_record,
      {.attributes = {{.type = LegacyRecord::type_data,
                       .flags = LegacyRecord::flag_encrypted,
                       .non_resident = true,
                       .allocated_size = cluster_bytes,
                       .real_size = cluster_bytes,
                       .initialized_size = cluster_bytes,
                       .runs = {{.lcn = stream_cluster, .clusters = 1}}},
                      {.type = LegacyRecord::type_logged_utility_stream,
                       .name = L"$EFS",
                       .non_resident = true,
                       .allocated_size = cluster_bytes,
                       .raw_runs = {0}}}});
  FillClusters(image, stream_cluster, 2, 0x21);
  return image;
}

// The $MFT has a second $DATA that starts at VCN 24 and ends at a VCN near
// 2^54: its extent does not fit in a byte count.
Image MergedSpanOverflow() {
  Image image = FakeImage::BuildFakeNtfsImage();
  constexpr ULONGLONG first_clusters = 24;
  constexpr ULONGLONG first_bytes =
      first_clusters * FakeImage::fake_cluster_size;
  constexpr ULONGLONG second_run_cluster = 20;
  constexpr ULONGLONG huge_last_vcn = (1ULL << 54U) + 100;
  LegacyRecord::Put(
      image, 0,
      {.attributes = {{.type = LegacyRecord::type_data,
                       .non_resident = true,
                       .last_vcn = first_clusters - 1,
                       .allocated_size = first_bytes,
                       .real_size = first_bytes,
                       .initialized_size = first_bytes,
                       .runs = {{.lcn = 1, .clusters = mft_clusters},
                                {.lcn = second_run_cluster,
                                 .clusters = first_clusters - mft_clusters}}},
                      {.type = LegacyRecord::type_data,
                       .non_resident = true,
                       .start_vcn = first_clusters,
                       .last_vcn = huge_last_vcn,
                       .raw_runs = {0}}}});
  return image;
}

// A chunk of LZNT1 that writes eight literals and then a back reference that
// runs past the unit.
constexpr const char* overrun_chunk =
    "0D B0 00 41 42 43 44 45 46 47 48 03 EF 0F 07";

// The $Volume record holds two compressed streams of one unit of four clusters:
// the unnamed one claims 6 KiB in 4 KiB, the named one maps three clusters and
// a hole.
Image CompressedOverrun() {
  Image image = HandMade();
  constexpr ULONGLONG unit_clusters = 4;
  constexpr ULONGLONG unit_bytes = unit_clusters * FakeImage::fake_cluster_size;
  constexpr ULONGLONG claimed_bytes = 0x1800;
  constexpr ULONGLONG stored_bytes = 3 * FakeImage::fake_cluster_size;
  constexpr BYTE unit_exponent = 2;
  constexpr ULONGLONG volume_record = 3;
  LegacyRecord::Put(
      image, volume_record,
      {.attributes = {
           {.type = LegacyRecord::type_volume_information,
            .name_offset = 0,
            .value = HexBytes::Decode("00 00 00 00 00 00 00 00 03 01 00 00")},
           {.type = LegacyRecord::type_data,
            .flags = LegacyRecord::flag_compressed,
            .non_resident = true,
            .last_vcn = unit_clusters - 1,
            .compression_unit = unit_exponent,
            .allocated_size = unit_bytes,
            .real_size = claimed_bytes,
            .initialized_size = claimed_bytes,
            .runs = {{.lcn = 50, .clusters = unit_clusters}}},
           {.type = LegacyRecord::type_data,
            .flags = LegacyRecord::flag_compressed,
            .name = L"$LZ",
            .non_resident = true,
            .last_vcn = unit_clusters - 1,
            .compression_unit = unit_exponent,
            .allocated_size = unit_bytes,
            .real_size = unit_bytes,
            .initialized_size = unit_bytes,
            .compressed_size = stored_bytes,
            .runs = {{.lcn = 54, .clusters = 3}, Hole(1)}}}});
  PutBytes(image, 50, HexBytes::Decode(overrun_chunk));
  PutBytes(image, 54, HexBytes::Decode(overrun_chunk));
  return image;
}

// An extension record of the $MFT, as the fake builders write one: the array
// at the end, the sequence number and the link count 0. The $DATA it holds
// maps one cluster at lcn for the VCNs from vcn to last_vcn.
LegacyRecord::Spec MftExtension(std::vector<Attribute> streams,
                                WORD sequence = 0) {
  return {.sequence = sequence,
          .links = 0,
          .update_sequence_number = 0,
          .attributes = std::move(streams)};
}

Attribute MftExtent(ULONGLONG vcn, ULONGLONG last_vcn, ULONGLONG lcn,
                    std::wstring name = {}) {
  const bool unnamed = name.empty();
  return {.type = LegacyRecord::type_data,
          .name = std::move(name),
          .name_offset = unnamed ? std::optional<WORD>(0) : std::nullopt,
          .non_resident = true,
          .start_vcn = vcn,
          .last_vcn = last_vcn,
          .allocated_size = FakeImage::fake_cluster_size,
          .real_size = FakeImage::fake_cluster_size,
          .initialized_size = FakeImage::fake_cluster_size,
          .runs = {{.lcn = lcn, .clusters = 1}}};
}

// The $MFT lists seven extents of its $DATA in records 6 to 11. Each record
// breaks one rule: a name, a VCN range that ends before it starts, a range
// that another record holds too, a sequence number that the list does not
// know, an entry of length zero.
Image MftExtensionDefects() {
  constexpr ULONGLONG first_extension = 6;
  constexpr ULONGLONG listed_sequence = 3;
  constexpr WORD reused_sequence = 9;
  constexpr WORD extension_array_offset = 0x30;
  constexpr ULONGLONG unnamed_extent_vcn = 32;
  const std::vector<std::pair<ULONGLONG, ULONGLONG>> listed = {
      {0x10, first_extension},
      {0x20, first_extension},
      {0x30, first_extension + 1},
      {0x40, first_extension + 2},
      {0x40, first_extension + 3 | (listed_sequence << 48U)},
      {0x20, first_extension + 4},
      {0x00, first_extension + 5}};
  std::vector<BYTE> list;
  for (const auto& [vcn, record] : listed) {
    const std::vector<BYTE> entry = DataEntry(vcn, record);
    list.insert(list.end(), entry.begin(), entry.end());
  }
  // The last entry declares no length.
  constexpr std::size_t last_entry = 6;
  list.at((last_entry * entry_bytes) + 4) = 0;

  Image image = FakeImage::BuildFakeNtfsImage();
  constexpr ULONGLONG mft_bytes = FakeImage::fake_cluster_size;
  LegacyRecord::Put(image, 0,
                    {.sequence = 0,
                     .links = 0,
                     .array_offset = 0x3FA,
                     .update_sequence_number = 0,
                     .attributes = {{.type = LegacyRecord::type_attribute_list,
                                     .name_offset = 0,
                                     .value = list},
                                    {.type = LegacyRecord::type_data,
                                     .name_offset = 0,
                                     .non_resident = true,
                                     .allocated_size = mft_bytes,
                                     .real_size = mft_bytes,
                                     .initialized_size = mft_bytes}}});
  const auto put = [&](ULONGLONG idx, LegacyRecord::Spec spec) {
    spec.array_offset = extension_array_offset;
    LegacyRecord::Put(image, idx, spec);
  };
  put(first_extension,
      MftExtension({MftExtent(0x10, 0x10, 0x28, L"x"),
                    MftExtent(unnamed_extent_vcn, unnamed_extent_vcn, 0x29)}));
  put(first_extension + 1, MftExtension({MftExtent(0x30, 0x28, 0x2A)}));
  put(first_extension + 2, MftExtension({MftExtent(0x38, 0x38, 0x2B)}));
  put(first_extension + 3,
      MftExtension({MftExtent(0x40, 0x40, 0x2C)}, reused_sequence));
  put(first_extension + 4,
      MftExtension({MftExtent(unnamed_extent_vcn, unnamed_extent_vcn, 0x2D)}));
  // The extents map clusters that hold a record in use with no attribute.
  constexpr ULONGLONG first_mapped_cluster = 0x28;
  constexpr ULONGLONG mapped_clusters = 6;
  const std::vector<BYTE> empty =
      LegacyRecord::Bytes({.sequence = 0,
                           .links = 0,
                           .array_offset = 0x3FA,
                           .update_sequence_number = 0,
                           .used_size = 0,
                           .first_attribute_offset = 0x30,
                           .allocated_size = 0});
  for (ULONGLONG i = 0; i < mapped_clusters; ++i) {
    PutBytes(image, first_mapped_cluster + i, empty);
  }
  return image;
}

}  // namespace

void LegacyVolumeScenarios::Register(ScenarioRegistry& registry) {
  LegacyFixtures::Register(
      registry,
      {
          {.name = "compressed_split_runs_leave_gap",
           .make = SplitRuns,
           .needs = Feature::Decompression},
          {.name = "bitmap_nonres_sparse_runs", .make = SparseBitmap},
          {.name = "attr_list_nonres_entry_past_end", .make = ListEntryPastEnd},
          {.name = "efs_zero_size_stream",
           .make = ZeroSizeEfs,
           .needs = Feature::Efs},
          {.name = "mft_data_merged_span_overflow", .make = MergedSpanOverflow},
          {.name = "mft_attribute_list_extension_defects",
           .make = MftExtensionDefects},
          {.name = "compressed_overrun_and_lznt1_chunk_overflow",
           .make = CompressedOverrun,
           .needs = Feature::Decompression},
      });
}

}  // namespace NTFS_BROWSER_NAMESPACE::Tool::FuzzGen
