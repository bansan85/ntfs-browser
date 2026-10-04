#include "std-filesystem-walk.h"

#include <ntfs-browser/win-types.h>

#include <chrono>
#include <cstdio>
#include <optional>
#include <ratio>
#include <string>
#include <system_error>
#include <utility>

#include "time-convert.h"

namespace NtfsCompare
{

namespace
{

ULONGLONG
FileClockToUtcTicks(std::filesystem::file_time_type file_time) noexcept
{
#ifdef _WIN32
  // MSVC's file clock counts 100 ns ticks since 1601-01-01: already FILETIME.
  return static_cast<ULONGLONG>(file_time.time_since_epoch().count());
#else
  const auto sys = std::chrono::file_clock::to_sys(file_time);
  const auto sinceEpoch = sys.time_since_epoch();
  const auto seconds =
      std::chrono::duration_cast<std::chrono::seconds>(sinceEpoch);
  const auto subSecondTicks = std::chrono::duration_cast<
      std::chrono::duration<long long, std::ratio<1, 10'000'000>>>(sinceEpoch -
                                                                   seconds);
  return (static_cast<ULONGLONG>(seconds.count()) + kUnixEpochOffsetSeconds) *
             kTicksPerSecond +
         static_cast<ULONGLONG>(subSecondTicks.count());
#endif
}

// The relative path key every method shares: "/"-separated, root-relative.
std::wstring RelativeKey(const std::filesystem::path& root,
                         const std::filesystem::path& full)
{
  return full.lexically_relative(root).generic_wstring();
}

}  // namespace

Listing WalkStdFilesystem(const std::filesystem::path& root)
{
  Listing result;

  std::error_code error_code;
  const auto options =
      std::filesystem::directory_options::skip_permission_denied;
  auto iterator =
      std::filesystem::recursive_directory_iterator(root, options, error_code);
  const auto end = std::filesystem::recursive_directory_iterator();
  for (; !error_code && iterator != end; iterator.increment(error_code))
  {
    const std::filesystem::directory_entry& directory_entry = *iterator;

    // Never recurse into (or past) a symlink/junction as a directory: every
    // method in this tool follows the same policy.
    if (directory_entry.is_symlink(error_code))
    {
      iterator.disable_recursion_pending();
    }

    Entry entry;
    entry.is_directory = directory_entry.is_directory(error_code) &&
                         !directory_entry.is_symlink(error_code);

    if (!entry.is_directory)
    {
      std::error_code sizeEc;
      const auto size = directory_entry.file_size(sizeEc);
      if (!sizeEc)
      {
        entry.logical_size = size;
      }
    }

    std::error_code timeEc;
    const auto writeTime = directory_entry.last_write_time(timeEc);
    if (!timeEc)
    {
      entry.modification_time_utc = FileClockToUtcTicks(writeTime);
    }

    result.emplace(RelativeKey(root, directory_entry.path()), entry);
  }

  if (error_code)
  {
    static_cast<void>(std::fprintf(stderr, "std::filesystem: %s\n",
                                   error_code.message().c_str()));
  }

  return result;
}

}  // namespace NtfsCompare
