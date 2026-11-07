#include <ntfs-browser/win-types.h>

#include <cstddef>
#include <vector>

#include <ntfs-browser/namespace.h>
#include <ntfs-browser/test/fake-image/fake-ntfs-image.h>
#include <ntfs-browser/tool/fuzzgen/mft-scan-scenarios.h>
#include <ntfs-browser/tool/fuzzgen/recorded-stream.h>
#include <ntfs-browser/tool/fuzzgen/scenario-registry.h>

namespace NTFS_BROWSER_NAMESPACE::Tool::FuzzGen {

namespace {

namespace FakeImage = Test::FakeImage;

// Byte offset of the $MFT in the fake volumes: their $MFT starts at cluster 1.
constexpr std::size_t mft_offset = FakeImage::fake_cluster_size;

// Bytes in a sector of a fake record.
constexpr std::size_t sector_bytes = FakeImage::fake_bytes_per_sector;

// Bytes of the update sequence check word, at the end of each sector.
constexpr std::size_t check_word_bytes = 2;

// A value that is not the update sequence number of any fake record.
constexpr BYTE wrong_check_byte = 0xEE;

// The records of the fixture that get a damaged sector: one on the first
// sector, one on the second, one on both.
constexpr ULONGLONG first_sector_victim = FakeImage::mft_tree_docs_idx;
constexpr ULONGLONG second_sector_victim = FakeImage::mft_tree_report_idx;
constexpr ULONGLONG both_sectors_victim = FakeImage::mft_tree_deleted_file_idx;

// Records and clusters per fragment of the fragmented $MFT: enough records to
// cycle through all the kinds the fixture makes, in several fragments.
constexpr ULONGLONG fragmented_record_count = 40;
constexpr ULONGLONG fragmented_fragment_clusters = 8;

// Overwrites the check word of one sector of record idx.
void BreakSector(std::vector<BYTE>& image, ULONGLONG idx, std::size_t sector) {
  const std::size_t end = mft_offset +
                          (idx * FakeImage::fake_file_record_size) +
                          ((sector + 1) * sector_bytes);
  for (std::size_t i = 1; i <= check_word_bytes; ++i) {
    image.at(end - i) = wrong_check_byte;
  }
}

// The freed tree with records whose update sequence check fails on one sector,
// on the other, and on both: a recovering volume salvages the first two and
// rejects the third.
std::vector<BYTE> BrokenSectors() {
  std::vector<BYTE> image = FakeImage::BuildFakeNtfsImageWithFreedFileTree();
  BreakSector(image, first_sector_victim, 0);
  BreakSector(image, second_sector_victim, 1);
  BreakSector(image, both_sectors_victim, 0);
  BreakSector(image, both_sectors_victim, 1);
  return image;
}

}  // namespace

void MftScanScenarios::Register(ScenarioRegistry& registry) {
  // Opened recovering with deleted items shown: only then does a scan salvage a
  // record whose check fails on some sectors.
  const auto options = RecordedStream::Recovering();
  registry.AddRecorded("mft_scan_broken_sectors", BrokenSectors, options);
  registry.AddRecorded(
      "mft_scan_fragmented",
      [] {
        return FakeImage::BuildFakeNtfsImageWithFragmentedMftScan(
            fragmented_record_count, fragmented_fragment_clusters);
      },
      options);
}

}  // namespace NTFS_BROWSER_NAMESPACE::Tool::FuzzGen
