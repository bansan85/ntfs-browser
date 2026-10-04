// Recursively lists a folder six ways and reports where they disagree.
//
// The three NtfsBrowser-based listings (NtfsVolume<FULL_CACHE>,
// NtfsVolume<NO_CACHE>, MftTree) are the reference: all three read the same
// on-disk NTFS metadata through different code paths, so they must agree on
// everything. std::filesystem and the platform's native API (Windows API or
// Linux API) are then diffed against that reference, field by field, on
// whatever each of them actually provides - see compare-engine.h for exactly
// how the three passes work.
//
// usage: ntfscompare [--log=...] <folder>

#include <ntfs-browser/win-types.h>

#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>

#include <gsl/narrow>

#include <ntfs-browser/log.h>
#include <ntfs-browser/mft-tree.h>

#include "compare-engine.h"
#include "console.h"
#include "entry.h"
#include "library-walk.h"
#include "os-api-walk.h"
#include "std-filesystem-walk.h"
#include "volume-open.h"

using NtfsBrowser::MftScanOptions;
using NtfsBrowser::MftTree;
using NtfsCompare::Listing;
using NtfsCompare::NativeText;
using NtfsCompare::OpenVolumeFor;
using NtfsCompare::OsApiMethodName;
using NtfsCompare::PrintErr;
using NtfsCompare::Report;
using NtfsCompare::ResolveDirectoryRecord;
using NtfsCompare::VolumeHandles;
using NtfsCompare::WalkLibraryIndex;
using NtfsCompare::WalkMftTree;
using NtfsCompare::WalkOsApi;
using NtfsCompare::WalkStdFilesystem;

namespace Log = NtfsBrowser::Log;

// main() only ever sees argv through the active ANSI code page on Windows,
// which cannot express every path; wmain()'s argv is wide instead. Every
// other platform has narrow argv and nothing else.
#ifdef _WIN32
  #define NTFSCOMPARE_MAIN wmain
using ArgChar = wchar_t;
#else
  #define NTFSCOMPARE_MAIN main
using ArgChar = char;
#endif

namespace
{

#ifdef _WIN32
constexpr std::wstring_view kLogPrefix = Log::kOptionPrefixW;
#else
constexpr std::string_view kLogPrefix = Log::kOptionPrefix;
#endif

void Usage(const ArgChar* program)
{
  PrintErr("usage: {} [--log=...] <folder>\n", NativeText(program));
  PrintErr("  {}\n", Log::kOptionUsage);
  PrintErr(
      "Compares 6 ways of recursively listing <folder>: std::filesystem, "
      "the platform's native API, and NtfsBrowser via NtfsVolume<FULL_CACHE>, "
      "NtfsVolume<NO_CACHE> and MftTree.\n");
}

int Run(int argc, ArgChar** argv)
{
  Log::Config logConfig;
  const std::span<ArgChar*> args(argv, gsl::narrow<size_t>(argc));
  const ArgChar* targetArg = nullptr;

  for (size_t i = 1; i < args.size(); i++)
  {
    // i < args.size() by the loop condition.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    const ArgChar* const arg = args[i];
    if (std::basic_string_view<ArgChar>(arg).starts_with(kLogPrefix))
    {
      if (!Log::ParseOption(arg, logConfig))
      {
        Usage(args.front());
        return 1;
      }
      continue;
    }
    if (targetArg != nullptr)
    {
      Usage(args.front());
      return 1;
    }
    targetArg = arg;
  }

  if (targetArg == nullptr)
  {
    Usage(args.front());
    return 1;
  }

  if (!Log::Configure(logConfig))
  {
    PrintErr("Cannot open log file {}\n", NativeText(logConfig.file_path));
  }

  const std::filesystem::path target(targetArg);
  std::error_code error_code;
  if (!std::filesystem::is_directory(target, error_code))
  {
    PrintErr("{} is not a directory\n", NativeText(targetArg));
    return 1;
  }

  // std::filesystem and the native API are walked right before they're
  // compared (below), not here: the target is live, ordinary system
  // activity keeps changing it, and every NtfsBrowser-based listing this
  // reference is built from - especially the whole-$MFT scan MftTree needs
  // - takes real time. Snapshotting std::filesystem/the native API this
  // early would widen that window instead of closing it, and a field that
  // changed in between (eg. AccessTimeUtc) would show up as a false
  // MISMATCH.
  const std::optional<VolumeHandles> volume = OpenVolumeFor(target);
  if (!volume)
  {
    PrintErr(
        "Cannot open the underlying NTFS volume: the comparison "
        "needs the three NtfsBrowser-based listings as its "
        "reference, so it cannot proceed. std::filesystem found "
        "{} entries, {} found {}.\n",
        WalkStdFilesystem(target).size(), OsApiMethodName(),
        WalkOsApi(target).size());
    return 1;
  }

  const std::optional<ULONGLONG> fullCacheRecord =
      ResolveDirectoryRecord(*volume->full_cache, volume->relative_path);
  const std::optional<ULONGLONG> noCacheRecord =
      ResolveDirectoryRecord(*volume->no_cache, volume->relative_path);
  if (!fullCacheRecord || !noCacheRecord)
  {
    PrintErr("Cannot resolve {} within its NTFS volume\n",
             NativeText(targetArg));
    return 1;
  }

  PrintErr("Listing {} via NtfsVolume<FULL_CACHE>...\n", NativeText(targetArg));
  const Listing fullCacheListing =
      WalkLibraryIndex(*volume->full_cache, *fullCacheRecord);

  PrintErr("Listing {} via NtfsVolume<NO_CACHE>...\n", NativeText(targetArg));
  const Listing noCacheListing =
      WalkLibraryIndex(*volume->no_cache, *noCacheRecord);

  PrintErr(
      "Scanning the whole $MFT for MftTree (this can take a "
      "while on a large volume)...\n");
  MftScanOptions scanOptions;
  scanOptions.progress = [](ULONGLONG done, ULONGLONG total)
  {
    PrintErr("\r$MFT: {} / {}", done, total);
    if (done == total)
    {
      PrintErr("\n");
    }
    return true;
  };
  const MftTree tree(*volume->no_cache, scanOptions);
  const Listing mftTreeListing = WalkMftTree(tree, *noCacheRecord);

  Report report;
  const Listing reference = CompareLibraryMethods(
      fullCacheListing, noCacheListing, mftTreeListing, report);

  PrintErr("Listing {} via std::filesystem...\n", NativeText(targetArg));
  CompareAgainstReference("std::filesystem", reference,
                          WalkStdFilesystem(target), report);

  PrintErr("Listing {} via {}...\n", NativeText(targetArg), OsApiMethodName());
  CompareAgainstReference(OsApiMethodName(), reference, WalkOsApi(target),
                          report);

  const bool hasFindings = PrintReport(report);
  return hasFindings ? 1 : 0;
}

}  // namespace

// Keeps any exception from escaping main(). PrintErr itself could throw only
// on a broken stderr, which nothing can report.
// NOLINTNEXTLINE(bugprone-exception-escape)
int NTFSCOMPARE_MAIN(int argc, ArgChar* argv[])
{
  try
  {
    return Run(argc, argv);
  }
  catch (...)
  {
    PrintErr("Unhandled exception\n");
    return 1;
  }
}
