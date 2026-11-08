#include "record-slack-scenarios.h"

#include <ntfs-browser/win-types.h>

#include <vector>

#include <ntfs-browser/namespace.h>
#include <ntfs-browser/test/fake-image/slack-inputs.h>
#include <ntfs-browser/tool/fuzzgen/recorded-stream.h>
#include <ntfs-browser/tool/fuzzgen/scenario-registry.h>

namespace NTFS_BROWSER_NAMESPACE::Tool::FuzzGen {

namespace {

using Test::FakeImage::SlackInputs;

// Dense names, and a name that runs past the end of the record.
std::vector<BYTE> DenseNames() {
  return SlackInputs::ImageWithRecordSlack(SlackInputs::DenseNames(
      SlackInputs::RecordEntry(L"dense.txt"), SlackInputs::record_slack_room));
}

std::vector<BYTE> NameRunsPastTheRecord() {
  return SlackInputs::ImageWithRecordSlack(SlackInputs::NameRunsPastTheEnd(
      SlackInputs::RecordEntry(L"keeper.txt"), SlackInputs::record_slack_room));
}

std::vector<BYTE> StaleTerminators() {
  return SlackInputs::ImageWithRecordSlack(SlackInputs::StaleTerminators());
}

}  // namespace

void RecordSlackScenarios::Register(ScenarioRegistry& registry) {
  // Opened recovering with deleted items shown: only then does a scan carve the
  // slack.
  const auto options = RecordedStream::Recovering();
  registry.AddRecorded("record_slack_dense_names", DenseNames, options);
  registry.AddRecorded("record_slack_name_runs_past_record",
                       NameRunsPastTheRecord, options);
  registry.AddRecorded("record_slack_stale_terminators", StaleTerminators,
                       options);
}

}  // namespace NTFS_BROWSER_NAMESPACE::Tool::FuzzGen
