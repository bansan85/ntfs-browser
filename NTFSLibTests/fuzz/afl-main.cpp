#include <iostream>
#include <string>
#include <vector>

#include "fuzz-core.h"

// Windows gives a wmain() the command line as wide characters. A narrow
// main() only ever sees it through the active ANSI code page, which cannot
// express every path. Every other platform has narrow argv and nothing else.
#ifdef _WIN32
  #define NTFS_FUZZ_MAIN wmain
#else
  #define NTFS_FUZZ_MAIN main
#endif

// Keeps any exception from escaping main().
int NTFS_FUZZ_MAIN(int argc, NtfsFuzz::ArgChar* argv[]) {
  try {
    const std::vector<std::basic_string<NtfsFuzz::ArgChar>> args(argv,
                                                                 argv + argc);
    return NtfsFuzz::RunCommandLine(args, std::cerr);
  } catch (...) {
    std::cerr << "Unhandled exception\n";
    return 1;
  }
}
