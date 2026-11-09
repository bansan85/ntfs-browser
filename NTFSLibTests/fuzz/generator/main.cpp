// Writes the fuzz inputs and the corpus traces from code, with the read order
// of the library as it is today. The unit tests and the AFL fuzzer read them.
//
// usage: NtfsFuzzGen (--list | --out DIR | --seeds DIR | --verify DIR)
//                    [--only NAME...] [--resources DIR]

#include <exception>
#include <filesystem>
#include <iostream>
#include <vector>

#include <ntfs-browser/namespace.h>
#include <ntfs-browser/tool/fuzzgen/command-line.h>
#include <ntfs-browser/tool/fuzzgen/scenario-catalog.h>

#include "generator-app.h"
#include "log/log.h"

// Windows gives a wmain() the command line as wide characters. A narrow main()
// only ever sees it through the active ANSI code page, which cannot express
// every path. Every other platform has narrow argv and nothing else.
#ifdef _WIN32
  #define NTFS_FUZZGEN_MAIN wmain
using NtfsFuzzGenArgChar = wchar_t;
#else
  #define NTFS_FUZZGEN_MAIN main
using NtfsFuzzGenArgChar = char;
#endif

// Keeps any exception from escaping main().
int NTFS_FUZZGEN_MAIN(int argc, NtfsFuzzGenArgChar* argv[]) {
  namespace FuzzGen = NTFS_BROWSER_NAMESPACE::Tool::FuzzGen;
  try {
    // The probes log what they reject: the generator wants the bytes they read,
    // not the messages.
    (void)NTFS_BROWSER_NAMESPACE::Log::Configure(
        {.console_level = NTFS_BROWSER_NAMESPACE::Log::Level::Off});

    const std::vector<std::filesystem::path> args(argv, argv + argc);
    const auto options = FuzzGen::CommandLine::Parse(args, std::cerr);
    if (!options) {
      FuzzGen::CommandLine::Usage(std::cerr);
      return 1;
    }
    const FuzzGen::ScenarioRegistry registry =
        FuzzGen::ScenarioCatalog::Build();
    return FuzzGen::GeneratorApp(registry, std::cout, std::cerr).Run(*options);
  } catch (const std::exception& error) {
    std::cerr << "NtfsFuzzGen: " << error.what() << "\n";
    return 1;
  } catch (...) {
    std::cerr << "NtfsFuzzGen: unhandled exception\n";
    return 1;
  }
}
