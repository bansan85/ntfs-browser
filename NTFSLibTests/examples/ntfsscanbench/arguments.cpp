#include "arguments.h"

#include <array>
#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>

#include <ntfs-browser/namespace.h>
#include <ntfs-browser/search/scan.h>
#include <ntfs-browser/search/types.h>
#include <ntfs-browser/tool/device-access/bench-options.h>
#include <ntfs-browser/tool/device-access/text-convert.h>
#include <ntfs-browser/tool/device-access/tool-support.h>

#include "alloc/cluster-account.h"
#include "log/log.h"

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

// Command-line switch that runs the engine through Search::Scan, and the
// options that only a scan takes: a pattern on the name and bounds on the size.
constexpr std::wstring_view via_scan_option = L"--via-scan";
constexpr std::wstring_view name_option = L"--name=";
constexpr std::wstring_view regex_option = L"--regex=";
constexpr std::wstring_view min_size_option = L"--min-size=";
constexpr std::wstring_view max_size_option = L"--max-size=";
constexpr std::wstring_view match_path_option = L"--match-path";

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

  constexpr std::array<Named, 5> engines{{{L"mftlist", Engine::MftList},
                                          {L"nocache", Engine::NoCache},
                                          {L"fullcache", Engine::FullCache},
                                          {L"auto", Engine::Auto},
                                          {L"all", Engine::All}}};
  for (const Named& named : engines) {
    if (text == named.name) {
      engine = named.engine;
      return true;
    }
  }
  fprintf(stderr,
          "Unknown engine %s: mftlist, nocache, fullcache, auto or all\n",
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

// The UTF-16 units of text. wchar_t is UTF-16 on Windows and UTF-32 elsewhere:
// a code point past U+FFFF becomes a surrogate pair.
std::u16string ToUtf16(std::wstring_view text) {
  constexpr std::uint32_t bmp_end = 0x10000U;
  constexpr std::uint32_t high_surrogate = 0xD800U;
  constexpr std::uint32_t low_surrogate = 0xDC00U;
  constexpr std::uint32_t surrogate_bits = 10U;
  constexpr std::uint32_t surrogate_mask = 0x3FFU;
  std::u16string units;
  for (const wchar_t character : text) {
    const auto point = static_cast<std::uint32_t>(character);
    if (point < bmp_end) {
      units.push_back(static_cast<char16_t>(point));
    } else {
      const std::uint32_t offset = point - bmp_end;
      units.push_back(
          static_cast<char16_t>(high_surrogate + (offset >> surrogate_bits)));
      units.push_back(
          static_cast<char16_t>(low_surrogate + (offset & surrogate_mask)));
    }
  }
  return units;
}

// Reads a number of bytes: decimal digits only, and a value that fits.
bool ParseBytes(std::wstring_view text, std::uint64_t& bytes) {
  // The most digits of a number that fits in 64 bits.
  constexpr size_t max_digits = 19;
  if (text.empty() || text.size() > max_digits) {
    return false;
  }
  std::uint64_t value = 0;
  for (const wchar_t digit : text) {
    if (digit < L'0' || digit > L'9') {
      return false;
    }
    value = (value * 10) + static_cast<std::uint64_t>(digit - L'0');
  }
  bytes = value;
  return true;
}

// Sets the pattern of --name or --regex. Rejects an empty text and both
// options together.
OptionStatus SetPattern(Search::PatternKind kind, std::wstring_view text,
                        Arguments& args) {
  if (text.empty() || (args.pattern.kind != Search::PatternKind::None &&
                       args.pattern.kind != kind)) {
    fprintf(stderr,
            "--name and --regex need a pattern, and exclude each other\n");
    return OptionStatus::Rejected;
  }
  if (kind == Search::PatternKind::Regex && !Search::RegexAvailable()) {
    fprintf(stderr,
            "--regex is not available: this build has no regex support\n");
    return OptionStatus::Rejected;
  }
  args.pattern.kind = kind;
  args.pattern.text = ToUtf16(text);
  return OptionStatus::Accepted;
}

// Applies arg when it is an option of the scan: the pattern and the size
// bounds.
OptionStatus ParseScanOption(std::wstring_view arg, Arguments& args) {
  if (arg == via_scan_option) {
    args.via_scan = true;
    return OptionStatus::Accepted;
  }
  if (arg == match_path_option) {
    args.pattern.target = Search::MatchTarget::FullPath;
    return OptionStatus::Accepted;
  }
  if (arg.starts_with(name_option)) {
    return SetPattern(Search::PatternKind::Wildcard,
                      arg.substr(name_option.size()), args);
  }
  if (arg.starts_with(regex_option)) {
    return SetPattern(Search::PatternKind::Regex,
                      arg.substr(regex_option.size()), args);
  }
  for (const std::wstring_view option : {min_size_option, max_size_option}) {
    if (!arg.starts_with(option)) {
      continue;
    }
    std::uint64_t bytes = 0;
    if (!ParseBytes(arg.substr(option.size()), bytes)) {
      fprintf(stderr, "%s needs a number of bytes\n",
              Tool::DeviceAccess::WideToUtf8(option).c_str());
      return OptionStatus::Rejected;
    }
    (option == min_size_option ? args.filter.size.min : args.filter.size.max) =
        bytes;
    return OptionStatus::Accepted;
  }
  return OptionStatus::NotMine;
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
    case Engine::Auto:
      return "auto";
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
    if (status == OptionStatus::NotMine) {
      status = ParseScanOption(arg, args);
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
    args.target_units = ToUtf16(arg);
  }
  if (args.target == nullptr) {
    return false;
  }
  const bool scan_only = args.engine == Engine::Auto ||
                         args.pattern.kind != Search::PatternKind::None ||
                         args.filter.size.min || args.filter.size.max;
  if (scan_only && !args.via_scan) {
    fprintf(stderr,
            "--engine=auto, --name, --regex, --min-size and --max-size need "
            "--via-scan\n");
    return false;
  }
  return true;
}

void Usage() {
  printf(
      "Usage: ntfsscanbench "
      "[--engine=mftlist|nocache|fullcache|auto|all] "
      "[--via-scan [--name=P] [--regex=P] [--min-size=N] [--max-size=N]] "
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
  printf(
      "  --via-scan    run the engine through Search::Scan, as the library "
      "runs a\n");
  printf(
      "                search, and measure its worker thread. Lists nothing; "
      "prints\n");
  printf(
      "                the verdicts (T_real, T_io from the read log, T_cpu "
      "from a\n");
  printf(
      "                second warm run, R, critical path), where the worker "
      "spent\n");
  printf(
      "                its time, and the parallelism. T_real is cold only if "
      "the\n");
  printf(
      "                file cache was empty. Takes --engine=auto too, and "
      "--name=P\n");
  printf(
      "                (wildcards), --regex=P, --min-size=N, --max-size=N "
      "and\n");
  printf("                --match-path, as ntfsdir does\n");
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
