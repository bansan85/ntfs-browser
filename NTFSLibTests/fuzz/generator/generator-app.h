#pragma once

#include <ostream>
#include <string>
#include <vector>

#include <ntfs-browser/namespace.h>
#include <ntfs-browser/tool/fuzzgen/scenario-registry.h>
#include <ntfs-browser/tool/fuzzgen/scenario.h>

#include "command-line.h"

namespace NTFS_BROWSER_NAMESPACE::Tool::FuzzGen {

// Runs what the command line asks for over a registry of scenarios.
class GeneratorApp {
 public:
  // out receives the results, err the problems. Both MUST outlive the app.
  GeneratorApp(const ScenarioRegistry& registry, std::ostream& out,
               std::ostream& err);

  // Runs options. Returns the exit code: 0 on success, 1 when a scenario
  // fails, a verified file differs or is missing, or a name is unknown.
  [[nodiscard]] int Run(const Options& options) const;

 private:
  // The scenarios that options.only names, or all of them. Returns false, with
  // a message on err, for a name that no scenario has.
  [[nodiscard]] bool Select(const Options& options,
                            std::vector<const Scenario*>& selected) const;

  // Writes the bytes that the layers file of options gives for each range of
  // the intervals file. Returns the exit code.
  [[nodiscard]] int Predict(const Options& options) const;

  // Prints the name of each scenario, sorted.
  void List(const std::vector<const Scenario*>& selected) const;

  // Writes each scenario under options.folder. Returns the exit code.
  [[nodiscard]] int
      Generate(const Options& options,
               const std::vector<const Scenario*>& selected) const;

  // Compares each scenario with the file of the same name under
  // options.folder, and counts the files that no scenario makes. Returns the
  // exit code.
  [[nodiscard]] int Verify(const Options& options,
                           const std::vector<const Scenario*>& selected) const;

  const ScenarioRegistry* registry_;
  std::ostream* out_;
  std::ostream* err_;
};

}  // namespace NTFS_BROWSER_NAMESPACE::Tool::FuzzGen
