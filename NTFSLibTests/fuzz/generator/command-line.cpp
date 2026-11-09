#include <array>
#include <cstddef>
#include <filesystem>
#include <optional>
#include <ostream>
#include <string>
#include <string_view>
#include <vector>

#include <ntfs-browser/namespace.h>
#include <ntfs-browser/tool/fuzzgen/command-line.h>

#ifndef NTFS_FUZZGEN_RESOURCES_DIR
  #define NTFS_FUZZGEN_RESOURCES_DIR ""
#endif

namespace NTFS_BROWSER_NAMESPACE::Tool::FuzzGen {

namespace {

// What a parse function reads and writes. The index points at the option under
// read. A function that takes operands moves it to the last one.
struct ParseState {
  const std::vector<std::filesystem::path>& args;
  std::size_t index;
  Options& options;
  std::ostream& err;
};

// An option that takes a folder and selects a mode.
struct FolderMode {
  std::string_view name;
  Options::Mode mode;
};

// The options of the folder modes. --list is apart: it takes no folder.
constexpr std::array<FolderMode, 3> kFolderModes{{
    {.name = "--out", .mode = Options::Mode::Out},
    {.name = "--seeds", .mode = Options::Mode::Seeds},
    {.name = "--verify", .mode = Options::Mode::Verify},
}};

// How many files --predict takes: layers, intervals and output.
constexpr std::size_t kPredictOperands = 3;

// The message for a second mode, as --list and the folder modes say it.
constexpr std::string_view kOneModeMessage =
    "only one of --list, --out, --seeds and --verify is allowed\n";

// The message for a second mode, as --predict says it. It names --predict too,
// so the two texts differ and the tests check both.
constexpr std::string_view kOneModeOrPredictMessage =
    "only one of --list, --out, --seeds, --verify and --predict is allowed\n";

// True when arg starts an option.
bool IsOption(const std::filesystem::path& arg) {
  return arg.generic_string().starts_with("--");
}

// True when an argument follows the option under read and it is not an option.
bool HasOperand(const ParseState& state) {
  return state.index + 1 < state.args.size() &&
         !IsOption(state.args[state.index + 1]);
}

// Selects the mode. Returns false, with conflict on err, when a mode is
// already selected.
bool ClaimMode(ParseState& state, Options::Mode mode,
               std::string_view conflict) {
  if (state.options.mode != Options::Mode::Help) {
    state.err << conflict;
    return false;
  }
  state.options.mode = mode;
  return true;
}

// Reads a mode option that takes a folder.
bool ParseFolderMode(ParseState& state, Options::Mode mode) {
  if (!ClaimMode(state, mode, kOneModeMessage)) {
    return false;
  }
  if (!HasOperand(state)) {
    state.err << state.args[state.index].string() << " needs a folder\n";
    return false;
  }
  state.options.folder = state.args[++state.index];
  return true;
}

// Reads --predict and its three files. A file MAY look like an option.
bool ParsePredict(ParseState& state) {
  if (!ClaimMode(state, Options::Mode::Predict, kOneModeOrPredictMessage)) {
    return false;
  }
  if (state.index + kPredictOperands >= state.args.size()) {
    state.err << "--predict needs a layers file, an intervals file and an "
                 "output file\n";
    return false;
  }
  state.options.layers = state.args[++state.index];
  state.options.intervals = state.args[++state.index];
  state.options.output = state.args[++state.index];
  return true;
}

// Reads --resources and its folder. The folder MAY look like an option.
bool ParseResources(ParseState& state) {
  if (state.index + 1 >= state.args.size()) {
    state.err << "--resources needs a folder\n";
    return false;
  }
  state.options.resources = state.args[++state.index];
  return true;
}

// Reads --only and every name up to the next option.
bool ParseOnly(ParseState& state) {
  if (!HasOperand(state)) {
    state.err << "--only needs at least one name\n";
    return false;
  }
  while (HasOperand(state)) {
    state.options.only.push_back(state.args[++state.index].generic_string());
  }
  return true;
}

// Reads the option at the current index, with its operands.
bool ParseOption(ParseState& state) {
  const std::string arg = state.args[state.index].generic_string();
  if (arg == "--list") {
    return ClaimMode(state, Options::Mode::List, kOneModeMessage);
  }
  for (const auto& [name, mode] : kFolderModes) {
    if (arg == name) {
      return ParseFolderMode(state, mode);
    }
  }
  if (arg == "--predict") {
    return ParsePredict(state);
  }
  if (arg == "--resources") {
    return ParseResources(state);
  }
  if (arg == "--only") {
    return ParseOnly(state);
  }
  state.err << "unknown argument " << arg << "\n";
  return false;
}

}  // namespace

std::optional<Options>
    CommandLine::Parse(const std::vector<std::filesystem::path>& args,
                       std::ostream& err) {
  Options options;
  options.resources = std::filesystem::path(u8"" NTFS_FUZZGEN_RESOURCES_DIR);

  ParseState state{.args = args, .index = 1, .options = options, .err = err};
  for (; state.index < args.size(); ++state.index) {
    if (!ParseOption(state)) {
      return std::nullopt;
    }
  }
  return options;
}

void CommandLine::Usage(std::ostream& out) {
  out << "usage: NtfsFuzzGen (--list | --out DIR | --seeds DIR | --verify "
         "DIR)\n"
         "                   [--only NAME...] [--resources DIR]\n"
         "  --list       print the name of each scenario\n"
         "  --out DIR    write every generated file under DIR: the fuzz "
         "inputs\n"
         "               flat, the corpus traces in DIR/corpus-traces\n"
         "  --seeds DIR  write only the fuzz inputs, flat in DIR: the seed\n"
         "               corpus of AFL and libFuzzer\n"
         "  --verify DIR compare the generated files with the ones under DIR\n"
         "               (exit 1 if one differs or is missing)\n"
         "  --predict    write the bytes that a layers file gives for the\n"
         "               ranges of an intervals file (lines \"START END\")\n"
         "  --only NAME  restrict the run to the named scenarios\n"
         "  --resources  the folder of the extracted resource archives\n";
}

}  // namespace NTFS_BROWSER_NAMESPACE::Tool::FuzzGen
