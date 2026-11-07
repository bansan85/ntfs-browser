#include <ntfs-browser/win-types.h>

#include <vector>

#include <ntfs-browser/namespace.h>
#include <ntfs-browser/test/fake-image/slack-inputs.h>
#include <ntfs-browser/tool/fuzzgen/recorded-stream.h>
#include <ntfs-browser/tool/fuzzgen/scenario-registry.h>
#include <ntfs-browser/tool/fuzzgen/slack-names-scenarios.h>

namespace NTFS_BROWSER_NAMESPACE::Tool::FuzzGen {

namespace {

using Test::FakeImage::SlackInputs;

std::vector<BYTE> NameUnits() {
  return SlackInputs::ImageWithRecordSlack(SlackInputs::NameUnits());
}

std::vector<BYTE> RejectedFields() {
  return SlackInputs::ImageWithRecordSlack(SlackInputs::RejectedFields());
}

std::vector<BYTE> RepeatedRecordNames() {
  return SlackInputs::ImageWithRecordSlack(SlackInputs::RepeatedRecordNames());
}

std::vector<BYTE> LiveNames() {
  return SlackInputs::ImageWithRecordSlack(SlackInputs::LiveNames());
}

std::vector<BYTE> RepeatedBlockNames() {
  return SlackInputs::ImageWithBlockSlack(SlackInputs::RepeatedBlockNames());
}

}  // namespace

void SlackNamesScenarios::Register(ScenarioRegistry& registry) {
  // Opened recovering with deleted items shown: only then does a scan carve the
  // slack.
  const auto options = RecordedStream::Recovering();
  registry.AddRecorded("record_slack_name_units", NameUnits, options);
  registry.AddRecorded("record_slack_rejected_fields", RejectedFields, options);
  registry.AddRecorded("record_slack_repeated_names", RepeatedRecordNames,
                       options);
  registry.AddRecorded("record_slack_live_names", LiveNames, options);
  registry.AddRecorded("index_slack_repeated_names", RepeatedBlockNames,
                       options);
}

}  // namespace NTFS_BROWSER_NAMESPACE::Tool::FuzzGen
