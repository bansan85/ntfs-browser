#include <ntfs-browser/win-types.h>

#include <vector>

#include <ntfs-browser/namespace.h>
#include <ntfs-browser/test/fake-image/fake-ntfs-image.h>
#include <ntfs-browser/tool/fuzzgen/legacy-builder-scenarios.h>
#include <ntfs-browser/tool/fuzzgen/legacy-fixtures.h>
#include <ntfs-browser/tool/fuzzgen/legacy-geometry.h>
#include <ntfs-browser/tool/fuzzgen/record-edit.h>
#include <ntfs-browser/tool/fuzzgen/recorded-stream.h>
#include <ntfs-browser/tool/fuzzgen/scenario-registry.h>

namespace NTFS_BROWSER_NAMESPACE::Tool::FuzzGen {

namespace {

namespace FakeImage = Test::FakeImage;

using Image = std::vector<BYTE>;
using Probe = RecordedStream::Probe;

// The root directory record, which the old builders sized without padding.
constexpr ULONGLONG root_record = 5;

// Cuts the root record to the size the old builders gave it.
void PackRoot(Image& image) {
  LegacyGeometry::PackLastAttribute(image, root_record);
}

const std::vector<LegacyFixtures::Damaged>& Inputs() {
  static const std::vector<LegacyFixtures::Damaged> inputs = {
      {.name = "attr_offset_exceeds_record_size",
       .base = FakeImage::BuildFakeNtfsImageWithAttrOffsetOutOfBounds,
       .early = true},
      {.name = "corrupt_mft_record_volume_ok",
       .base = FakeImage::BuildFakeNtfsImageWithCorruptMftRecord,
       .early = true},
      {.name = "attribute_walk_no_end_marker",
       .base = FakeImage::BuildFakeNtfsImageWithNoEndMarker,
       .probe = Probe::FullCache},
      {.name = "find_stream_named_data",
       .base = FakeImage::BuildFakeNtfsImageWithNamedDataStream,
       .damage = PackRoot},
      {.name = "compressed_index_allocation_missing_compressed_size",
       .base = FakeImage::
           BuildFakeNtfsImageWithMissingCompressedSizeIndexAllocation,
       .damage = PackRoot},
      {.name = "standard_information_minimal_size",
       .base = FakeImage::BuildFakeNtfsImageWithLegacyStandardInformation},
      {.name = "volume_information_minimal_size",
       .base = FakeImage::BuildFakeNtfsImage},
      {.name = "root_record_parse_failure",
       .base = FakeImage::BuildFakeNtfsImageWithCorruptRootRecord},
      {.name = "corrupt_compressed_index_allocation",
       .base =
           FakeImage::BuildFakeNtfsImageWithCorruptCompressedIndexAllocation,
       .damage = PackRoot,
       .needs = Feature::Decompression},
      {.name = "compressed_index_allocation_unmapped_unit",
       .base = FakeImage::
           BuildFakeNtfsImageWithUnmappedCompressionUnitIndexAllocation,
       .damage = PackRoot,
       .needs = Feature::Decompression},
      {.name = "compressed_index_allocation_sparse_unit",
       .base = FakeImage::
           BuildFakeNtfsImageWithSparseCompressionUnitIndexAllocation,
       .damage = PackRoot,
       .needs = Feature::Decompression},
      {.name = "compressed_index_allocation_real_after_hole",
       .base = FakeImage::
           BuildFakeNtfsImageWithRealClustersAfterHoleIndexAllocation,
       .damage = PackRoot,
       .needs = Feature::Decompression},
      {.name = "compressed_index_allocation_short_decompressed_unit",
       .base = FakeImage::
           BuildFakeNtfsImageWithShortDecompressedUnitIndexAllocation,
       .damage = PackRoot,
       .needs = Feature::Decompression},
      {.name = "compressed_index_allocation_misaligned_start_vcn",
       .base =
           FakeImage::BuildFakeNtfsImageWithMisalignedStartVcnIndexAllocation,
       .damage = PackRoot,
       .needs = Feature::Decompression},
      {.name = "compressed_index_allocation_oversized_compression_unit",
       .base = FakeImage::
           BuildFakeNtfsImageWithOversizedCompressionUnitIndexAllocation,
       .damage = PackRoot,
       .needs = Feature::Decompression},
      {.name = "compressed_index_allocation_comp_unit_size_out_of_range",
       .base = FakeImage::
           BuildFakeNtfsImageWithCompUnitSizeOutOfRangeIndexAllocation,
       .damage = PackRoot,
       .needs = Feature::Decompression},
      {.name = "compressed_index_allocation_lznt1_backreference_exceeds_dest",
       .base = FakeImage::
           BuildFakeNtfsImageWithLznt1BackreferenceExceedsDestIndexAllocation,
       .damage = PackRoot,
       .needs = Feature::Decompression},
      {.name = "compressed_index_allocation_lznt1_chunk_exceeds_src_bounds",
       .base = FakeImage::
           BuildFakeNtfsImageWithLznt1ChunkExceedsSrcBoundsIndexAllocation,
       .damage = PackRoot,
       .needs = Feature::Decompression},
      {.name = "compressed_index_allocation_lznt1_chunk_over_4096",
       .base =
           FakeImage::BuildFakeNtfsImageWithLznt1ChunkOver4096IndexAllocation,
       .damage = PackRoot,
       .needs = Feature::Decompression},
      {.name = "compressed_index_allocation_lznt1_invalid_signature",
       .base = FakeImage::
           BuildFakeNtfsImageWithLznt1InvalidSignatureIndexAllocation,
       .damage = PackRoot,
       .needs = Feature::Decompression},
      {.name = "compressed_index_allocation_lznt1_literal_exceeds_dest",
       .base = FakeImage::
           BuildFakeNtfsImageWithLznt1LiteralExceedsDestIndexAllocation,
       .damage = PackRoot,
       .needs = Feature::Decompression},
      {.name = "compressed_index_allocation_lznt1_truncated_word",
       .base =
           FakeImage::BuildFakeNtfsImageWithLznt1TruncatedWordIndexAllocation,
       .damage = PackRoot,
       .needs = Feature::Decompression},
      {.name =
           "compressed_index_allocation_lznt1_uncompressed_chunk_exceeds_dest",
       .base = FakeImage::
           BuildFakeNtfsImageWithLznt1UncompressedChunkExceedsDestIndexAllocation,
       .damage = PackRoot,
       .needs = Feature::Decompression},
  };
  return inputs;
}

}  // namespace

void LegacyBuilderScenarios::Register(ScenarioRegistry& registry) {
  LegacyFixtures::RegisterDamaged(registry, Inputs());
}

}  // namespace NTFS_BROWSER_NAMESPACE::Tool::FuzzGen
