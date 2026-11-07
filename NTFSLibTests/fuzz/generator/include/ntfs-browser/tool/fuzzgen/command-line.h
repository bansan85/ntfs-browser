#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <ostream>
#include <string>
#include <vector>

#include <ntfs-browser/namespace.h>

namespace NTFS_BROWSER_NAMESPACE::Tool::FuzzGen {

// What the command line asks for.
struct Options {
  enum class Mode : std::uint8_t {
    // Nothing asked: print the usage.
    Help,
    // --list: print the names of the scenarios.
    List,
    // --out DIR: write the files under DIR.
    Out,
    // --verify DIR: compare the files with the ones under DIR.
    Verify
  };

  Mode mode = Mode::Help;
  // The folder of --out or --verify.
  std::filesystem::path folder;
  // The folder of the extracted resource archives (--resources).
  std::filesystem::path resources;
  // The scenarios named by --only. Empty means all of them.
  std::vector<std::string> only;
};

// Reads the command line of NtfsFuzzGen.
class CommandLine {
 public:
  // args[0] is the program name. Returns nullopt, after a message on err, when
  // the command line is wrong.
  [[nodiscard]] static std::optional<Options>
      Parse(const std::vector<std::filesystem::path>& args, std::ostream& err);

  // Prints the usage.
  static void Usage(std::ostream& out);
};

}  // namespace NTFS_BROWSER_NAMESPACE::Tool::FuzzGen
