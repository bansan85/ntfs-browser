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

#include <cstdio>
#include <filesystem>
#include <optional>
#include <string_view>

#include <ntfs-browser/log.h>
#include <ntfs-browser/mft-idx.h>
#include <ntfs-browser/mft-tree.h>

#include "compare-engine.h"
#include "entry.h"
#include "library-walk.h"
#include "os-api-walk.h"
#include "std-filesystem-walk.h"
#include "volume-open.h"

using namespace NtfsBrowser;
using namespace NtfsCompare;

// main() only ever sees argv through the active ANSI code page on Windows,
// which cannot express every path; wmain()'s argv is wide instead. Every
// other platform has narrow argv and nothing else.
#ifdef _WIN32
  #define NTFSCOMPARE_MAIN wmain
  #define NTFSCOMPARE_NATIVE "%ls"
using ArgChar = wchar_t;
#else
  #define NTFSCOMPARE_MAIN main
  #define NTFSCOMPARE_NATIVE "%s"
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
  std::fprintf(stderr, "usage: " NTFSCOMPARE_NATIVE " [--log=...] <folder>\n",
               program);
  std::fprintf(stderr, "  %s\n", std::string(Log::kOptionUsage).c_str());
  std::fprintf(
      stderr,
      "Compares 6 ways of recursively listing <folder>: std::filesystem, "
      "the platform's native API, and NtfsBrowser via NtfsVolume<FULL_CACHE>, "
      "NtfsVolume<NO_CACHE> and MftTree.\n");
}

}  // namespace

int NTFSCOMPARE_MAIN(int argc, ArgChar* argv[])
{
  Log::Config logConfig;
  const ArgChar* targetArg = nullptr;

  for (int i = 1; i < argc; i++)
  {
    if (std::basic_string_view<ArgChar>(argv[i]).starts_with(kLogPrefix))
    {
      if (!Log::ParseOption(argv[i], logConfig))
      {
        Usage(argv[0]);
        return 1;
      }
      continue;
    }
    if (targetArg != nullptr)
    {
      Usage(argv[0]);
      return 1;
    }
    targetArg = argv[i];
  }

  if (targetArg == nullptr)
  {
    Usage(argv[0]);
    return 1;
  }

  if (!Log::Configure(logConfig))
  {
    std::fprintf(stderr, "Cannot open log file %ls\n",
                 logConfig.file_path.c_str());
  }

  const std::filesystem::path target(targetArg);
  std::error_code ec;
  if (!std::filesystem::is_directory(target, ec))
  {
    std::fprintf(stderr, NTFSCOMPARE_NATIVE " is not a directory\n", targetArg);
    return 1;
  }

  std::fprintf(stderr,
               "Listing " NTFSCOMPARE_NATIVE " via std::filesystem...\n",
               targetArg);
  const Listing stdFsListing = WalkStdFilesystem(target);

  std::fprintf(stderr, "Listing " NTFSCOMPARE_NATIVE " via %s...\n", targetArg,
               OsApiMethodName());
  const Listing osApiListing = WalkOsApi(target);

  const std::optional<VolumeHandles> volume = OpenVolumeFor(target);
  if (!volume)
  {
    std::fprintf(
        stderr,
        "Cannot open the underlying NTFS volume: the comparison needs the "
        "three NtfsBrowser-based listings as its reference, so it cannot "
        "proceed. std::filesystem found %zu entries, %s found %zu.\n",
        stdFsListing.size(), OsApiMethodName(), osApiListing.size());
    return 1;
  }

  const std::optional<ULONGLONG> fullCacheRecord =
      ResolveDirectoryRecord(*volume->full_cache, volume->relative_path);
  const std::optional<ULONGLONG> noCacheRecord =
      ResolveDirectoryRecord(*volume->no_cache, volume->relative_path);
  if (!fullCacheRecord || !noCacheRecord)
  {
    std::fprintf(stderr,
                 "Cannot resolve " NTFSCOMPARE_NATIVE
                 " within its NTFS volume\n",
                 targetArg);
    return 1;
  }

  std::fprintf(stderr,
               "Listing " NTFSCOMPARE_NATIVE " via NtfsVolume<FULL_CACHE>...\n",
               targetArg);
  const Listing fullCacheListing =
      WalkLibraryIndex(*volume->full_cache, *fullCacheRecord);

  std::fprintf(stderr,
               "Listing " NTFSCOMPARE_NATIVE " via NtfsVolume<NO_CACHE>...\n",
               targetArg);
  const Listing noCacheListing =
      WalkLibraryIndex(*volume->no_cache, *noCacheRecord);

  std::fprintf(stderr,
               "Scanning the whole $MFT for MftTree (this can take a "
               "while on a large volume)...\n");
  MftScanOptions scanOptions;
  scanOptions.progress = [](ULONGLONG done, ULONGLONG total)
  {
    std::fprintf(stderr, "\r$MFT: %llu / %llu", done, total);
    if (done == total)
    {
      std::fprintf(stderr, "\n");
    }
    return true;
  };
  const MftTree tree(*volume->no_cache, scanOptions);
  const Listing mftTreeListing = WalkMftTree(tree, *noCacheRecord);

  Report report;
  const Listing reference = CompareLibraryMethods(
      fullCacheListing, noCacheListing, mftTreeListing, report);
  CompareAgainstReference("std::filesystem", reference, stdFsListing, report);
  CompareAgainstReference(OsApiMethodName(), reference, osApiListing, report);

  const bool hasFindings = PrintReport(report);
  return hasFindings ? 1 : 0;
}
