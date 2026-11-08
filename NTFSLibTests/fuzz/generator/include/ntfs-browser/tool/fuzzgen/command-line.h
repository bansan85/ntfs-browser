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
    // --out DIR: write every file under DIR: the fuzz inputs, and the corpus
    // traces in DIR/corpus-traces.
    Out,
    // --seeds DIR: write only the fuzz inputs, flat in DIR. They are the seed
    // corpus of AFL and libFuzzer.
    Seeds,
    // --verify DIR: compare the files with the ones under DIR.
    Verify,
    // --predict LAYERS INTERVALS OUT: write what the layers file gives for the
    // byte ranges of the intervals file. For the tool that makes the residue.
    Predict
  };

  Mode mode = Mode::Help;
  // The folder of --out, --seeds or --verify.
  std::filesystem::path folder;
  // The layers file, the intervals file and the output file of --predict.
  std::filesystem::path layers;
  std::filesystem::path intervals;
  std::filesystem::path output;
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
