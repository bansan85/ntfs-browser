#include "mft-fixture-scenarios.h"

#include <ntfs-browser/win-types.h>

#include <vector>

#include <ntfs-browser/namespace.h>
#include <ntfs-browser/test/fake-image/fake-ntfs-image.h>
#include <ntfs-browser/tool/fuzzgen/recorded-stream.h>
#include <ntfs-browser/tool/fuzzgen/scenario-registry.h>

#include "bpb-edit.h"
#include "data/ntfs-bpb.h"
#include "legacy-fixtures.h"
#include "record-edit.h"

namespace NTFS_BROWSER_NAMESPACE::Tool::FuzzGen {

namespace {

namespace FakeImage = Test::FakeImage;
using Boot = LegacyFixtures::Boot;

// Record numbers of the $MFT, and of the records the damaged names input
// edits.
constexpr ULONGLONG mft_record = 0;
constexpr ULONGLONG short_name_record = 16;
constexpr ULONGLONG resident_name_record = 17;
constexpr ULONGLONG moved_name_record = 18;

// The cluster of the $MFT in the fake volumes, which the mirror then shares.
constexpr ULONGLONG mft_cluster = 1;

// The start VCN a first extent cannot have: the $MFT has no extent that starts
// at 0.
constexpr ULONGLONG start_vcn_past_zero = 1;

// The parent the moved name claims: a record that is not the root.
constexpr ULONGLONG moved_parent_record = 16;
constexpr WORD moved_parent_sequence = 1;

// The $MFT with a first extent that does not start at VCN 0.
std::vector<BYTE> WithoutBaseExtent() {
  std::vector<BYTE> image = FakeImage::BuildFakeNtfsImage();
  RecordEdit::SetStartVcn(
      image, RecordEdit::Attribute(image, mft_record, RecordEdit::type_data),
      start_vcn_past_zero);
  return image;
}

// The $MFT whose mirror points at the $MFT itself, and whose record 0 cannot be
// read.
std::vector<BYTE> MirrorSameAsMft() {
  std::vector<BYTE> image = FakeImage::BuildFakeNtfsImage();
  BpbEdit::Apply(image,
                 [](Data::NtfsBpb& bpb) { bpb.lcn_mft_mirr = mft_cluster; });
  RecordEdit::BreakMagic(image, mft_record);
  return image;
}

// The $MFT copy is sound and record 0 of the $MFT is not.
std::vector<BYTE> Record0CorruptMirrorIntact() {
  return FakeImage::BuildFakeNtfsImageWithMftMirror({.in_mft = 1});
}

// Three records of the extension image with one name field each wrong: a name
// shorter than its length says, a name that is not resident, and a second link
// to a directory that is not the root.
std::vector<BYTE> NamesDamaged() {
  std::vector<BYTE> image =
      FakeImage::BuildFakeNtfsImageWithMftExtensionRecord();
  RecordEdit::SetFileNameLength(
      image,
      RecordEdit::Attribute(image, short_name_record,
                            RecordEdit::type_file_name),
      0);
  RecordEdit::SetNonResidentFlag(
      image,
      RecordEdit::Attribute(image, resident_name_record,
                            RecordEdit::type_file_name),
      1);
  RecordEdit::SetFileNameParent(
      image,
      RecordEdit::Attribute(image, moved_name_record,
                            RecordEdit::type_file_name),
      moved_parent_record, moved_parent_sequence);
  return image;
}

}  // namespace

void MftFixtureScenarios::Register(ScenarioRegistry& registry) {
  const NTFS_BROWSER_NAMESPACE::VolumeOptions strict{};
  LegacyFixtures::Register(
      registry,
      {
          {.name = "mft_data_extent_chain",
           .make = FakeImage::BuildFakeNtfsImageWithMftDataExtentChain},
          {.name = "mft_data_without_base_extent", .make = WithoutBaseExtent},
          {.name = "mft_mirror_same_as_mft",
           .make = MirrorSameAsMft,
           .boot = Boot::Current},
          {.name = "mft_record0_corrupt_mirror_intact",
           .make = Record0CorruptMirrorIntact,
           .boot = Boot::Current},
          {.name = "mft_tree_full_cache",
           .make = FakeImage::BuildFakeNtfsImageWithMftTree,
           .boot = Boot::PatchedTotalSectors,
           .options = strict,
           .probe = RecordedStream::Probe::FullCache},
          {.name = "mft_tree_huge_real_size",
           .make = FakeImage::BuildFakeNtfsImageWithHugeMftRealSize},
          {.name = "mft_tree_names_damaged_record_and_duplicate_link",
           .make = NamesDamaged},
      });
}

}  // namespace NTFS_BROWSER_NAMESPACE::Tool::FuzzGen
