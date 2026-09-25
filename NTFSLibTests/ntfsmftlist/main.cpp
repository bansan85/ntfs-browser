// Lists every file the $MFT records, with its characteristics. Paths come
// from each record's own $FILE_NAME parent reference, not from directory
// indexes, so files of a directory whose index is lost still show up.

#include <cstdio>
#include <cwctype>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

#include <gsl/narrow>

#include <ntfs-browser/log.h>
#include <ntfs-browser/mft-tree.h>
#include <ntfs-browser/ntfs-volume.h>
#include <ntfs-browser/volume-options.h>

using namespace NtfsBrowser;

namespace
{

// Command-line switch that adds the records NTFS freed to the listing.
constexpr std::wstring_view kDeletedOption = L"--deleted";
// Command-line switch that salvages a damaged item instead of rejecting it.
constexpr std::wstring_view kRecoverOption = L"--recover";

// Width of every column before the path, separators included, so an extra
// hard link's "=" line lines up under the path it follows.
constexpr int kPathColumn = 63;

// Prints command-line usage help.
void usage()
{
  printf("Usage: ntfsmftlist [--log=...] [--deleted] [--recover] <volume>\n");
  printf("  %s\n", std::string(Log::kOptionUsage).c_str());
  printf("  --deleted     also list the records NTFS freed\n");
  printf("  --recover     salvage a damaged item instead of rejecting it\n");
  printf("  <volume>      a drive letter (c:), or a device or image path\n");
  printf(
      "Columns: record, sequence, DEL if freed, <DIR> or size, last write,\n");
  printf(
      "  attributes (Read-only Hidden System Compressed Encrypted sParse),\n");
  printf("  hard links, path. A path whose parent chain breaks reads\n");
  printf("  <lost #N>\\..., N being the record where it breaks.\n");
  printf("eg. ntfsmftlist c:\n");
  printf("eg. ntfsmftlist --deleted d:\\images\\disk.img\n");
  printf("eg. ntfsmftlist --deleted --recover d:\\images\\disk.img\n");
}

// Converts to UTF-8, which the console is switched to, so every name prints
// whatever the active code page.
std::string ToUtf8(std::wstring_view text)
{
  if (text.empty())
  {
    return {};
  }
  const int length = gsl::narrow<int>(text.size());
  const int size = WideCharToMultiByte(CP_UTF8, 0, text.data(), length, nullptr,
                                       0, nullptr, nullptr);
  std::string utf8(static_cast<size_t>(size), '\0');
  WideCharToMultiByte(CP_UTF8, 0, text.data(), length, utf8.data(), size,
                      nullptr, nullptr);
  return utf8;
}

// The path of one name of entry, as shown in the listing.
std::string DisplayPath(const MftTree& tree, const MftEntry& entry,
                        std::optional<size_t> nameIndex)
{
  std::optional<ULONGLONG> lost;
  const std::wstring path = nameIndex
                                ? tree.GetPath(entry.record, *nameIndex, &lost)
                                : tree.GetPath(entry.record, &lost);
  if (path.empty())
  {
    return "<no name>";
  }
  if (lost)
  {
    return "<lost #" + std::to_string(*lost) + ">\\" + ToUtf8(path);
  }
  return ToUtf8(path);
}

// Prints one listing line for entry, then one line per other hard link.
void PrintEntry(const MftTree& tree, const MftEntry& entry)
{
  SYSTEMTIME st{};
  FileTimeToSystemTime(&entry.write_time, &st);

  std::string sizeColumn = "<DIR>";
  if (!entry.directory)
  {
    sizeColumn = std::to_string(entry.size);
  }

  size_t links = 0;
  for (const MftName& name : entry.names)
  {
    if (!name.dos_only)
    {
      links++;
    }
  }

  const std::string path = DisplayPath(tree, entry, std::nullopt);
  printf("%10llu %5u %-3s %14s %04u-%02u-%02u %02u:%02u %c%c%c%c%c%c %2zu %s\n",
         entry.record, entry.sequence, entry.in_use ? "" : "DEL",
         sizeColumn.c_str(), st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute,
         entry.read_only ? 'R' : '-', entry.hidden ? 'H' : '-',
         entry.system ? 'S' : '-', entry.compressed ? 'C' : '-',
         entry.encrypted ? 'E' : '-', entry.sparse ? 'P' : '-', links,
         path.c_str());

  for (size_t i = 0; i < entry.names.size(); i++)
  {
    if (entry.names[i].dos_only)
    {
      continue;
    }
    const std::string other = DisplayPath(tree, entry, i);
    if (other != path)
    {
      printf("%*s= %s\n", kPathColumn, "", other.c_str());
    }
  }
}

// Opens target as a drive letter ("c" or "c:"), or else as a device or
// image path.
std::unique_ptr<NtfsVolume<Strategy::NO_CACHE>>
    OpenVolume(std::wstring_view target, const VolumeOptions& options)
{
  // A lone letter, optionally followed by a colon, names a drive.
  const bool driveLetter =
      (target.size() == 1 || (target.size() == 2 && target[1] == L':')) &&
      iswalpha(target[0]) != 0;
  if (driveLetter)
  {
    return std::make_unique<NtfsVolume<Strategy::NO_CACHE>>(target[0], options);
  }
  return std::make_unique<NtfsVolume<Strategy::NO_CACHE>>(target, options);
}

}  // namespace

int wmain(int argc, wchar_t* argv[])
{
  Log::Config logConfig;
  VolumeOptions volumeOptions;
  MftScanOptions scanOptions;
  const wchar_t* target = nullptr;

  for (int i = 1; i < argc; i++)
  {
    const std::wstring_view arg(argv[i]);
    if (arg.starts_with(Log::kOptionPrefixW))
    {
      if (!Log::ParseOption(arg, logConfig))
      {
        usage();
        return -1;
      }
      continue;
    }
    if (arg == kDeletedOption)
    {
      volumeOptions.include_deleted = true;
      continue;
    }
    if (arg == kRecoverOption)
    {
      volumeOptions.recover_errors = true;
      continue;
    }
    if (target != nullptr)
    {
      usage();
      return -1;
    }
    target = argv[i];
  }

  if (target == nullptr)
  {
    usage();
    return -1;
  }

  if (!Log::Configure(logConfig))
  {
    fprintf(stderr, "Cannot open log file %ls\n", logConfig.file_path.c_str());
  }

  SetConsoleOutputCP(CP_UTF8);

  const std::unique_ptr<NtfsVolume<Strategy::NO_CACHE>> volume =
      OpenVolume(target, volumeOptions);
  if (!volume->IsVolumeOK())
  {
    fprintf(stderr, "Cannot open %ls as an NTFS volume\n", target);
    return -1;
  }

  scanOptions.progress = [](ULONGLONG done, ULONGLONG total)
  {
    fprintf(stderr, "\rScanning $MFT: %llu / %llu", done, total);
    if (done == total)
    {
      fprintf(stderr, "\n");
    }
    return true;
  };

  const MftTree tree(*volume, scanOptions);

  printf("%10s %5s %-3s %14s %-16s %-6s %2s %s\n", "Record", "Seq", "", "Size",
         "Last write", "Attrib", "Ln", "Path");
  for (const MftEntry& entry : tree.Entries())
  {
    PrintEntry(tree, entry);
  }

  const MftScanStats& stats = tree.Stats();
  printf(
      "\nRecord slots: %llu, in use: %llu, deleted: %llu, extensions: %llu\n",
      stats.slots, stats.in_use, stats.deleted, stats.extensions);
  printf("Unreadable: %llu, damaged: %llu, not linked to the root: %llu\n",
         stats.unreadable, stats.damaged, stats.unreachable);

  return 0;
}
