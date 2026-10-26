#pragma once

#include <ntfs-browser/win-types.h>

#include <filesystem>
#include <memory>
#include <ostream>
#include <span>
#include <string>
#include <vector>

#include <ntfs-browser/disk-reader.h>
#include <ntfs-browser/volume-options.h>

#include "ntfs-browser/namespace.h"

namespace NtfsFuzz {

// Character type of argv: wchar_t on Windows, char elsewhere.
using ArgChar = std::filesystem::path::value_type;

// The boot sector fields FuzzTestcase() overwrites so that an input passes
// the boot sector checks. A field set to false stays as the input has it, to
// test the check on that field.
struct BootSectorPatch {
  // The 0xAA55 end marker.
  bool marker{true};
  // total_sectors, set high enough to hold any MFT address.
  bool total_sectors{true};
  // The FAT-era fields a real NTFS BPB keeps at zero, set to zero.
  bool reserved_fields{true};
};

// Runs one testcase through every parse path the fuzzer covers, once per
// cache strategy and VolumeOptions mode. Patches the boot sector signature
// in place, and the other fields patch names. Library exceptions are caught
// here; a crash is not.
// Log messages go to the library logger, so the caller picks the sink.
// inject_failures adds one run per strategy and mode for each injected
// read failure.
void FuzzTestcase(std::span<BYTE> data, bool inject_failures,
                  const BootSectorPatch& patch = {});

// Runs every probe of FuzzTestcase() once, on a NoCache volume over reader.
// The reads are those of the first run FuzzTestcase() makes of an input, so a
// recording reader around an image yields the byte stream that input replays.
// Library exceptions are caught here.
void ProbeOnce(std::unique_ptr<NTFS_BROWSER_NAMESPACE::IDiskReader> reader,
               const NTFS_BROWSER_NAMESPACE::VolumeOptions& options = {});

// Scans the $MFT once with one thread, on a NoCache volume over reader, as
// the thread probe of FuzzTestcase() does for each input. A recording reader
// around an image yields the byte stream that probe replays in step: the other
// probes of an input read in another order. Library exceptions are caught
// here.
void ProbeScanOnce(std::unique_ptr<NTFS_BROWSER_NAMESPACE::IDiskReader> reader,
                   const NTFS_BROWSER_NAMESPACE::VolumeOptions& options = {});

// Runs the NtfsFuzzerAfl command line. args[0] is the program name, the
// rest are options and one input file. Usage and errors go to err. Applies
// any --log option with Log::Configure(), so the library logger's sinks
// change for the rest of the process. Returns the exit code.
int RunCommandLine(const std::vector<std::basic_string<ArgChar>>& args,
                   std::ostream& err);

}  // namespace NtfsFuzz
