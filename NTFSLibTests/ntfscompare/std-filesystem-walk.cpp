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

ULONGLONG FileClockToUtcTicks(std::filesystem::file_time_type ft) noexcept
{
  const auto sys = std::chrono::file_clock::to_sys(ft);
  const auto sinceEpoch = sys.time_since_epoch();
  const auto seconds =
      std::chrono::duration_cast<std::chrono::seconds>(sinceEpoch);
  const auto subSecondTicks = std::chrono::duration_cast<
      std::chrono::duration<long long, std::ratio<1, 10'000'000>>>(sinceEpoch -
                                                                   seconds);
  return (static_cast<ULONGLONG>(seconds.count()) + kUnixEpochOffsetSeconds) *
             kTicksPerSecond +
         static_cast<ULONGLONG>(subSecondTicks.count());
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

  std::error_code ec;
  const auto options =
      std::filesystem::directory_options::skip_permission_denied;
  auto it = std::filesystem::recursive_directory_iterator(root, options, ec);
  const auto end = std::filesystem::recursive_directory_iterator();
  for (; !ec && it != end; it.increment(ec))
  {
    const std::filesystem::directory_entry& de = *it;

    // Never recurse into (or past) a symlink/junction as a directory: every
    // method in this tool follows the same policy.
    if (de.is_symlink(ec))
    {
      it.disable_recursion_pending();
    }

    Entry entry;
    entry.is_directory = de.is_directory(ec) && !de.is_symlink(ec);

    if (!entry.is_directory)
    {
      std::error_code sizeEc;
      const auto size = de.file_size(sizeEc);
      if (!sizeEc)
      {
        entry.logical_size = size;
      }
    }

    std::error_code timeEc;
    const auto writeTime = de.last_write_time(timeEc);
    if (!timeEc)
    {
      entry.modification_time_utc = FileClockToUtcTicks(writeTime);
    }

    result.emplace(RelativeKey(root, de.path()), std::move(entry));
  }

  if (ec)
  {
    std::fprintf(stderr, "std::filesystem: %s\n", ec.message().c_str());
  }

  return result;
}

}  // namespace NtfsCompare
