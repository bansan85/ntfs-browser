#include <cassert>
#include <span>
#include <vector>

#include "fake-ntfs-image-internal.h"

namespace NtfsBrowserTests {

std::vector<BYTE> MakeUncompressedLznt1Chunk(std::span<const BYTE> payload) {
  // [MS-XCA] section 2.5.3: input streams are compressed in units of 4096
  // bytes, so a single chunk never carries more than that (and a chunk with
  // no payload at all is not representable - the declared size is
  // payload.size() - 1).
  assert(!payload.empty() && payload.size() <= NtfsBrowser::Lznt1::chunk_size);

  // Header: bit 15 clear (uncompressed), bits 14-12 == 3 (signature), bits
  // 11-0 == payload size - 1 (the whole chunk's size, header included, minus
  // three) - [MS-XCA] section 2.5.1.2.
  const auto header =
      gsl::narrow<WORD>(0x3000U | gsl::narrow<unsigned>(payload.size() - 1));

  std::vector<BYTE> chunk;
  chunk.reserve(payload.size() + 2);
  chunk.push_back(static_cast<BYTE>(header & byte_mask));
  chunk.push_back(static_cast<BYTE>(header >> bits_per_byte));
  chunk.insert(chunk.end(), payload.begin(), payload.end());
  return chunk;
}

std::vector<BYTE> BuildFakeNtfsImageWithCompressedIndexAllocation() {
  // Two real clusters (a 1026-byte uncompressed LZNT1 chunk wrapping one
  // whole index block) plus a 2-cluster sparse pad: fewer real clusters than
  // the unit holds, so the unit is compressed.
  const std::vector<FakeDataRun> runs{{compressed_data_lcn, 2},
                                      {{}, compression_unit_clusters - 2}};

  const FakeRecord record = MakeIndexAllocationDirRecord(
      NtfsBrowser::Data::StdInfoPermission::Archive |
          NtfsBrowser::Data::StdInfoPermission::Compressed,
      {.comp_unit_size = compression_unit_size_shift,
       .real_size = fake_file_record_size,
       .runs = runs});

  return BuildCompressionImage(
      record, runs,
      MakeUncompressedLznt1Chunk(MakeCompressedIndexBlockContent()));
}

std::vector<BYTE> BuildFakeNtfsImageWithCorruptCompressedIndexAllocation() {
  const std::vector<FakeDataRun> runs{{compressed_data_lcn, 2},
                                      {{}, compression_unit_clusters - 2}};

  const FakeRecord record = MakeIndexAllocationDirRecord(
      NtfsBrowser::Data::StdInfoPermission::Archive |
          NtfsBrowser::Data::StdInfoPermission::Compressed,
      {.comp_unit_size = compression_unit_size_shift,
       .real_size = fake_file_record_size,
       .runs = runs});

  return BuildCompressionImage(record, runs, MakeCorruptLznt1Chunk());
}

////////////////////////////////////////////////////////////////////////////
// Fuzz-corpus-only compressed $INDEX_ALLOCATION fixtures - stay on
// $INDEX_ALLOCATION, never $DATA, since that is all FuzzOnce() ReadData()s.
////////////////////////////////////////////////////////////////////////////

std::vector<BYTE>
    BuildFakeNtfsImageWithCompUnitSizeOutOfRangeIndexAllocation() {
  const std::vector<FakeDataRun> runs{{compressed_data_lcn, 2},
                                      {{}, compression_unit_clusters - 2}};

  const FakeRecord record = MakeIndexAllocationDirRecord(
      NtfsBrowser::Data::StdInfoPermission::Archive |
          NtfsBrowser::Data::StdInfoPermission::Compressed,
      {.comp_unit_size = comp_unit_size_out_of_range_shift,
       .real_size = fake_file_record_size,
       .runs = runs});

  return BuildCompressionImage(
      record, runs,
      MakeUncompressedLznt1Chunk(MakeCompressedIndexBlockContent()));
}

std::vector<BYTE>
    BuildFakeNtfsImageWithOversizedCompressionUnitIndexAllocation() {
  const std::vector<FakeDataRun> runs{{compressed_data_lcn, 2},
                                      {{}, compression_unit_clusters - 2}};

  const FakeRecord record = MakeIndexAllocationDirRecord(
      NtfsBrowser::Data::StdInfoPermission::Archive |
          NtfsBrowser::Data::StdInfoPermission::Compressed,
      {.comp_unit_size = oversized_comp_unit_size_shift,
       .real_size = fake_file_record_size,
       .runs = runs});

  return BuildCompressionImage(
      record, runs,
      MakeUncompressedLznt1Chunk(MakeCompressedIndexBlockContent()));
}

std::vector<BYTE> BuildFakeNtfsImageWithMisalignedStartVcnIndexAllocation() {
  const std::vector<FakeDataRun> runs{{compressed_data_lcn, 2},
                                      {{}, compression_unit_clusters - 2}};

  const FakeRecord record = MakeIndexAllocationDirRecord(
      NtfsBrowser::Data::StdInfoPermission::Archive |
          NtfsBrowser::Data::StdInfoPermission::Compressed,
      {.comp_unit_size = compression_unit_size_shift,
       .real_size = fake_file_record_size,
       .runs = runs,
       .overrides = {.start_vcn = misaligned_compressed_start_vcn}});

  return BuildCompressionImage(
      record, runs,
      MakeUncompressedLznt1Chunk(MakeCompressedIndexBlockContent()));
}

std::vector<BYTE> BuildFakeNtfsImageWithMissingCompressedSizeIndexAllocation() {
  const std::vector<FakeDataRun> runs{{compressed_data_lcn, 2},
                                      {{}, compression_unit_clusters - 2}};

  const FakeRecord record = MakeIndexAllocationDirRecord(
      NtfsBrowser::Data::StdInfoPermission::Archive |
          NtfsBrowser::Data::StdInfoPermission::Compressed,
      {.comp_unit_size = compression_unit_size_shift,
       .real_size = fake_file_record_size,
       .runs = runs,
       .overrides = {.total_size = compressed_attr_truncated_total_size}});

  return BuildCompressionImage(
      record, runs,
      MakeUncompressedLznt1Chunk(MakeCompressedIndexBlockContent()));
}

std::vector<BYTE>
    BuildFakeNtfsImageWithUnmappedCompressionUnitIndexAllocation() {
  // Only 2 of unit 0's 4 clusters are actually mapped by the run list, but
  // last_vcn (forced to 2 via the override) claims a 3-cluster attribute -
  // LeadingRealClusters() walks the one real run, reaches vcn 2, then runs
  // out of runs with vcn(2) != unitEnd(3).
  const std::vector<FakeDataRun> runs{{compressed_data_lcn, 2}};

  const FakeRecord record = MakeIndexAllocationDirRecord(
      NtfsBrowser::Data::StdInfoPermission::Archive |
          NtfsBrowser::Data::StdInfoPermission::Compressed,
      {.comp_unit_size = compression_unit_size_shift,
       .real_size = fake_file_record_size,
       .runs = runs,
       .overrides = {.last_vcn = 2}});

  return BuildCompressionImage(
      record, runs, CompressionFixturePattern(compression_unit_size));
}

std::vector<BYTE> BuildFakeNtfsImageWithRealClustersAfterHoleIndexAllocation() {
  // real, hole, real - within a single compression unit, no encoding this
  // library understands produces real clusters after a hole, so
  // LeadingRealClusters() must reject it before ParseIndexBlock() ever tries
  // to decompress anything.
  const std::vector<FakeDataRun> runs{
      {compressed_data_lcn, 1}, {{}, 1}, {fragmented_compressed_data_lcn, 2}};

  const FakeRecord record = MakeIndexAllocationDirRecord(
      NtfsBrowser::Data::StdInfoPermission::Archive |
          NtfsBrowser::Data::StdInfoPermission::Compressed,
      {.comp_unit_size = compression_unit_size_shift,
       .real_size = compression_unit_size,
       .runs = runs});

  // Content is irrelevant - the layout is rejected before anything is
  // decompressed - but real LZNT1-looking bytes keep the fixture honest
  // about being otherwise plausible.
  const std::vector<BYTE> cluster_bytes(xca_lznt1_example_compressed.begin(),
                                        xca_lznt1_example_compressed.end());
  return BuildCompressionImage(record, runs, cluster_bytes);
}

std::vector<BYTE> BuildFakeNtfsImageWithSparseCompressionUnitIndexAllocation() {
  // A pure hole: LeadingRealClusters() returns 0, so GetCompressionUnit()
  // takes its "sparse" branch - the fixture's own point, independent of
  // ParseIndexBlock()'s later, separate magic-check failure on the result.
  const std::vector<FakeDataRun> runs{{{}, compression_unit_clusters}};

  const FakeRecord record = MakeIndexAllocationDirRecord(
      NtfsBrowser::Data::StdInfoPermission::Archive |
          NtfsBrowser::Data::StdInfoPermission::Compressed |
          NtfsBrowser::Data::StdInfoPermission::Sparse,
      {.comp_unit_size = compression_unit_size_shift,
       .real_size = compression_unit_size,
       .runs = runs});

  return BuildCompressionImage(record, runs, {});
}

std::vector<BYTE> BuildFakeNtfsImageWithShortDecompressedUnitIndexAllocation() {
  const std::vector<FakeDataRun> runs{{compressed_data_lcn, 1},
                                      {{}, compression_unit_clusters - 1}};

  const FakeRecord record = MakeIndexAllocationDirRecord(
      NtfsBrowser::Data::StdInfoPermission::Archive |
          NtfsBrowser::Data::StdInfoPermission::Compressed,
      {.comp_unit_size = compression_unit_size_shift,
       .real_size = compression_unit_size,
       .runs = runs});

  return BuildCompressionImage(
      record, runs,
      MakeUncompressedLznt1Chunk(
          CompressionFixturePattern(short_decompressed_unit_size)));
}

////////////////////////////////////////////////////////////////////////////
// LZNT1 decompressor rejection-path fixtures (src/lznt1/decompress.cpp):
// each a single compression unit whose real cluster(s) hold a hand-crafted,
// malformed LZNT1 byte stream.
////////////////////////////////////////////////////////////////////////////

std::vector<BYTE> BuildFakeNtfsImageWithLznt1InvalidSignatureIndexAllocation() {
  const std::vector<FakeDataRun> runs{{compressed_data_lcn, 1},
                                      {{}, compression_unit_clusters - 1}};

  const FakeRecord record = MakeIndexAllocationDirRecord(
      NtfsBrowser::Data::StdInfoPermission::Archive |
          NtfsBrowser::Data::StdInfoPermission::Compressed,
      {.comp_unit_size = compression_unit_size_shift,
       .real_size = fake_file_record_size,
       .runs = runs});

  // Chunk header 0x1002: bit 15 clear (not the end-of-buffer 0x0000 marker),
  // bits 14-12 == 1 != the mandatory signature 3.
  const std::vector<BYTE> cluster_bytes{0x02, 0x10};
  return BuildCompressionImage(record, runs, cluster_bytes);
}

std::vector<BYTE>
    BuildFakeNtfsImageWithLznt1ChunkExceedsSrcBoundsIndexAllocation() {
  const std::vector<FakeDataRun> runs{{compressed_data_lcn, 1},
                                      {{}, compression_unit_clusters - 1}};

  const FakeRecord record = MakeIndexAllocationDirRecord(
      NtfsBrowser::Data::StdInfoPermission::Archive |
          NtfsBrowser::Data::StdInfoPermission::Compressed,
      {.comp_unit_size = compression_unit_size_shift,
       .real_size = fake_file_record_size,
       .runs = runs});

  // Chunk header 0xBFFF: valid, declares a 4096-byte payload - far more than
  // the ~1022 bytes actually available in the one real cluster.
  const std::vector<BYTE> cluster_bytes{0xFF, 0xBF};
  return BuildCompressionImage(record, runs, cluster_bytes);
}

std::vector<BYTE>
    BuildFakeNtfsImageWithLznt1UncompressedChunkExceedsDestIndexAllocation() {
  const std::vector<FakeDataRun> runs{{compressed_data_lcn, 1},
                                      {{}, compression_unit_clusters - 1}};

  const FakeRecord record = MakeIndexAllocationDirRecord(
      NtfsBrowser::Data::StdInfoPermission::Archive |
          NtfsBrowser::Data::StdInfoPermission::Compressed,
      {.comp_unit_size = compression_unit_size_shift,
       .real_size = fake_file_record_size,
       .runs = runs});

  // Chunk 1 decompresses to just short of the 4096-byte unit; chunk 2
  // (uncompressed, declared payload 10) has too little dest buffer left.
  const std::vector<BYTE> cluster_bytes{0x03, 0xB0, 0x02, 0xAA,
                                        0xF4, 0x0F, 0x09, 0x30};
  return BuildCompressionImage(record, runs, cluster_bytes);
}

std::vector<BYTE> BuildFakeNtfsImageWithLznt1ChunkOver4096IndexAllocation() {
  const std::vector<FakeDataRun> runs{{compressed_data_lcn, 1},
                                      {{}, compression_unit_clusters - 1}};

  const FakeRecord record = MakeIndexAllocationDirRecord(
      NtfsBrowser::Data::StdInfoPermission::Archive |
          NtfsBrowser::Data::StdInfoPermission::Compressed,
      {.comp_unit_size = compression_unit_size_shift,
       .real_size = fake_file_record_size,
       .runs = runs});

  // Output reaches exactly 4096 bytes, then one more trailing byte forces a
  // 3rd data element whose own bounds check must reject it.
  const std::vector<BYTE> cluster_bytes{0x04, 0xB0, 0x02, 0xAA,
                                        0xFC, 0x0F, 0x00};
  return BuildCompressionImage(record, runs, cluster_bytes);
}

std::vector<BYTE>
    BuildFakeNtfsImageWithLznt1LiteralExceedsDestIndexAllocation() {
  const std::vector<FakeDataRun> runs{{compressed_data_lcn, 1},
                                      {{}, compression_unit_clusters - 1}};

  const FakeRecord record = MakeIndexAllocationDirRecord(
      NtfsBrowser::Data::StdInfoPermission::Archive |
          NtfsBrowser::Data::StdInfoPermission::Compressed,
      {.comp_unit_size = compression_unit_size_shift,
       .real_size = fake_file_record_size,
       .runs = runs});

  // Chunk 1 fills the dest buffer to exactly 4096 bytes; chunk 2's first
  // element is a literal, rejected for writing past a buffer already full.
  const std::vector<BYTE> cluster_bytes{0x03, 0xB0, 0x02, 0xAA, 0xFC,
                                        0x0F, 0x01, 0xB0, 0x00, 0x00};
  return BuildCompressionImage(record, runs, cluster_bytes);
}

std::vector<BYTE> BuildFakeNtfsImageWithLznt1TruncatedWordIndexAllocation() {
  const std::vector<FakeDataRun> runs{{compressed_data_lcn, 1},
                                      {{}, compression_unit_clusters - 1}};

  const FakeRecord record = MakeIndexAllocationDirRecord(
      NtfsBrowser::Data::StdInfoPermission::Archive |
          NtfsBrowser::Data::StdInfoPermission::Compressed,
      {.comp_unit_size = compression_unit_size_shift,
       .real_size = fake_file_record_size,
       .runs = runs});

  // Chunk header 0xB001 (payload 2): a flag byte (0x01 - a compressed word)
  // followed by only ONE more byte, not the two a compressed word needs.
  const std::vector<BYTE> cluster_bytes{0x01, 0xB0, 0x01, 0x00};
  return BuildCompressionImage(record, runs, cluster_bytes);
}

std::vector<BYTE>
    BuildFakeNtfsImageWithLznt1BackreferenceExceedsDestIndexAllocation() {
  // comp_unit_size == 1 (2048-byte unit), deliberately smaller than
  // chunk_size, so a legal-looking back-reference can still be rejected
  // purely for exceeding this smaller unit's own dest buffer.
  const std::vector<FakeDataRun> runs{{compressed_data_lcn, 1}, {{}, 1}};

  const FakeRecord record = MakeIndexAllocationDirRecord(
      NtfsBrowser::Data::StdInfoPermission::Archive |
          NtfsBrowser::Data::StdInfoPermission::Compressed,
      {.comp_unit_size = 1, .real_size = fake_file_record_size, .runs = runs});

  // Back-reference (length 2048) fits the per-chunk cap but exceeds this
  // unit's own 2048-byte dest buffer.
  const std::vector<BYTE> cluster_bytes{0x03, 0xB0, 0x02, 0xAA, 0xFD, 0x07};
  return BuildCompressionImage(record, runs, cluster_bytes);
}

}  // namespace NtfsBrowserTests
