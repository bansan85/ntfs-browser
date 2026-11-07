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
  const auto block_entry = SlackInputs::BlockEntry(L"dense.txt");
  return {
      {"index_slack_dense_names",
       SlackInputs::ImageWithBlockSlack(SlackInputs::DenseNames(
           block_entry, SlackInputs::index_slack_room))},
      {"index_slack_name_runs_past_block",
       SlackInputs::ImageWithBlockSlack(SlackInputs::NameRunsPastTheEnd(
           block_entry, SlackInputs::index_slack_room))},
      {"index_slack_damaged_blocks", SlackInputs::ImageWithDamagedBlocks()}};
}

}  // namespace

// Writes the saved fuzz inputs of the index slack: the byte stream that
// Test::Fuzz::ProbeOnce() reads from the image, in read order and without
// addresses. The volume is opened recovering with deleted items shown, since
// only then does a scan carve the slack. Set NTFS_BROWSER_FUZZ_INPUT_DIR to the
// folder to write them in (the repository's NTFSLibTests/fuzz/data) and run
// this hidden test case by its tag.
TEST_CASE("write the index slack fuzz inputs", "[.write-fuzz-inputs]") {
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
