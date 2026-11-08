#pragma once

#include <ntfs-browser/namespace.h>
#include <ntfs-browser/tool/fuzzgen/scenario-registry.h>

namespace NTFS_BROWSER_NAMESPACE::Tool::FuzzGen {

// The inputs where the records of a change journal ($J) are wrong.
class UsnJournalScenarios {
 public:
  // Adds the eight usn_journal_* inputs, except usn_journal_adjacent_stretches
  // (RunListScenarios).
  static void Register(ScenarioRegistry& registry);
};

}  // namespace NTFS_BROWSER_NAMESPACE::Tool::FuzzGen
