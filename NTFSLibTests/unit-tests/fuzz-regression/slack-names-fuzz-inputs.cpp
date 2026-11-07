#include <ntfs-browser/win-types.h>

#include <cstdlib>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include <ntfs-browser/namespace.h>
#include <ntfs-browser/test/fake-image/slack-inputs.h>
#include <ntfs-browser/volume-options.h>

#include "fuzz-input-writer.h"

namespace TestFuzzRegression = NTFS_BROWSER_NAMESPACE::Test::FuzzRegression;
using NTFS_BROWSER_NAMESPACE::VolumeOptions;
using NTFS_BROWSER_NAMESPACE::Test::FakeImage::SlackInputs;

namespace {

// One saved fuzz input: its file name, and the image it was recorded from.
struct Variant {
  std::string name;
  std::vector<BYTE> image;
};

// The inputs, one per way a leftover can repeat or mislead.
std::vector<Variant> Variants() {
  return {
      {"record_slack_name_units",
       SlackInputs::ImageWithRecordSlack(SlackInputs::NameUnits())},
      {"record_slack_rejected_fields",
       SlackInputs::ImageWithRecordSlack(SlackInputs::RejectedFields())},
      {"record_slack_repeated_names",
       SlackInputs::ImageWithRecordSlack(SlackInputs::RepeatedRecordNames())},
      {"record_slack_live_names",
       SlackInputs::ImageWithRecordSlack(SlackInputs::LiveNames())},
      {"index_slack_repeated_names",
       SlackInputs::ImageWithBlockSlack(SlackInputs::RepeatedBlockNames())}};
}

}  // namespace

// Writes the saved fuzz inputs of names carved twice or carved from bytes that
// are not a name. The volume is opened recovering with deleted items shown,
// since only then does a scan carve the slack. Set NTFS_BROWSER_FUZZ_INPUT_DIR
// to the folder to write them in (the repository's NTFSLibTests/fuzz/data) and
// run this hidden test case by its tag.
TEST_CASE("write the slack names fuzz inputs", "[.write-fuzz-inputs]") {
  const char* const dir = std::getenv("NTFS_BROWSER_FUZZ_INPUT_DIR");
  if (dir == nullptr) {
    SKIP("NTFS_BROWSER_FUZZ_INPUT_DIR is not set");
  }

  const VolumeOptions recovering{.include_deleted = true,
                                 .recover_errors = true};
  for (Variant& variant : Variants()) {
    TestFuzzRegression::WriteRecordedInput(
        std::filesystem::path(dir) / variant.name, std::move(variant.image),
        recovering);
  }
}
