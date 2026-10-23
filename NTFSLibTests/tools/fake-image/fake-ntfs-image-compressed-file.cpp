#include <algorithm>
#include <array>
#include <cstring>
#include <optional>
#include <span>
#include <vector>

#include <ntfs-browser/mft/idx.h>
#include <ntfs-browser/namespace.h>

#include "data/std-info-permission.h"
#include "fake-ntfs-image-internal.h"
#include "fake-ntfs-image.h"
#include "ntfs-browser/attr/type.h"
#include "ntfs-browser/win-types.h"

namespace NTFS_BROWSER_NAMESPACE::Tests {

namespace {

// A bitmap byte with every bit set, and the mask and shift that split a
// WORD into bytes.
constexpr BYTE all_bits_set = 0xFF;

// The permission bits of a compressed file.
NTFS_BROWSER_NAMESPACE::Data::StdInfoPermission CompressedPermission() {
  return NTFS_BROWSER_NAMESPACE::Data::StdInfoPermission::Archive |
         NTFS_BROWSER_NAMESPACE::Data::StdInfoPermission::Compressed;
}

// The [MS-XCA] section 3.3 worked example's 59 compressed bytes.
std::vector<BYTE> XcaExampleBytes() {
  return std::vector<BYTE>(xca_lznt1_example_compressed.begin(),
                           xca_lznt1_example_compressed.end());
}

// Root image whose compressed $DATA holds one real cluster, cluster_bytes,
// then a sparse pad up to a whole compression unit. 1 <
// compression_unit_clusters real clusters is exactly what marks a unit as
// compressed rather than stored. comp_unit_size and overrides forge the header.
std::vector<BYTE> BuildOneClusterCompressedImage(
    WORD comp_unit_size, const std::vector<BYTE>& cluster_bytes,
    const FakeNonResidentOverrides& overrides = {}) {
  const std::vector<FakeDataRun> runs{{compressed_data_lcn, 1},
                                      {{}, compression_unit_clusters - 1}};

  const FakeRecord record = MakeNonResidentDataRecord(
      CompressedPermission(), comp_unit_size,
      xca_lznt1_example_decompressed_size, runs, overrides);

  return BuildCompressionImage(record, runs, cluster_bytes);
}

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
  return BuildOneClusterCompressedImage(compression_unit_size_shift,
                                        XcaExampleBytes());
}

std::vector<BYTE> BuildFakeNtfsImageWithStoredCompressionUnit() {
  // Real runs covering the whole unit, no sparse pad: a stored
  // (incompressible) unit, whose bytes must come back untouched.
  const std::vector<FakeDataRun> runs{
      {compressed_data_lcn, compression_unit_clusters}};

  const FakeRecord record = MakeNonResidentDataRecord(
      CompressedPermission(), compression_unit_size_shift,
      compression_unit_size, runs);

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

  FakeRecord record = MakeInUseFileRecord();
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
      CompressedPermission(), compression_unit_size_shift,
      sparse_then_compressed_real_size, runs);

  return BuildCompressionImage(record, runs, XcaExampleBytes());
}

std::vector<BYTE> BuildFakeNtfsImageWithHoleBeforeRealCluster() {
  const std::vector<FakeDataRun> runs{
      {{}, 1}, {compressed_data_lcn, 1}, {{}, 2}};

  const FakeRecord record = MakeNonResidentDataRecord(
      CompressedPermission(), compression_unit_size_shift,
      compression_unit_size, runs);

  return BuildCompressionImage(record, runs, XcaExampleBytes());
}

std::vector<BYTE> BuildFakeNtfsImageWithFragmentedCompressedFile() {
  // Two non-contiguous single-cluster real runs (so the compressed bytes
  // genuinely span two Data::RunEntrys) plus a 2-cluster sparse pad.
  const std::vector<FakeDataRun> runs{{compressed_data_lcn, 1},
                                      {fragmented_compressed_data_lcn, 1},
                                      {{}, compression_unit_clusters - 2}};

  const FakeRecord record = MakeNonResidentDataRecord(
      CompressedPermission(), compression_unit_size_shift,
      fragmented_compressed_payload_size, runs);

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
      CompressedPermission(), compression_unit_size_shift,
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
  return BuildOneClusterCompressedImage(compression_unit_size_shift,
                                        MakeCorruptLznt1Chunk());
}

std::vector<BYTE> BuildFakeNtfsImageWithUnmappedCompressionUnit() {
  // Only unit 0's clusters are actually mapped; last_vcn claims two units'
  // worth.
  const std::vector<FakeDataRun> runs{
      {compressed_data_lcn, compression_unit_clusters}};

  const FakeRecord record = MakeNonResidentDataRecord(
      CompressedPermission(), compression_unit_size_shift,
      2ULL * compression_unit_size, runs,
      {.last_vcn = 2ULL * compression_unit_clusters - 1});

  return BuildCompressionImage(
      record, runs, CompressionFixturePattern(compression_unit_size));
}

std::vector<BYTE> BuildFakeNtfsImageWithRealClustersAfterHole() {
  const std::vector<FakeDataRun> runs{
      {compressed_data_lcn, 1}, {{}, 1}, {fragmented_compressed_data_lcn, 2}};

  const FakeRecord record = MakeNonResidentDataRecord(
      CompressedPermission(), compression_unit_size_shift,
      compression_unit_size, runs);

  // Content is irrelevant - the layout is rejected before anything is
  // decompressed - but a real LZNT1 stream keeps the fixture honest about
  // being otherwise plausible.
  return BuildCompressionImage(record, runs, XcaExampleBytes());
}

std::vector<BYTE> BuildFakeNtfsImageWithShortDecompressedUnit() {
  // One real cluster then seven sparse: unit 0 (VCN 0..3) is compressed,
  // unit 1 (VCN 4..7) a hole. real_size covers both, so unit 0 is interior
  // and must yield a whole compression_unit_size bytes.
  const std::vector<FakeDataRun> runs{
      {compressed_data_lcn, 1}, {{}, 2ULL * compression_unit_clusters - 1}};

  const FakeRecord record = MakeNonResidentDataRecord(
      CompressedPermission(), compression_unit_size_shift,
      2ULL * compression_unit_size, runs);

  return BuildCompressionImage(
      record, runs,
      MakeUncompressedLznt1Chunk(
          CompressionFixturePattern(short_decompressed_unit_size)));
}

std::vector<BYTE> BuildFakeNtfsImageWithCompressedAttrMissingCompressedSize() {
  return BuildOneClusterCompressedImage(
      compression_unit_size_shift, XcaExampleBytes(),
      {.total_size = compressed_attr_truncated_total_size});
}

std::vector<BYTE> BuildFakeNtfsImageWithCompUnitSizeOutOfRange() {
  return BuildOneClusterCompressedImage(comp_unit_size_out_of_range_shift,
                                        XcaExampleBytes());
}

std::vector<BYTE> BuildFakeNtfsImageWithOversizedCompressionUnit() {
  return BuildOneClusterCompressedImage(oversized_comp_unit_size_shift,
                                        XcaExampleBytes());
}

std::vector<BYTE> BuildFakeNtfsImageWithMisalignedCompressedStartVcn() {
  return BuildOneClusterCompressedImage(
      compression_unit_size_shift, XcaExampleBytes(),
      {.start_vcn = misaligned_compressed_start_vcn});
}

namespace {

// Root image whose compressed $DATA maps "runs". Only the record is written:
// the runs sit at overflowing_lcn, so their clusters have nowhere to go.
std::vector<BYTE>
    BuildRootWithCompressedDataRuns(const std::vector<FakeDataRun>& runs) {
  const FakeRecord record = MakeNonResidentDataRecord(
      CompressedPermission(), compression_unit_size_shift,
      compression_unit_size, runs);

  std::vector<BYTE> image = BuildFakeNtfsImage();
  PutRootRecord(image, record);
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
