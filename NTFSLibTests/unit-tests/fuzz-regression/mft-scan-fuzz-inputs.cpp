#include <ntfs-browser/win-types.h>

#include <cstdlib>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include <ntfs-browser/namespace.h>
#include <ntfs-browser/volume-options.h>

#include "fake-ntfs-image.h"
#include "fuzz-input-writer.h"

namespace Tests = NTFS_BROWSER_NAMESPACE::Tests;
using NTFS_BROWSER_NAMESPACE::VolumeOptions;

namespace {

// One saved fuzz input: its file name, and the image it was recorded from.
struct Variant {
  std::string name;
  std::vector<BYTE> image;
};

// Byte offset of the $MFT in the fake volumes: their $MFT starts at cluster 1.
constexpr size_t mft_offset = Tests::fake_cluster_size;

// Bytes in a sector of a fake record, and the two bytes at the end of each one:
// the update sequence check word.
constexpr size_t sector_bytes = Tests::fake_bytes_per_sector;
constexpr size_t check_word_bytes = 2;

// A value that is not the update sequence number of any fake record.
constexpr BYTE wrong_check_byte = 0xEE;

// Records of the fixture that get a damaged sector.
constexpr ULONGLONG first_sector_victim = Tests::mft_tree_docs_idx;
constexpr ULONGLONG second_sector_victim = Tests::mft_tree_report_idx;
constexpr ULONGLONG both_sectors_victim = Tests::mft_tree_deleted_file_idx;

// Records and clusters per fragment of the fragmented $MFT: enough records to
// cycle through all the kinds the fixture makes, in several fragments.
constexpr ULONGLONG fragmented_record_count = 40;
constexpr ULONGLONG fragmented_fragment_clusters = 8;

// Overwrites the check word of one sector of record idx.
void BreakSector(std::vector<BYTE>& image, ULONGLONG idx, size_t sector) {
  const size_t end = mft_offset + (idx * Tests::fake_file_record_size) +
                     ((sector + 1) * sector_bytes);
  for (size_t i = 1; i <= check_word_bytes; ++i) {
    image.at(end - i) = wrong_check_byte;
  }
}

// The freed tree with records whose update sequence check fails on one sector,
// on the other, and on both: a recovering volume salvages the first two and
// rejects the third.
std::vector<BYTE> BrokenSectors() {
  std::vector<BYTE> image = Tests::BuildFakeNtfsImageWithFreedFileTree();
  BreakSector(image, first_sector_victim, 0);
  BreakSector(image, second_sector_victim, 1);
  BreakSector(image, both_sectors_victim, 0);
  BreakSector(image, both_sectors_victim, 1);
  return image;
}

// The inputs, one per way a scan meets a damaged or scattered $MFT.
std::vector<Variant> Variants() {
  std::vector<Variant> variants;
  variants.push_back({"mft_scan_broken_sectors", BrokenSectors()});
  variants.push_back(
      {"mft_scan_fragmented",
       Tests::BuildFakeNtfsImageWithFragmentedMftScan(
           fragmented_record_count, fragmented_fragment_clusters)});
  return variants;
}

}  // namespace

// Writes the saved fuzz inputs where a scan meets damaged records. The volume
// is opened recovering with deleted items shown, since only then does a scan
// salvage a record whose check fails on some sectors. Set
// NTFS_BROWSER_FUZZ_INPUT_DIR to the folder to write them in (the repository's
// NTFSLibTests/fuzz/data) and run this hidden test case by its tag.
TEST_CASE("write the mft scan fuzz inputs", "[.write-fuzz-inputs]") {
  const char* const dir = std::getenv("NTFS_BROWSER_FUZZ_INPUT_DIR");
  if (dir == nullptr) {
    SKIP("NTFS_BROWSER_FUZZ_INPUT_DIR is not set");
  }

  const VolumeOptions recovering{.include_deleted = true,
                                 .recover_errors = true};
  for (Variant& variant : Variants()) {
    Tests::FuzzInputs::WriteRecordedInput(std::filesystem::path(dir) /
                                              variant.name,
                                          std::move(variant.image), recovering);
  }
}
