#include "index-slack-scenarios.h"

#include <ntfs-browser/win-types.h>

#include <vector>

#include <ntfs-browser/namespace.h>
#include <ntfs-browser/test/fake-image/slack-inputs.h>
#include <ntfs-browser/tool/fuzzgen/recorded-stream.h>
#include <ntfs-browser/tool/fuzzgen/scenario-registry.h>

namespace NTFS_BROWSER_NAMESPACE::Tool::FuzzGen {

namespace {

using Test::FakeImage::SlackInputs;

// Dense names, and a name that runs past the end of the block.
std::vector<BYTE> DenseNames() {
  return SlackInputs::ImageWithBlockSlack(SlackInputs::DenseNames(
      SlackInputs::BlockEntry(L"dense.txt"), SlackInputs::index_slack_room));
}

std::vector<BYTE> NameRunsPastTheBlock() {
  return SlackInputs::ImageWithBlockSlack(SlackInputs::NameRunsPastTheEnd(
      SlackInputs::BlockEntry(L"keeper.txt"), SlackInputs::index_slack_room));
}

}  // namespace

void IndexSlackScenarios::Register(ScenarioRegistry& registry) {
  // Opened recovering with deleted items shown: only then does a scan carve the
  // slack.
  const auto options = RecordedStream::Recovering();
  registry.AddRecorded("index_slack_dense_names", DenseNames, options);
  registry.AddRecorded("index_slack_name_runs_past_block", NameRunsPastTheBlock,
                       options);
  registry.AddRecorded("index_slack_damaged_blocks",
                       SlackInputs::ImageWithDamagedBlocks, options);
}

}  // namespace NTFS_BROWSER_NAMESPACE::Tool::FuzzGen
