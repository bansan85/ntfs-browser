#pragma once

#include <string>
#include <vector>

#include <ntfs-browser/alloc/cluster-account.h>
#include <ntfs-browser/log/log.h>
#include <ntfs-browser/namespace.h>
#include <ntfs-browser/tool/device-access/bench-options.h>
#include <ntfs-browser/volume-options.h>

namespace NTFS_BROWSER_NAMESPACE::Example::NtfsMftList {

// What the command line asks for.
struct Arguments {
  NTFS_BROWSER_NAMESPACE::Log::Config log_config;
  NTFS_BROWSER_NAMESPACE::VolumeOptions volume_options;
  // Threads that scan $MFT and format the listing, 0 to choose.
  unsigned threads = 0;
  NTFS_BROWSER_NAMESPACE::Alloc::ClusterAccountLevel clusters =
      NTFS_BROWSER_NAMESPACE::Alloc::ClusterAccountLevel::Off;
  NTFS_BROWSER_NAMESPACE::Alloc::DataCheckMode verify =
      NTFS_BROWSER_NAMESPACE::Alloc::DataCheckMode::Off;
  // Stays null until the command line names a volume.
  const wchar_t* target = nullptr;
  // The instrumentation and tuning options, and the command line they came
  // from as UTF-8, for the summary.
  Tool::DeviceAccess::BenchOptions bench;
  std::vector<std::string> command_line;
};

// Fills args from the command line. Returns false on an unknown or malformed
// option, or unless exactly one volume is named.
bool ParseArguments(int argc, wchar_t* argv[], Arguments& args);

// Prints command-line usage help.
void Usage();

}  // namespace NTFS_BROWSER_NAMESPACE::Example::NtfsMftList
