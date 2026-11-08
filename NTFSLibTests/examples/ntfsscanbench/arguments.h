#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include <ntfs-browser/alloc/cluster-account.h>
#include <ntfs-browser/log/log.h>
#include <ntfs-browser/namespace.h>
#include <ntfs-browser/search/types.h>
#include <ntfs-browser/tool/device-access/bench-options.h>
#include <ntfs-browser/volume-options.h>

namespace NTFS_BROWSER_NAMESPACE::Example::NtfsScanBench {

// How the volume is listed. Each engine reads the same volume its own way.
enum class Engine : std::uint8_t {
  // The scan of $MFT in record order (MftTree): every file of the volume.
  MftList,
  // The walk of the directory indexes, with a volume that keeps no cache.
  NoCache,
  // The same walk, with a volume that caches what it reads.
  FullCache,
  // The choice of the scan itself: MftList for the root of a volume, a walk
  // for a folder. Only with --via-scan.
  Auto,
  // The three one after the other on the same target, then a table.
  All
};

// The name of engine on the command line ("mftlist", "nocache", "fullcache",
// "auto" or "all").
std::string_view EngineName(Engine engine);

// What the command line asks for.
struct Arguments {
  NTFS_BROWSER_NAMESPACE::Log::Config log_config;
  NTFS_BROWSER_NAMESPACE::VolumeOptions volume_options;
  Engine engine = Engine::MftList;
  // Whether a walk lists every directory below the target (--recursive). The
  // mftlist engine always lists the whole volume.
  bool recursive = false;
  // Whether to write no listing on stdout (--quiet).
  bool quiet = false;
  // Threads that scan $MFT and format the listing, 0 to choose. Only the
  // mftlist engine takes them.
  unsigned threads = 0;
  NTFS_BROWSER_NAMESPACE::Alloc::ClusterAccountLevel clusters =
      NTFS_BROWSER_NAMESPACE::Alloc::ClusterAccountLevel::Off;
  NTFS_BROWSER_NAMESPACE::Alloc::DataCheckMode verify =
      NTFS_BROWSER_NAMESPACE::Alloc::DataCheckMode::Off;
  // Stays null until the command line names a volume. A walk also takes a
  // directory after the image or device ("disk.img\Windows").
  const wchar_t* target = nullptr;
  // The same target as UTF-16 units, for Search::Options. Set with target.
  std::u16string target_units;
  // Runs the engine through Search::Scan (--via-scan), as the library runs a
  // search, and measures its worker thread. The listing is not shown.
  bool via_scan = false;
  // What --name, --regex, --min-size and --max-size ask of a scan. Only with
  // --via-scan.
  NTFS_BROWSER_NAMESPACE::Search::Pattern pattern;
  NTFS_BROWSER_NAMESPACE::Search::Filter filter;
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

}  // namespace NTFS_BROWSER_NAMESPACE::Example::NtfsScanBench
