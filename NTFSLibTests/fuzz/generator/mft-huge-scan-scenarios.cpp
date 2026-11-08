#include "mft-huge-scan-scenarios.h"

#include <ntfs-browser/win-types.h>

#include <vector>

#include <ntfs-browser/namespace.h>
#include <ntfs-browser/test/fake-image/fake-ntfs-image.h>
#include <ntfs-browser/tool/fuzzgen/scenario-registry.h>

#include "data/header-non-resident.h"
#include "hex-bytes.h"
#include "legacy-fixtures.h"
#include "record-edit.h"

namespace NTFS_BROWSER_NAMESPACE::Tool::FuzzGen {

namespace {

namespace FakeImage = Test::FakeImage;

// The $MFT record.
constexpr ULONGLONG mft_record = 0;

// The $MFT of 4200 clusters of 1 KiB, one record each: 26 clusters at LCN 1,
// where the base volume has its records, and 4174 at LCN 27. The first run
// spells its offset in four bytes.
constexpr const char* huge_mft_runs = "41 1A 01 00 00 00 12 4E 10 1A 00";
constexpr ULONGLONG huge_mft_clusters = 4200;
constexpr ULONGLONG huge_mft_bytes =
    huge_mft_clusters * FakeImage::fake_cluster_size;

// The tree volume, with a $MFT that maps far more records than the image holds.
std::vector<BYTE> HugeMft() {
  std::vector<BYTE> image = FakeImage::BuildFakeNtfsImageWithMftTree();
  const std::size_t data =
      RecordEdit::Attribute(image, mft_record, RecordEdit::type_data);
  RecordEdit::Edit<Data::HeaderNonResident>(image, data, [](auto& header) {
    header.last_vcn = huge_mft_clusters - 1;
    header.alloc_size = huge_mft_bytes;
    header.real_size = huge_mft_bytes;
    header.ini_size = huge_mft_bytes;
  });
  RecordEdit::ReplaceRunList(image, data, HexBytes::Decode(huge_mft_runs));
  return image;
}

}  // namespace

void MftHugeScanScenarios::Register(ScenarioRegistry& registry) {
  LegacyFixtures::Register(
      registry, {{.name = "mft_huge_scan_stops_at_progress", .make = HugeMft}});
}

}  // namespace NTFS_BROWSER_NAMESPACE::Tool::FuzzGen
