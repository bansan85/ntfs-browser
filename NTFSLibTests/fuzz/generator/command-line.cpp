#include "command-line.h"

#include <cstddef>
#include <filesystem>
#include <optional>
#include <ostream>
#include <string>
#include <vector>

#include <ntfs-browser/namespace.h>

#ifndef NTFS_FUZZGEN_RESOURCES_DIR
  #define NTFS_FUZZGEN_RESOURCES_DIR ""
#endif

namespace NTFS_BROWSER_NAMESPACE::Tool::FuzzGen {

namespace {

// True when arg starts an option.
bool IsOption(const std::filesystem::path& arg) {
  return arg.generic_string().starts_with("--");
}

}  // namespace

std::optional<Options>
    CommandLine::Parse(const std::vector<std::filesystem::path>& args,
                       std::ostream& err) {
  Options options;
  options.resources = std::filesystem::path(u8"" NTFS_FUZZGEN_RESOURCES_DIR);

  // Reads the folder that follows a mode option.
  const auto set_mode = [&](Options::Mode mode, std::size_t& index) {
    if (options.mode != Options::Mode::Help) {
      err << "only one of --list, --out, --seeds and --verify is allowed\n";
      return false;
    }
    options.mode = mode;
    if (mode == Options::Mode::List) {
      return true;
    }
    if (index + 1 >= args.size() || IsOption(args[index + 1])) {
      err << args[index].string() << " needs a folder\n";
      return false;
    }
    options.folder = args[++index];
    return true;
  };

  for (std::size_t index = 1; index < args.size(); ++index) {
    const std::string arg = args[index].generic_string();
    bool ok = true;
    if (arg == "--list") {
      ok = set_mode(Options::Mode::List, index);
    } else if (arg == "--out") {
      ok = set_mode(Options::Mode::Out, index);
    } else if (arg == "--seeds") {
      ok = set_mode(Options::Mode::Seeds, index);
    } else if (arg == "--verify") {
      ok = set_mode(Options::Mode::Verify, index);
    } else if (arg == "--predict") {
      if (options.mode != Options::Mode::Help) {
        err << "only one of --list, --out, --seeds, --verify and --predict "
               "is allowed\n";
        return std::nullopt;
      }
      constexpr std::size_t predict_operands = 3;
      if (index + predict_operands >= args.size()) {
        err << "--predict needs a layers file, an intervals file and an "
               "output file\n";
        return std::nullopt;
      }
      options.mode = Options::Mode::Predict;
      options.layers = args[++index];
      options.intervals = args[++index];
      options.output = args[++index];
    } else if (arg == "--resources") {
      if (index + 1 >= args.size()) {
        err << "--resources needs a folder\n";
        return std::nullopt;
      }
      options.resources = args[++index];
    } else if (arg == "--only") {
      if (index + 1 >= args.size() || IsOption(args[index + 1])) {
        err << "--only needs at least one name\n";
        return std::nullopt;
      }
      while (index + 1 < args.size() && !IsOption(args[index + 1])) {
        options.only.push_back(args[++index].generic_string());
      }
    } else {
      err << "unknown argument " << arg << "\n";
      ok = false;
    }
    if (!ok) {
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
