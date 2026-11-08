#include "arguments.h"

#include <array>
#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>

#include <ntfs-browser/alloc/cluster-account.h>
#include <ntfs-browser/log/log.h>
#include <ntfs-browser/namespace.h>
#include <ntfs-browser/tool/device-access/bench-options.h>
#include <ntfs-browser/tool/device-access/text-convert.h>
#include <ntfs-browser/tool/device-access/tool-support.h>

using namespace NTFS_BROWSER_NAMESPACE;

namespace NTFS_BROWSER_NAMESPACE::Example::NtfsScanBench {

namespace {

// Command-line prefix of the switch that picks the engine, and its values.
constexpr std::wstring_view engine_option = L"--engine=";
// Command-line switches that list every directory below the target, and that
// write no listing.
constexpr std::wstring_view recursive_option = L"--recursive";
constexpr std::wstring_view recursive_short_option = L"-r";
constexpr std::wstring_view quiet_option = L"--quiet";
// Command-line switch that adds the records NTFS freed to the listing.
constexpr std::wstring_view deleted_option = L"--deleted";
// Command-line switch that salvages a damaged item instead of rejecting it.
constexpr std::wstring_view recover_option = L"--recover";
// Command-line prefix of the switch that sets how many threads scan $MFT and
// format the listing.
constexpr std::wstring_view threads_option = L"--threads=";
// Command-line switch that counts the clusters of the freed and damaged files,
// and its value that also lists, for each stream, where the damage lies.
constexpr std::wstring_view clusters_option = L"--clusters";
constexpr std::wstring_view clusters_map_option = L"--clusters=map";
// Command-line switch that reads the clusters presumed intact, to see whether
// they hold zeros or metadata, and its values: one cluster per stream, or all.
constexpr std::wstring_view verify_option = L"--verify-data";
constexpr std::wstring_view verify_sample_option = L"--verify-data=sample";
constexpr std::wstring_view verify_full_option = L"--verify-data=full";

// What one option of the command line is.
enum class OptionStatus : std::uint8_t {
  // The option is understood and applied.
  Accepted,
  // The option is malformed: the command line is refused.
  Rejected,
  // Not an option of this kind: someone else MAY take it.
  NotMine
};

// Reads the thread count of --threads=: one to four decimal digits, from 1 to
// 4096. False otherwise.
bool ParseThreads(std::wstring_view text, unsigned& threads) {
  constexpr unsigned max_threads = 4096;
  constexpr size_t max_digits = 4;
  if (text.empty() || text.size() > max_digits) {
    return false;
  }
  unsigned value = 0;
  for (const wchar_t digit : text) {
    if (digit < L'0' || digit > L'9') {
      return false;
    }
    value = (value * 10) + static_cast<unsigned>(digit - L'0');
  }
  if (value == 0 || value > max_threads) {
    return false;
  }
  threads = value;
  return true;
}

// Reads the engine name of --engine=. False for a name that is not one.
bool ParseEngine(std::wstring_view text, Engine& engine) {
  struct Named {
    std::wstring_view name;
    Engine engine;
  };

  constexpr std::array<Named, 4> engines{{{L"mftlist", Engine::MftList},
                                          {L"nocache", Engine::NoCache},
                                          {L"fullcache", Engine::FullCache},
                                          {L"all", Engine::All}}};
  for (const Named& named : engines) {
    if (text == named.name) {
      engine = named.engine;
      return true;
    }
  }
  fprintf(stderr, "Unknown engine %s: mftlist, nocache, fullcache or all\n",
          Tool::DeviceAccess::WideToUtf8(text).c_str());
  return false;
}

// Applies arg when it is an instrumentation, tuning or logging option, the
// ones the console tools share.
OptionStatus ParseSharedOption(std::wstring_view arg, Arguments& args) {
  std::string error;
  switch (Tool::DeviceAccess::ParseBenchOption(arg, args.bench, error)) {
    case Tool::DeviceAccess::OptionResult::Accepted:
      return OptionStatus::Accepted;
    case Tool::DeviceAccess::OptionResult::Malformed:
      fprintf(stderr, "%s\n", error.c_str());
      return OptionStatus::Rejected;
    case Tool::DeviceAccess::OptionResult::NotMine:
      break;
  }
  if (!Tool::DeviceAccess::IsLogOption(arg)) {
    return OptionStatus::NotMine;
  }
  return Tool::DeviceAccess::ParseLogOption(arg, args.log_config)
             ? OptionStatus::Accepted
             : OptionStatus::Rejected;
}

// Applies arg when it is an option of the listing itself.
OptionStatus ParseListingOption(std::wstring_view arg, Arguments& args) {
  if (arg == deleted_option) {
    args.volume_options.include_deleted = true;
    return OptionStatus::Accepted;
  }
  if (arg.starts_with(engine_option)) {
    return ParseEngine(arg.substr(engine_option.size()), args.engine)
               ? OptionStatus::Accepted
               : OptionStatus::Rejected;
  }
  if (arg == recursive_option || arg == recursive_short_option) {
    args.recursive = true;
    return OptionStatus::Accepted;
  }
  if (arg == quiet_option) {
    args.quiet = true;
    return OptionStatus::Accepted;
  }
  if (arg == recover_option) {
    args.volume_options.recover_errors = true;
    return OptionStatus::Accepted;
  }
  if (arg == clusters_option || arg == clusters_map_option) {
    args.clusters = arg == clusters_map_option
                        ? Alloc::ClusterAccountLevel::CountsAndMap
                        : Alloc::ClusterAccountLevel::Counts;
    return OptionStatus::Accepted;
  }
  if (arg == verify_option || arg == verify_sample_option ||
      arg == verify_full_option) {
    args.verify = arg == verify_full_option ? Alloc::DataCheckMode::Full
                                            : Alloc::DataCheckMode::Sample;
    return OptionStatus::Accepted;
  }
  if (arg.starts_with(threads_option)) {
    return ParseThreads(arg.substr(threads_option.size()), args.threads)
               ? OptionStatus::Accepted
               : OptionStatus::Rejected;
  }
  return OptionStatus::NotMine;
}

}  // namespace

std::string_view EngineName(Engine engine) {
  switch (engine) {
    case Engine::MftList:
      return "mftlist";
    case Engine::NoCache:
      return "nocache";
    case Engine::FullCache:
      return "fullcache";
    case Engine::All:
      break;
  }
  return "all";
}

bool ParseArguments(int argc, wchar_t* argv[], Arguments& args) {
  for (int i = 1; i < argc; i++) {
    const std::wstring_view arg(argv[i]);
    args.command_line.push_back(Tool::DeviceAccess::WideToUtf8(arg));
    OptionStatus status = ParseSharedOption(arg, args);
    if (status == OptionStatus::NotMine) {
      status = ParseListingOption(arg, args);
    }
    if (status == OptionStatus::Rejected) {
      return false;
    }
    if (status == OptionStatus::Accepted) {
      continue;
    }
    if (args.target != nullptr) {
      return false;
    }
    args.target = argv[i];
  }
  return args.target != nullptr;
}

void Usage() {
  printf(
      "Usage: ntfsscanbench [--engine=mftlist|nocache|fullcache|all] "
      "[--recursive] [--quiet] [--log=...] [--deleted] [--recover] "
      "[--threads=N] [--clusters[=map]] [--verify-data[=sample|full]] "
      "<volume>\n");
  printf("  %s\n", std::string(Log::option_usage).c_str());
  printf("%s", std::string(Tool::DeviceAccess::BenchUsage()).c_str());
  printf(
      "  --engine=E    how the volume is listed: mftlist (the default) "
      "scans\n");
  printf(
      "                $MFT and lists every file; nocache and fullcache "
      "walk\n");
  printf(
      "                the directories with a volume that keeps no cache or "
      "one\n");
  printf(
      "                that does; all runs the three one after the other on\n");
  printf(
      "                the same target, each in a recursive listing, and "
      "prints\n");
  printf(
      "                a table. With --benchmark, the summary of each engine "
      "is\n");
  printf("                written to the file name plus -<engine>\n");
  printf(
      "  --recursive   (-r) list every directory below the target: nocache "
      "and\n");
  printf("                fullcache only, mftlist always lists them all\n");
  printf("  --quiet       write no listing, only the messages and the table\n");
  printf("  --deleted     also list the records NTFS freed\n");
  printf(
      "  --threads=N   threads that parse $MFT and format the listing, 1 "
      "for\n");
  printf(
      "                one; by default the machine's, on a volume big "
      "enough\n");
  printf("                to pay for them\n");
  printf(
      "  --recover     salvage a damaged item instead of rejecting it, and\n");
  printf(
      "                read the change journal ($UsnJrnl): a ~ line gives\n");
  printf(
      "                what it knows of a file, and with --deleted the "
      "files\n");
  printf("                only it names follow the listing\n");
  printf(
      "  --clusters    count the clusters of each freed file (--deleted) and "
      "of\n");
  printf(
      "                each damaged one (--recover) by condition: a # line "
      "follows\n");
  printf(
      "                it, and the totals close the listing. =map adds, per "
      "stream,\n");
  printf(
      "                the byte ranges in file order with their condition\n");
  printf(
      "  --verify-data read the clusters presumed intact, and tell zeros and "
      "NTFS\n");
  printf(
      "                metadata from data: =sample reads the first of each "
      "stream\n");
  printf(
      "                (the default), =full reads them all. Implies "
      "--clusters\n");
#ifdef _WIN32
  printf("  <volume>      a drive letter (c:), or a device or image path\n");
#else
  printf("  <volume>      a device or image path\n");
#endif
  printf("                nocache and fullcache also take a folder after it\n");
  printf(
      "Columns: record, sequence, DEL if freed, <DIR> or size, last write,\n");
  printf(
      "  attributes (Read-only Hidden System Compressed Encrypted sParse),\n");
  printf("  hard links, path. A path whose parent chain breaks reads\n");
  printf("  <lost #N>\\..., N being the record where it breaks.\n");
#ifdef _WIN32
  printf("eg. ntfsscanbench c:\n");
  printf("eg. ntfsscanbench --deleted d:\\images\\disk.img\n");
  printf("eg. ntfsscanbench --deleted --recover d:\\images\\disk.img\n");
#else
  printf("eg. ntfsscanbench /dev/sda1\n");
  printf("eg. ntfsscanbench --deleted ./disk.img\n");
  printf("eg. ntfsscanbench --deleted --recover ./disk.img\n");
#endif
}

}  // namespace NTFS_BROWSER_NAMESPACE::Example::NtfsScanBench
