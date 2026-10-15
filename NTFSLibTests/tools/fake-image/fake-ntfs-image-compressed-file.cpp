#include <algorithm>
#include <cstring>
#include <vector>

#include <ntfs-browser/mft/idx.h>
#include <ntfs-browser/namespace.h>

#include "fake-ntfs-image-internal.h"

namespace NTFS_BROWSER_NAMESPACE::Tests {

namespace {

// A bitmap byte with every bit set, and the mask and shift that split a
// WORD into bytes.
constexpr BYTE all_bits_set = 0xFF;

}  // namespace

std::vector<BYTE> CompressionFixturePattern(size_t size) {
  std::vector<BYTE> pattern(size, 0);
  for (size_t i = 0; i < size; i++) {
    // Deliberately not a byte-aligned cycle, so a fixture whose content got
    // shifted by a whole number of bytes/clusters still compares unequal.
    pattern.at(i) =
        static_cast<BYTE>((i * pattern_mul + pattern_add) % pattern_mod);
  }
  return pattern;
}

std::vector<BYTE> BuildFakeNtfsImageWithCompressedFile() {
  // One real cluster (the [MS-XCA] section 3.3 worked example's 59
  // compressed bytes) plus a sparse pad up to a whole compression unit -
  // 1 < compression_unit_clusters real clusters is exactly what marks a unit
  // as compressed rather than stored.
  const std::vector<FakeDataRun> runs{{compressed_data_lcn, 1},
                                      {{}, compression_unit_clusters - 1}};

  const FakeRecord record = MakeNonResidentDataRecord(
      NTFS_BROWSER_NAMESPACE::Data::StdInfoPermission::Archive |
          NTFS_BROWSER_NAMESPACE::Data::StdInfoPermission::Compressed,
      compression_unit_size_shift, xca_lznt1_example_decompressed_size, runs);

  const std::vector<BYTE> cluster_bytes(xca_lznt1_example_compressed.begin(),
                                        xca_lznt1_example_compressed.end());
  return BuildCompressionImage(record, runs, cluster_bytes);
}

std::vector<BYTE> BuildFakeNtfsImageWithStoredCompressionUnit() {
  // Real runs covering the whole unit, no sparse pad: a stored
  // (incompressible) unit, whose bytes must come back untouched.
  const std::vector<FakeDataRun> runs{
      {compressed_data_lcn, compression_unit_clusters}};

  const FakeRecord record = MakeNonResidentDataRecord(
      NTFS_BROWSER_NAMESPACE::Data::StdInfoPermission::Archive |
          NTFS_BROWSER_NAMESPACE::Data::StdInfoPermission::Compressed,
      compression_unit_size_shift, compression_unit_size, runs);

  return BuildCompressionImage(
      record, runs, CompressionFixturePattern(compression_unit_size));
}

std::vector<BYTE> BuildFakeNtfsImageWithUninitializedTail() {
  const std::vector<FakeDataRun> runs{
      {compressed_data_lcn, uninitialized_tail_clusters}};

  const FakeRecord record = MakeNonResidentDataRecord(
      NTFS_BROWSER_NAMESPACE::Data::StdInfoPermission::Archive, 0,
      uninitialized_tail_real_size, runs,
      {.ini_size = uninitialized_tail_ini_size});

  return BuildCompressionImage(
      record, runs,
      CompressionFixturePattern(size_t{uninitialized_tail_clusters} *
                                cluster_size));
}

std::vector<BYTE> BuildFakeNtfsImageWithMultiClusterBitmap() {
  const std::vector<FakeDataRun> runs{
      {compressed_data_lcn, multi_cluster_bitmap_clusters}};

  FakeRecord record = MakeRecordHeader(
      attr_offset_value, NTFS_BROWSER_NAMESPACE::Data::FileRecordFlag::InUse);
  DWORD offset = attr_offset_value;
  offset += WriteStandardInformationAttr(
      record, offset, NTFS_BROWSER_NAMESPACE::Data::StdInfoPermission::Archive);
  offset += WriteNonResidentAttr(
      record, offset, Attr::Type::Bitmap,
      {.comp_unit_size = 0,
       .real_size =
           static_cast<ULONGLONG>(multi_cluster_bitmap_clusters) * cluster_size,
       .runs = runs});
  WriteEndOfAttributesMarker(record, offset);

  std::vector<BYTE> bitmap(
      static_cast<size_t>(multi_cluster_bitmap_clusters) * cluster_size, 0x00);
  std::fill_n(bitmap.begin(), cluster_size, all_bits_set);
  bitmap.at(static_cast<size_t>(2) * cluster_size) = 0x01;

  return BuildCompressionImage(record, runs, bitmap);
}

std::vector<BYTE> BuildFakeNtfsImageWithSparseCompressionUnit() {
  const std::vector<FakeDataRun> runs{{{}, compression_unit_clusters}};

  const FakeRecord record = MakeNonResidentDataRecord(
      NTFS_BROWSER_NAMESPACE::Data::StdInfoPermission::Archive |
          NTFS_BROWSER_NAMESPACE::Data::StdInfoPermission::Compressed |
          NTFS_BROWSER_NAMESPACE::Data::StdInfoPermission::Sparse,
      compression_unit_size_shift, compression_unit_size, runs);

  return BuildCompressionImage(record, runs, {});
}

std::vector<BYTE> BuildFakeNtfsImageWithSparseThenCompressedUnit() {
  // Unit 0 (VCN 0..3) is a hole; unit 1 (VCN 4..7) holds one real cluster
  // and a sparse pad, like BuildFakeNtfsImageWithCompressedFile().
  const std::vector<FakeDataRun> runs{{{}, compression_unit_clusters},
                                      {compressed_data_lcn, 1},
                                      {{}, compression_unit_clusters - 1}};

  const FakeRecord record = MakeNonResidentDataRecord(
      NTFS_BROWSER_NAMESPACE::Data::StdInfoPermission::Archive |
          NTFS_BROWSER_NAMESPACE::Data::StdInfoPermission::Compressed,
      compression_unit_size_shift, sparse_then_compressed_real_size, runs);

  const std::vector<BYTE> cluster_bytes(xca_lznt1_example_compressed.begin(),
                                        xca_lznt1_example_compressed.end());
  return BuildCompressionImage(record, runs, cluster_bytes);
}

std::vector<BYTE> BuildFakeNtfsImageWithHoleBeforeRealCluster() {
  const std::vector<FakeDataRun> runs{
      {{}, 1}, {compressed_data_lcn, 1}, {{}, 2}};

  const FakeRecord record = MakeNonResidentDataRecord(
      NTFS_BROWSER_NAMESPACE::Data::StdInfoPermission::Archive |
          NTFS_BROWSER_NAMESPACE::Data::StdInfoPermission::Compressed,
      compression_unit_size_shift, compression_unit_size, runs);

  const std::vector<BYTE> cluster_bytes(xca_lznt1_example_compressed.begin(),
                                        xca_lznt1_example_compressed.end());
  return BuildCompressionImage(record, runs, cluster_bytes);
}

std::vector<BYTE> BuildFakeNtfsImageWithFragmentedCompressedFile() {
  // Two non-contiguous single-cluster real runs (so the compressed bytes
  // genuinely span two Data::RunEntrys) plus a 2-cluster sparse pad.
  const std::vector<FakeDataRun> runs{{compressed_data_lcn, 1},
                                      {fragmented_compressed_data_lcn, 1},
                                      {{}, compression_unit_clusters - 2}};

  const FakeRecord record = MakeNonResidentDataRecord(
      NTFS_BROWSER_NAMESPACE::Data::StdInfoPermission::Archive |
          NTFS_BROWSER_NAMESPACE::Data::StdInfoPermission::Compressed,
      compression_unit_size_shift, fragmented_compressed_payload_size, runs);

  const std::vector<BYTE> payload =
      CompressionFixturePattern(fragmented_compressed_payload_size);
  return BuildCompressionImage(record, runs,
                               MakeUncompressedLznt1Chunk(payload));
}

std::vector<BYTE> BuildFakeNtfsImageWithTrailingPartialCompressionUnit() {
  // Five real clusters then one sparse: unit 0 (VCN 0..3) is entirely real
  // (stored), and unit 1 (VCN 4..5 only - the attribute stops there) sees
  // the tail of that same run plus one hole, so it is a compressed unit
  // whose real extent comes from a PARTIAL run rather than a run of its own.
  const std::vector<FakeDataRun> runs{
      {compressed_data_lcn, compression_unit_clusters + 1}, {{}, 1}};

  const FakeRecord record = MakeNonResidentDataRecord(
      NTFS_BROWSER_NAMESPACE::Data::StdInfoPermission::Archive |
          NTFS_BROWSER_NAMESPACE::Data::StdInfoPermission::Compressed,
      compression_unit_size_shift,
      trailing_partial_unit_stored_size + trailing_partial_unit_tail_size,
      runs);

  // Clusters 0..3 hold unit 0's stored bytes verbatim; cluster 4 holds unit
  // 1's LZNT1 stream (a single hand-encoded uncompressed chunk).
  std::vector<BYTE> cluster_bytes =
      CompressionFixturePattern(trailing_partial_unit_stored_size);
  const std::vector<BYTE> tail = MakeUncompressedLznt1Chunk(
      CompressionFixturePattern(trailing_partial_unit_tail_size));
  cluster_bytes.insert(cluster_bytes.end(), tail.begin(), tail.end());

  return BuildCompressionImage(record, runs, cluster_bytes);
}

std::vector<BYTE> BuildFakeNtfsImageWithCorruptCompressedUnit() {
  const std::vector<FakeDataRun> runs{{compressed_data_lcn, 1},
                                      {{}, compression_unit_clusters - 1}};

  const FakeRecord record = MakeNonResidentDataRecord(
      NTFS_BROWSER_NAMESPACE::Data::StdInfoPermission::Archive |
          NTFS_BROWSER_NAMESPACE::Data::StdInfoPermission::Compressed,
      compression_unit_size_shift, xca_lznt1_example_decompressed_size, runs);

  return BuildCompressionImage(record, runs, MakeCorruptLznt1Chunk());
}

std::vector<BYTE> BuildFakeNtfsImageWithUnmappedCompressionUnit() {
  // Only unit 0's clusters are actually mapped; last_vcn claims two units'
  // worth.
  const std::vector<FakeDataRun> runs{
      {compressed_data_lcn, compression_unit_clusters}};

  const FakeRecord record = MakeNonResidentDataRecord(
      NTFS_BROWSER_NAMESPACE::Data::StdInfoPermission::Archive |
          NTFS_BROWSER_NAMESPACE::Data::StdInfoPermission::Compressed,
      compression_unit_size_shift, 2ULL * compression_unit_size, runs,
      {.last_vcn = 2ULL * compression_unit_clusters - 1});

  return BuildCompressionImage(
      record, runs, CompressionFixturePattern(compression_unit_size));
}

std::vector<BYTE> BuildFakeNtfsImageWithRealClustersAfterHole() {
  const std::vector<FakeDataRun> runs{
      {compressed_data_lcn, 1}, {{}, 1}, {fragmented_compressed_data_lcn, 2}};

  const FakeRecord record = MakeNonResidentDataRecord(
      NTFS_BROWSER_NAMESPACE::Data::StdInfoPermission::Archive |
          NTFS_BROWSER_NAMESPACE::Data::StdInfoPermission::Compressed,
      compression_unit_size_shift, compression_unit_size, runs);

  // Content is irrelevant - the layout is rejected before anything is
  // decompressed - but a real LZNT1 stream keeps the fixture honest about
  // being otherwise plausible.
  const std::vector<BYTE> cluster_bytes(xca_lznt1_example_compressed.begin(),
                                        xca_lznt1_example_compressed.end());
  return BuildCompressionImage(record, runs, cluster_bytes);
}

std::vector<BYTE> BuildFakeNtfsImageWithShortDecompressedUnit() {
  // One real cluster then seven sparse: unit 0 (VCN 0..3) is compressed,
  // unit 1 (VCN 4..7) a hole. real_size covers both, so unit 0 is interior
  // and must yield a whole compression_unit_size bytes.
  const std::vector<FakeDataRun> runs{
      {compressed_data_lcn, 1}, {{}, 2ULL * compression_unit_clusters - 1}};

  const FakeRecord record = MakeNonResidentDataRecord(
      NTFS_BROWSER_NAMESPACE::Data::StdInfoPermission::Archive |
          NTFS_BROWSER_NAMESPACE::Data::StdInfoPermission::Compressed,
      compression_unit_size_shift, 2ULL * compression_unit_size, runs);

  return BuildCompressionImage(
      record, runs,
      MakeUncompressedLznt1Chunk(
          CompressionFixturePattern(short_decompressed_unit_size)));
}

std::vector<BYTE> BuildFakeNtfsImageWithCompressedAttrMissingCompressedSize() {
  const std::vector<FakeDataRun> runs{{compressed_data_lcn, 1},
                                      {{}, compression_unit_clusters - 1}};

  const FakeRecord record = MakeNonResidentDataRecord(
      NTFS_BROWSER_NAMESPACE::Data::StdInfoPermission::Archive |
          NTFS_BROWSER_NAMESPACE::Data::StdInfoPermission::Compressed,
      compression_unit_size_shift, xca_lznt1_example_decompressed_size, runs,
      {.total_size = compressed_attr_truncated_total_size});

  const std::vector<BYTE> cluster_bytes(xca_lznt1_example_compressed.begin(),
                                        xca_lznt1_example_compressed.end());
  return BuildCompressionImage(record, runs, cluster_bytes);
}

std::vector<BYTE> BuildFakeNtfsImageWithCompUnitSizeOutOfRange() {
  const std::vector<FakeDataRun> runs{{compressed_data_lcn, 1},
                                      {{}, compression_unit_clusters - 1}};

  const FakeRecord record = MakeNonResidentDataRecord(
      NTFS_BROWSER_NAMESPACE::Data::StdInfoPermission::Archive |
          NTFS_BROWSER_NAMESPACE::Data::StdInfoPermission::Compressed,
      comp_unit_size_out_of_range_shift, xca_lznt1_example_decompressed_size,
      runs);

  const std::vector<BYTE> cluster_bytes(xca_lznt1_example_compressed.begin(),
                                        xca_lznt1_example_compressed.end());
  return BuildCompressionImage(record, runs, cluster_bytes);
}

std::vector<BYTE> BuildFakeNtfsImageWithOversizedCompressionUnit() {
  const std::vector<FakeDataRun> runs{{compressed_data_lcn, 1},
                                      {{}, compression_unit_clusters - 1}};

  const FakeRecord record = MakeNonResidentDataRecord(
      NTFS_BROWSER_NAMESPACE::Data::StdInfoPermission::Archive |
          NTFS_BROWSER_NAMESPACE::Data::StdInfoPermission::Compressed,
      oversized_comp_unit_size_shift, xca_lznt1_example_decompressed_size,
      runs);

  const std::vector<BYTE> cluster_bytes(xca_lznt1_example_compressed.begin(),
                                        xca_lznt1_example_compressed.end());
  return BuildCompressionImage(record, runs, cluster_bytes);
}

std::vector<BYTE> BuildFakeNtfsImageWithMisalignedCompressedStartVcn() {
  const std::vector<FakeDataRun> runs{{compressed_data_lcn, 1},
                                      {{}, compression_unit_clusters - 1}};

  const FakeRecord record = MakeNonResidentDataRecord(
      NTFS_BROWSER_NAMESPACE::Data::StdInfoPermission::Archive |
          NTFS_BROWSER_NAMESPACE::Data::StdInfoPermission::Compressed,
      compression_unit_size_shift, xca_lznt1_example_decompressed_size, runs,
      {.start_vcn = misaligned_compressed_start_vcn});

  const std::vector<BYTE> cluster_bytes(xca_lznt1_example_compressed.begin(),
                                        xca_lznt1_example_compressed.end());
  return BuildCompressionImage(record, runs, cluster_bytes);
}

namespace {

// Root image whose compressed $DATA maps "runs". Only the record is written:
// the runs sit at overflowing_lcn, so their clusters have nowhere to go.
std::vector<BYTE>
    BuildRootWithCompressedDataRuns(const std::vector<FakeDataRun>& runs) {
  const FakeRecord record = MakeNonResidentDataRecord(
      NTFS_BROWSER_NAMESPACE::Data::StdInfoPermission::Archive |
          NTFS_BROWSER_NAMESPACE::Data::StdInfoPermission::Compressed,
      compression_unit_size_shift, compression_unit_size, runs);

  std::vector<BYTE> image = BuildFakeNtfsImage();
  const DWORD mft_addr = static_cast<DWORD>(mft_lcn) * cluster_size;
  const size_t root_offset =
      mft_addr + static_cast<size_t>(fake_file_record_size) *
                     static_cast<size_t>(Mft::Idx::Root);
  std::memcpy(&image.at(root_offset), record.data(), record.size());
  return image;
}

}  // namespace

std::vector<BYTE> BuildFakeNtfsImageWithStoredCompressionUnitBadLcn() {
  return BuildRootWithCompressedDataRuns(
      {{overflowing_lcn, compression_unit_clusters}});
}

std::vector<BYTE> BuildFakeNtfsImageWithCompressedCompressionUnitBadLcn() {
  return BuildRootWithCompressedDataRuns(
      {{overflowing_lcn, 1}, {{}, compression_unit_clusters - 1}});
}

}  // namespace NTFS_BROWSER_NAMESPACE::Tests
