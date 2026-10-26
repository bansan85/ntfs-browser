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

// The inputs, one per way an $ATTRIBUTE_LIST spreads a file over records or
// clusters.
std::vector<Variant> Variants() {
  std::vector<Variant> variants;
  variants.push_back({"attribute_list_scan_both_sides",
                      Tests::BuildFakeNtfsImageWithMftTreeAttributeList()});
  variants.push_back({"attribute_list_scan_foreign_extension",
                      Tests::BuildFakeNtfsImageWithMftTreeAttributeList(
                          Tests::FakeListedExtension::Foreign)});
  variants.push_back(
      {"attribute_list_multi_cluster",
       Tests::BuildFakeNtfsImageWithMultiClusterAttributeList()});
  variants.push_back({"attribute_list_scan_non_resident",
                      Tests::BuildFakeNtfsImageWithMftTreeAttributeList(
                          Tests::FakeListedExtension::Genuine,
                          Tests::FakeListResidency::NonResident)});
  return variants;
}

}  // namespace

// Writes the saved fuzz inputs where a scan waits for the extension records of
// a file, and where a non-resident $ATTRIBUTE_LIST spans clusters. The volume
// is strict, so a foreign extension record rejects its file. Set
// NTFS_BROWSER_FUZZ_INPUT_DIR to the folder to write them in (the repository's
// NTFSLibTests/fuzz/data) and run this hidden test case by its tag.
TEST_CASE("write the attribute list scan fuzz inputs", "[.write-fuzz-inputs]") {
  const char* const dir = std::getenv("NTFS_BROWSER_FUZZ_INPUT_DIR");
  if (dir == nullptr) {
    SKIP("NTFS_BROWSER_FUZZ_INPUT_DIR is not set");
  }

  for (Variant& variant : Variants()) {
    Tests::FuzzInputs::WriteRecordedInput(
        std::filesystem::path(dir) / variant.name, std::move(variant.image),
        VolumeOptions{});
  }
}

// Writes the saved fuzz inputs that make the thread probe of FuzzTestcase()
// scan files with an $ATTRIBUTE_LIST in the second pass: the kinds of slot of
// the list fixture, recorded from the scan itself, since the main probes read
// in another order. One is strict, the other recovers and shows freed records.
// Set NTFS_BROWSER_FUZZ_INPUT_DIR as for the test case above.
TEST_CASE("write the waiting scan fuzz inputs", "[.write-fuzz-inputs]") {
  const char* const dir = std::getenv("NTFS_BROWSER_FUZZ_INPUT_DIR");
  if (dir == nullptr) {
    SKIP("NTFS_BROWSER_FUZZ_INPUT_DIR is not set");
  }

  // Five turns of the kinds of slot, past the system files.
  constexpr ULONGLONG slots = 16 + 14 * 5;
  Tests::FuzzInputs::WriteRecordedScanInput(
      std::filesystem::path(dir) / "scan_threads_waiting_strict",
      Tests::BuildFakeNtfsImageWithListMftScan(slots), VolumeOptions{});
  Tests::FuzzInputs::WriteRecordedScanInput(
      std::filesystem::path(dir) / "scan_threads_waiting_recovering",
      Tests::BuildFakeNtfsImageWithListMftScan(slots),
      VolumeOptions{.include_deleted = true, .recover_errors = true});
}
