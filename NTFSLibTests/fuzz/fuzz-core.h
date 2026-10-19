#pragma once

#include <ntfs-browser/win-types.h>

#include <filesystem>
#include <ostream>
#include <span>
#include <string>
#include <vector>

namespace NtfsFuzz {

// Character type of argv: wchar_t on Windows, char elsewhere.
using ArgChar = std::filesystem::path::value_type;

// Runs one testcase through every parse path the fuzzer covers, once per
// cache strategy and VolumeOptions mode. Patches the boot sector signature
// and 0xAA55 end marker in place. Library exceptions are caught here; a
// crash is not.
// Log messages go to the library logger, so the caller picks the sink.
// inject_failures adds one run per strategy and mode for each injected
// read failure. patch_boot_marker false leaves the end marker as it is, to
// test an input that lacks it.
void FuzzTestcase(std::span<BYTE> data, bool inject_failures,
                  bool patch_boot_marker = true);

// Runs the NtfsFuzzerAfl command line. args[0] is the program name, the
// rest are options and one input file. Usage and errors go to err. Applies
// any --log option with Log::Configure(), so the library logger's sinks
// change for the rest of the process. Returns the exit code.
int RunCommandLine(const std::vector<std::basic_string<ArgChar>>& args,
                   std::ostream& err);

}  // namespace NtfsFuzz
