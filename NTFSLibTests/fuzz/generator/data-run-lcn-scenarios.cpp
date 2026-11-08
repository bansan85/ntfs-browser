#include "data-run-lcn-scenarios.h"

#include <ntfs-browser/win-types.h>

#include <vector>

#include <ntfs-browser/namespace.h>
#include <ntfs-browser/test/fake-image/fake-ntfs-image.h>
#include <ntfs-browser/tool/fuzzgen/scenario-registry.h>

#include "hex-bytes.h"
#include "legacy-fixtures.h"
#include "legacy-geometry.h"
#include "record-edit.h"
#include "root-index-edit.h"

namespace NTFS_BROWSER_NAMESPACE::Tool::FuzzGen {

namespace {

namespace FakeImage = Test::FakeImage;

using Image = std::vector<BYTE>;

// The root directory record and the $Volume record.
constexpr ULONGLONG root_record = 5;
constexpr ULONGLONG volume_record = 3;

// The type code of $STANDARD_INFORMATION.
constexpr DWORD type_standard_information = 0x10;

// A $STANDARD_INFORMATION of NTFS 3: the four times, the archive flag, and
// zeros up to 72 bytes.
constexpr const char* standard_information_v3 =
    "08 07 06 05 04 03 02 01 18 17 16 15 14 13 12 11 28 27 26 25 24 23 22 21 "
    "38 37 36 35 34 33 32 31 20 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 "
    "00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00";

// The run list that makes the sum of two LCNs overflow: two runs of one
// cluster, each at the largest signed LCN, and the end byte. The allocation
// spans VCN 0 and 1, two clusters of the volume.
constexpr const char* lcn_sum_runs =
    "81 01 FF FF FF FF FF FF FF 7F 81 01 FF FF FF FF FF FF FF 7F 00";
constexpr ULONGLONG lcn_sum_last_vcn = 1;
constexpr ULONGLONG lcn_sum_size = 0x800;

// A run of one cluster at LCN 2^54: its byte address, a multiple of the 1 KiB
// cluster, is 2^64 and wraps to zero. The allocation spans one cluster.
constexpr const char* lcn_product_runs = "81 01 00 00 00 00 00 00 40 00 00";
constexpr ULONGLONG lcn_product_last_vcn = 0;
constexpr ULONGLONG lcn_product_size = 0x400;

// The root of the first fake images: the standard information, an index with
// one sub-node entry for VCN 0, and an index allocation whose run list is
// runs. The $Volume record has the form of the early builders.
Image RootWithAllocation(const char* runs, ULONGLONG last_vcn, ULONGLONG size) {
  Image image = FakeImage::BuildFakeNtfsImage();
  LegacyGeometry::PackLastAttribute(image, volume_record);
  RecordEdit::ClearAttributes(image, root_record);
  RecordEdit::AppendResident(image, root_record, type_standard_information, 0,
                             HexBytes::Decode(standard_information_v3));
  RootIndexEdit::Put(image, {.sub_node_vcn = 0, .keep_attributes = true});
  RootIndexEdit::PutAllocation(image, HexBytes::Decode(runs), last_vcn, size);
  return image;
}

}  // namespace

void DataRunLcnScenarios::Register(ScenarioRegistry& registry) {
  LegacyFixtures::Register(
      registry, {
                    {.name = "data_run_lcn_sum_overflows",
                     .make =
                         [] {
                           return RootWithAllocation(
                               lcn_sum_runs, lcn_sum_last_vcn, lcn_sum_size);
                         }},
                    {.name = "data_run_lcn_product_wraps",
                     .make =
                         [] {
                           return RootWithAllocation(lcn_product_runs,
                                                     lcn_product_last_vcn,
                                                     lcn_product_size);
                         }},
                });
}

}  // namespace NTFS_BROWSER_NAMESPACE::Tool::FuzzGen
