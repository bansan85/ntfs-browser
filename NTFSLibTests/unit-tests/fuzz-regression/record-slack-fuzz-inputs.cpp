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

// The inputs, one per way a hostile slack can be wrong.
std::vector<Variant> Variants() {
  const auto record_entry = SlackInputs::RecordEntry(L"dense.txt");
  return {{"record_slack_dense_names",
           SlackInputs::ImageWithRecordSlack(SlackInputs::DenseNames(
               record_entry, SlackInputs::record_slack_room))},
          {"record_slack_name_runs_past_record",
           SlackInputs::ImageWithRecordSlack(SlackInputs::NameRunsPastTheEnd(
               record_entry, SlackInputs::record_slack_room))},
          {"record_slack_stale_terminators",
           SlackInputs::ImageWithRecordSlack(SlackInputs::StaleTerminators())}};
}

}  // namespace

// Writes the saved fuzz inputs of the record slack: the byte stream that
// Test::Fuzz::ProbeOnce() reads from the image, in read order and without
// addresses. The volume is opened recovering with deleted items shown, since
// only then does a scan carve the slack. Set NTFS_BROWSER_FUZZ_INPUT_DIR to the
// folder to write them in (the repository's NTFSLibTests/fuzz/data) and run
// this hidden test case by its tag.
TEST_CASE("write the record slack fuzz inputs", "[.write-fuzz-inputs]") {
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
