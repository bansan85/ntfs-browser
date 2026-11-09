#include "legacy-builder-scenarios.h"

#include <vector>

#include <ntfs-browser/namespace.h>
#include <ntfs-browser/test/fake-image/fake-ntfs-image.h>
#include <ntfs-browser/tool/fuzzgen/recorded-stream.h>
#include <ntfs-browser/tool/fuzzgen/scenario-registry.h>

#include "data/index-block.h"
#include "index-edit.h"
#include "legacy-fixtures.h"
#include "legacy-geometry.h"
#include "record-edit.h"
#include "win-types.h"

namespace NTFS_BROWSER_NAMESPACE::Tool::FuzzGen {

namespace {

namespace FakeImage = Test::FakeImage;

using Image = std::vector<BYTE>;
using Probe = RecordedStream::Probe;

// The root directory record, which the old builders sized without padding.
constexpr ULONGLONG root_record = 5;

// Where the update sequence array starts in a block of one sector, and the
// words of that array.
constexpr WORD block_array_offset = 0x3FC;
constexpr WORD one_sector_array_size = 2;

// Cuts the root record to the size the old builders gave it.
void PackRoot(Image& image) {
  LegacyGeometry::PackLastAttribute(image, root_record);
}

// The first builders put an index block into a compressed unit without the
// padding of its entries, and with the array of a one-sector block. Gives the
// block that form.
void UnpackCompressedBlock(Image& image) {
  const std::size_t block = IndexEdit::FindBlock(image);
  IndexEdit::UnpadBlock(image, block);
  RecordEdit::Edit<Data::IndexBlock>(image, block, [](auto& header) {
    header.offset_of_us = block_array_offset;
    header.size_of_us = one_sector_array_size;
  });
}

// A run of one cluster at a logical cluster number whose byte address
// overflows, which is 8 bytes of offset in a run header of 0x81.
constexpr std::size_t compressed_attribute_header_bytes = 0x48;

// The run lists of a compression unit of four clusters whose first run lies at
// the overflowing cluster: that run takes one cluster and a hole takes three,
// or that run takes all four (a stored unit).
const std::vector<BYTE> compressed_unit_bad_runs = {
    0x81, 0x01, 0, 0, 0, 0, 0, 0, 0x20, 0x00, 0x01, 0x03, 0x00};
const std::vector<BYTE> stored_unit_bad_runs = {0x81, 0x04, 0,    0,    0,   0,
                                                0,    0,    0x20, 0x00, 0x00};

// Where a compressed attribute keeps its compressed size, after the 64 bytes
// of the header of a non-resident one, and the sizes of the two inputs: one
// cluster, and the four clusters of a stored unit.
constexpr std::size_t compressed_size_field = 0x40;
constexpr ULONGLONG one_cluster_size = 0x400;
constexpr ULONGLONG four_clusters_size = 0x1000;

// Replaces the run list of the compressed $INDEX_ALLOCATION of the root, and
// cuts the attribute to the size of its header and its runs.
void PutRuns(Image& image, const std::vector<BYTE>& runs,
             ULONGLONG compressed_size) {
  const std::size_t attr = RecordEdit::Attribute(
      image, root_record, RecordEdit::type_index_allocation);
  RecordEdit::StoreQword(image, attr + compressed_size_field, compressed_size);
  RecordEdit::SetRunList(image, attr, runs);
  RecordEdit::ShrinkLastAttribute(
      image, attr,
      static_cast<DWORD>(compressed_attribute_header_bytes + runs.size()));
}

// Packs the root, and the block of the compressed unit.
void PackRootAndBlock(Image& image) {
  IndexEdit::UnpadRoot(image, root_record);
  PackRoot(image);
  UnpackCompressedBlock(image);
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
      {.name = "index_block_chain_depth_limit",
       .base = FakeImage::BuildFakeNtfsImageWithDeepIndexBlockChain,
       .damage = PackRoot},
      {.name = "compressed_index_allocation",
       .base = FakeImage::BuildFakeNtfsImageWithCompressedIndexAllocation,
       .damage = PackRootAndBlock,
       .needs = Feature::Decompression},
      {.name = "compressed_index_allocation_compressed_unit_bad_lcn",
       .base = FakeImage::BuildFakeNtfsImageWithCompressedIndexAllocation,
       .damage =
           [](Image& image) {
             PutRuns(image, compressed_unit_bad_runs, one_cluster_size);
           },
       .needs = Feature::Decompression},
      {.name = "compressed_index_allocation_stored_unit_bad_lcn",
       .base = FakeImage::BuildFakeNtfsImageWithCompressedIndexAllocation,
       .damage =
           [](Image& image) {
             PutRuns(image, stored_unit_bad_runs, four_clusters_size);
           },
       .needs = Feature::Decompression},
      {.name = "surrogate_pair_names",
       .base = FakeImage::BuildFakeNtfsImageWithSurrogatePairNames,
       .damage = PackRootAndBlock,
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
