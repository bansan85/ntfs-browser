// The AFL entry point: it runs one input file through FuzzTestcase().
//
// The seed corpus is not in the repository. NtfsFuzzGen writes it from code,
// with the read order of the library as it is today:
//
//   cmake --build build/<preset> --target NtfsFuzzSeedCorpus
//   afl-fuzz -i build/<preset>/fuzz-generated/afl-seeds -o findings \
//       -- build/<preset>/.../NtfsFuzzerAfl @@
//
// A crash that AFL finds is a new scenario of the generator
// (NTFSLibTests/fuzz/generator), with its expected log messages in
// NTFSLibTests/unit-tests/fuzz-regression/fuzzer-expected-messages.h. Do not
// pass --inject-read-failures to AFL.

#include <iostream>
#include <string>
#include <vector>

#include <ntfs-browser/test/fuzz/fuzz-core.h>

// Windows gives a wmain() the command line as wide characters. A narrow
// main() only ever sees it through the active ANSI code page, which cannot
// express every path. Every other platform has narrow argv and nothing else.
#ifdef _WIN32
  #define NTFS_FUZZ_MAIN wmain
#else
  #define NTFS_FUZZ_MAIN main
#endif

// Keeps any exception from escaping main().
int NTFS_FUZZ_MAIN(int argc,
                   NTFS_BROWSER_NAMESPACE::Test::Fuzz::ArgChar* argv[]) {
  try {
    const std::vector<
        std::basic_string<NTFS_BROWSER_NAMESPACE::Test::Fuzz::ArgChar>>
        args(argv, argv + argc);
    return NTFS_BROWSER_NAMESPACE::Test::Fuzz::RunCommandLine(args, std::cerr);
  } catch (...) {
    std::cerr << "Unhandled exception\n";
    return 1;
  }
}
