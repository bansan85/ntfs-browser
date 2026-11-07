#include "arguments.h"

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

namespace MftList {

namespace {

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

// Applies arg when it is an instrumentation, tuning or logging option, the
// ones the console tools share.
OptionStatus ParseSharedOption(std::wstring_view arg, Arguments& args) {
  std::string error;
  switch (NtfsDevice::ParseBenchOption(arg, args.bench, error)) {
    case NtfsDevice::OptionResult::Accepted:
      return OptionStatus::Accepted;
    case NtfsDevice::OptionResult::Malformed:
      fprintf(stderr, "%s\n", error.c_str());
      return OptionStatus::Rejected;
    case NtfsDevice::OptionResult::NotMine:
      break;
  }
  if (!NtfsDevice::IsLogOption(arg)) {
    return OptionStatus::NotMine;
  }
  return NtfsDevice::ParseLogOption(arg, args.log_config)
             ? OptionStatus::Accepted
             : OptionStatus::Rejected;
}

// Applies arg when it is an option of the listing itself.
OptionStatus ParseListingOption(std::wstring_view arg, Arguments& args) {
  if (arg == deleted_option) {
    args.volume_options.include_deleted = true;
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

bool ParseArguments(int argc, wchar_t* argv[], Arguments& args) {
  for (int i = 1; i < argc; i++) {
    const std::wstring_view arg(argv[i]);
    args.command_line.push_back(NtfsDevice::WideToUtf8(arg));
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
      "Usage: ntfsmftlist [--log=...] [--deleted] [--recover] [--threads=N] "
      "[--clusters[=map]] [--verify-data[=sample|full]] <volume>\n");
  printf("  %s\n", std::string(Log::option_usage).c_str());
  printf("%s", std::string(NtfsDevice::BenchUsage()).c_str());
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
  printf(
      "Columns: record, sequence, DEL if freed, <DIR> or size, last write,\n");
  printf(
      "  attributes (Read-only Hidden System Compressed Encrypted sParse),\n");
  printf("  hard links, path. A path whose parent chain breaks reads\n");
  printf("  <lost #N>\\..., N being the record where it breaks.\n");
#ifdef _WIN32
  printf("eg. ntfsmftlist c:\n");
  printf("eg. ntfsmftlist --deleted d:\\images\\disk.img\n");
  printf("eg. ntfsmftlist --deleted --recover d:\\images\\disk.img\n");
#else
  printf("eg. ntfsmftlist /dev/sda1\n");
  printf("eg. ntfsmftlist --deleted ./disk.img\n");
  printf("eg. ntfsmftlist --deleted --recover ./disk.img\n");
#endif
}

}  // namespace MftList
