// Lists every file the $MFT records, with its characteristics. Paths come
// from each record's own $FILE_NAME parent reference, not from directory
// indexes, so files of a directory whose index is lost still show up. With
// --recover, the change journal ($UsnJrnl) adds what it knows of each file, and
// lists the files only it names.

#include <ntfs-browser/win-types.h>

#include <array>
#include <cerrno>
#include <cstdio>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <ntfs-browser/log/log.h>
#include <ntfs-browser/mft/tree.h>
#include <ntfs-browser/namespace.h>
#include <ntfs-browser/ntfs-volume.h>
#include <ntfs-browser/usn/types.h>
#include <ntfs-browser/volume-options.h>

#include "device-path.h"
#include "local-time.h"
#include "text-convert.h"
#include "tool-support.h"

#ifdef _WIN32
  #include <cwctype>
#else
  #include "argv-wide.h"
  #include "posix-device-disk-reader.h"
#endif

using namespace NTFS_BROWSER_NAMESPACE;

namespace {

#ifndef _WIN32
// Bits in a DWORD: where a FILETIME's high half starts.
constexpr unsigned dword_bits = 32;
// Year a FILETIME of zero names.
constexpr unsigned filetime_epoch_year = 1601;
#endif

// Command-line switch that adds the records NTFS freed to the listing.
constexpr std::wstring_view deleted_option = L"--deleted";
// Command-line switch that salvages a damaged item instead of rejecting it.
constexpr std::wstring_view recover_option = L"--recover";

// Width of every column before the path, separators included, so an extra
// hard link's "=" line lines up under the path it follows.
constexpr int path_column = 63;

// Prints command-line usage help.
void Usage() {
  printf("Usage: ntfsmftlist [--log=...] [--deleted] [--recover] <volume>\n");
  printf("  %s\n", std::string(Log::option_usage).c_str());
  printf("  --deleted     also list the records NTFS freed\n");
  printf(
      "  --recover     salvage a damaged item instead of rejecting it, and\n");
  printf(
      "                read the change journal ($UsnJrnl): a ~ line gives\n");
  printf(
      "                what it knows of a file, and with --deleted the "
      "files\n");
  printf("                only it names follow the listing\n");
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

// Converts to UTF-8, which the console is switched to on Windows, so every
// name prints whatever the active code page or locale.
std::string ToUtf8(std::wstring_view text) {
  return NtfsDevice::WideToUtf8(text);
}

// The last write time of an entry in the host's local zone. A time the
// platform cannot show reads as all zeros.
NtfsDevice::LocalTime LocalWriteTime(const FILETIME& write_time) {
#ifdef _WIN32
  // The library already converts the time to local on Windows.
  SYSTEMTIME local{};
  FileTimeToSystemTime(&write_time, &local);
  return {.year = local.wYear,
          .month = local.wMonth,
          .day = local.wDay,
          .hour = local.wHour,
          .minute = local.wMinute};
#else
  // The library leaves the time in UTC off Windows.
  const ULONGLONG ticks =
      (static_cast<ULONGLONG>(write_time.dwHighDateTime) << dword_bits) |
      write_time.dwLowDateTime;
  // A record with no $STANDARD_INFORMATION keeps a time of zero, which
  // Windows shows as it is, so the zone does not move it.
  if (ticks == 0) {
    return {.year = filetime_epoch_year, .month = 1, .day = 1};
  }
  return NtfsDevice::UtcTicksToLocalTime(ticks).value_or(
      NtfsDevice::LocalTime{});
#endif
}

// The path of one name of entry, as shown in the listing.
std::string
    DisplayPath(const NTFS_BROWSER_NAMESPACE::Mft::MftTree& tree,
                const NTFS_BROWSER_NAMESPACE::Mft::MftTree::Entry& entry,
                std::optional<size_t> name_index) {
  std::optional<ULONGLONG> lost;
  const std::wstring path = name_index
                                ? tree.GetPath(entry.record, *name_index, &lost)
                                : tree.GetPath(entry.record, &lost);
  if (path.empty()) {
    return "<no name>";
  }
  if (lost) {
    return "<lost #" + std::to_string(*lost) + ">\\" + ToUtf8(path);
  }
  return ToUtf8(path);
}

// The letter of one attribute flag when it is set, '-' when it is not.
char FlagLetter(bool set, char letter) { return set ? letter : '-'; }

// The six attribute flags as letters, each shown when its flag is set.
std::string
    AttributeColumn(const NTFS_BROWSER_NAMESPACE::Mft::MftTree::Entry& entry) {
  return {FlagLetter(entry.read_only, 'R'), FlagLetter(entry.hidden, 'H'),
          FlagLetter(entry.system, 'S'),    FlagLetter(entry.compressed, 'C'),
          FlagLetter(entry.encrypted, 'E'), FlagLetter(entry.sparse, 'P')};
}

// The size in bytes, or <DIR> for a directory.
std::string
    SizeColumn(const NTFS_BROWSER_NAMESPACE::Mft::MftTree::Entry& entry) {
  if (entry.directory) {
    return "<DIR>";
  }
  return std::to_string(entry.size);
}

// Counts the names that are hard links, leaving out DOS 8.3 aliases.
size_t
    CountHardLinks(const NTFS_BROWSER_NAMESPACE::Mft::MftTree::Entry& entry) {
  size_t links = 0;
  for (const NTFS_BROWSER_NAMESPACE::Mft::MftTree::Name& name : entry.names) {
    if (!name.dos_only) {
      links++;
    }
  }
  return links;
}

// Prints the columns of entry, then its path.
void PrintMainLine(const NTFS_BROWSER_NAMESPACE::Mft::MftTree::Entry& entry,
                   const std::string& path) {
  const NtfsDevice::LocalTime local_time = LocalWriteTime(entry.write_time);

  printf("%10llu %5u %-3s %14s %04u-%02u-%02u %02u:%02u %s %2zu %s\n",
         static_cast<unsigned long long>(entry.record), entry.sequence,
         entry.in_use ? "" : "DEL", SizeColumn(entry).c_str(), local_time.year,
         local_time.month, local_time.day, local_time.hour, local_time.minute,
         AttributeColumn(entry).c_str(), CountHardLinks(entry), path.c_str());
}

// Prints one "=" line for each hard link whose path differs from path.
void PrintOtherLinks(const NTFS_BROWSER_NAMESPACE::Mft::MftTree& tree,
                     const NTFS_BROWSER_NAMESPACE::Mft::MftTree::Entry& entry,
                     const std::string& path) {
  for (size_t i = 0; i < entry.names.size(); i++) {
    // i < entry.names.size() by the loop condition.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    if (entry.names[i].dos_only) {
      continue;
    }
    const std::string other = DisplayPath(tree, entry, i);
    if (other != path) {
      printf("%*s= %s\n", path_column, "", other.c_str());
    }
  }
}

// One reason bit of a journal record, and the name the listing gives it.
struct ReasonName {
  Usn::Reason bit;
  std::string_view name;
};

// The reasons a journal record can give, in bit order.
constexpr std::array<ReasonName, 24> reason_names{{
    {Usn::Reason::DataOverwrite, "DataOverwrite"},
    {Usn::Reason::DataExtend, "DataExtend"},
    {Usn::Reason::DataTruncation, "DataTruncation"},
    {Usn::Reason::NamedDataOverwrite, "NamedDataOverwrite"},
    {Usn::Reason::NamedDataExtend, "NamedDataExtend"},
    {Usn::Reason::NamedDataTruncation, "NamedDataTruncation"},
    {Usn::Reason::FileCreate, "FileCreate"},
    {Usn::Reason::FileDelete, "FileDelete"},
    {Usn::Reason::EaChange, "EaChange"},
    {Usn::Reason::SecurityChange, "SecurityChange"},
    {Usn::Reason::RenameOldName, "RenameOldName"},
    {Usn::Reason::RenameNewName, "RenameNewName"},
    {Usn::Reason::IndexableChange, "IndexableChange"},
    {Usn::Reason::BasicInfoChange, "BasicInfoChange"},
    {Usn::Reason::HardLinkChange, "HardLinkChange"},
    {Usn::Reason::CompressionChange, "CompressionChange"},
    {Usn::Reason::EncryptionChange, "EncryptionChange"},
    {Usn::Reason::ObjectIdChange, "ObjectIdChange"},
    {Usn::Reason::ReparsePointChange, "ReparsePointChange"},
    {Usn::Reason::StreamChange, "StreamChange"},
    {Usn::Reason::TransactedChange, "TransactedChange"},
    {Usn::Reason::IntegrityChange, "IntegrityChange"},
    {Usn::Reason::DesiredStorageClassChange, "DesiredStorageClassChange"},
    {Usn::Reason::Close, "Close"},
}};

// The names of the reason bits set in reasons, joined by '|'.
std::string ReasonText(DWORD reasons) {
  std::string text;
  for (const ReasonName& reason : reason_names) {
    if ((reasons & static_cast<DWORD>(reason.bit)) != 0) {
      text += text.empty() ? "" : "|";
      text += reason.name;
    }
  }
  return text.empty() ? "-" : text;
}

// A time of the journal as "YYYY-MM-DD hh:mm".
std::string TimeText(const FILETIME& time) {
  const NtfsDevice::LocalTime local = LocalWriteTime(time);
  char text[32]{};
  snprintf(text, sizeof(text), "%04u-%02u-%02u %02u:%02u", local.year,
           local.month, local.day, local.hour, local.minute);
  return text;
}

// Prints what the journal knows of an entry, under its listing lines.
void PrintJournalLine(
    const NTFS_BROWSER_NAMESPACE::Mft::MftTree::UsnInfo& info) {
  printf("%*s~ %s %s", path_column, "", TimeText(info.last_time).c_str(),
         ReasonText(info.last_reason).c_str());
  if (info.delete_time) {
    printf(", deleted %s", TimeText(*info.delete_time).c_str());
  }
  printf(" (%llu records)\n",
         static_cast<unsigned long long>(info.record_count));
}

// Prints one listing line for entry, then one line per other hard link, then
// what the journal knows of it.
void PrintEntry(const NTFS_BROWSER_NAMESPACE::Mft::MftTree& tree,
                const NTFS_BROWSER_NAMESPACE::Mft::MftTree::Entry& entry) {
  const std::string path = DisplayPath(tree, entry, std::nullopt);
  PrintMainLine(entry, path);
  PrintOtherLinks(tree, entry, path);
  if (entry.usn) {
    PrintJournalLine(*entry.usn);
  }
}

// The path of one file only the journal names, as shown in the listing.
std::string JournalDisplayPath(const NTFS_BROWSER_NAMESPACE::Mft::MftTree& tree,
                               size_t index) {
  std::optional<ULONGLONG> lost;
  const std::wstring path = tree.GetJournalPath(index, &lost);
  if (lost) {
    return "<lost #" + std::to_string(*lost) + ">\\" + ToUtf8(path);
  }
  return ToUtf8(path);
}

// Prints the files only the journal names, after the listing.
void PrintJournalOnly(const NTFS_BROWSER_NAMESPACE::Mft::MftTree& tree) {
  const auto files = tree.JournalOnly();
  if (files.empty()) {
    return;
  }
  printf("\nFiles only the journal names: %zu\n", files.size());
  printf("%10s %5s %-5s %-16s %s\n", "Record", "Seq", "", "Last event",
         "Path [reasons]");
  for (size_t i = 0; i < files.size(); i++) {
    // i < files.size() by the loop condition.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    const auto& file = files[i];
    printf("%10llu %5u %-5s %-16s %s [%s]\n",
           static_cast<unsigned long long>(file.record), file.sequence,
           file.directory ? "<DIR>" : "", TimeText(file.usn.last_time).c_str(),
           JournalDisplayPath(tree, i).c_str(),
           ReasonText(file.usn.all_reasons).c_str());
  }
}

#ifdef _WIN32

// Opens target as a drive letter ("c" or "c:"), or else as a device or
// image path.
std::unique_ptr<NtfsVolume<Cache::Strategy::NoCache>>
    OpenVolume(std::wstring_view target, const VolumeOptions& options) {
  // A lone letter, optionally followed by a colon, names a drive.
  const bool drive_letter =
      (target.size() == 1 || (target.size() == 2 && target.back() == L':')) &&
      iswalpha(target.front()) != 0;
  if (drive_letter) {
    return std::make_unique<NtfsVolume<Cache::Strategy::NoCache>>(
        target.front(), options);
  }
  return std::make_unique<NtfsVolume<Cache::Strategy::NoCache>>(target,
                                                                options);
}

#else

// Opens target, a block device or an image file, through a POSIX reader.
// Returns null after saying why when it cannot.
std::unique_ptr<NtfsVolume<Cache::Strategy::NoCache>>
    OpenVolume(std::wstring_view target, const VolumeOptions& options) {
  // The whole volume is listed, so the target must name no directory.
  const std::optional<NtfsDevice::DeviceTarget> split =
      NtfsDevice::SplitDeviceTarget(target);
  if (!split || !split->directory.empty()) {
    return nullptr;
  }

  auto reader = std::make_unique<NtfsDevice::PosixDeviceDiskReader>();
  if (!reader->Open(NtfsDevice::WideFromPath(split->device))) {
    const int error = reader->LastError();
    if (error == EACCES || error == EPERM) {
      fprintf(stderr, "Cannot read device %s: read access is needed\n",
              split->device.c_str());
    }
    return nullptr;
  }
  return std::make_unique<NtfsVolume<Cache::Strategy::NoCache>>(
      std::move(reader), options);
}

#endif  // _WIN32

// What the command line asks for.
struct Arguments {
  Log::Config log_config;
  VolumeOptions volume_options;
  // Stays null until the command line names a volume.
  const wchar_t* target = nullptr;
};

// Fills args from the command line. Returns false on an unknown or malformed
// option, or unless exactly one volume is named.
bool ParseArguments(int argc, wchar_t* argv[], Arguments& args) {
  for (int i = 1; i < argc; i++) {
    const std::wstring_view arg(argv[i]);
    if (NtfsDevice::IsLogOption(arg)) {
      if (!NtfsDevice::ParseLogOption(arg, args.log_config)) {
        return false;
      }
      continue;
    }
    if (arg == deleted_option) {
      args.volume_options.include_deleted = true;
      continue;
    }
    if (arg == recover_option) {
      args.volume_options.recover_errors = true;
      continue;
    }
    if (args.target != nullptr) {
      return false;
    }
    args.target = argv[i];
  }
  return args.target != nullptr;
}

// Shows the $MFT scan's progress on stderr, and ends the line when it is done.
bool ShowScanProgress(ULONGLONG done, ULONGLONG total) {
  fprintf(stderr, "\rScanning $MFT: %llu / %llu",
          static_cast<unsigned long long>(done),
          static_cast<unsigned long long>(total));
  if (done == total) {
    fprintf(stderr, "\n");
  }
  return true;
}

// Prints the scan totals that close the listing.
void PrintStats(const NTFS_BROWSER_NAMESPACE::Mft::MftTree::ScanStats& stats) {
  printf(
      "\nRecord slots: %llu, in use: %llu, deleted: %llu, extensions: %llu\n",
      static_cast<unsigned long long>(stats.slots),
      static_cast<unsigned long long>(stats.in_use),
      static_cast<unsigned long long>(stats.deleted),
      static_cast<unsigned long long>(stats.extensions));
  printf("Unreadable: %llu, damaged: %llu, not linked to the root: %llu\n",
         static_cast<unsigned long long>(stats.unreadable),
         static_cast<unsigned long long>(stats.damaged),
         static_cast<unsigned long long>(stats.unreachable));
  if (stats.usn_records != 0) {
    printf(
        "Journal: %llu records, %llu files annotated, %llu only in the "
        "journal, %llu damaged%s\n",
        static_cast<unsigned long long>(stats.usn_records),
        static_cast<unsigned long long>(stats.usn_annotated),
        static_cast<unsigned long long>(stats.usn_journal_only),
        static_cast<unsigned long long>(stats.usn_damaged),
        stats.usn_truncated ? " (truncated)" : "");
  }
}

// Lists every $MFT record of the volume args names. Returns the exit code.
int ListVolume(const Arguments& args) {
  if (!Log::Configure(args.log_config)) {
    NtfsDevice::ReportLogFileError(args.log_config);
  }

#ifdef _WIN32
  SetConsoleOutputCP(CP_UTF8);
#endif

  const std::unique_ptr<NtfsVolume<Cache::Strategy::NoCache>> volume =
      OpenVolume(args.target, args.volume_options);
  if (!volume || !volume->IsVolumeOK()) {
    fprintf(stderr, "Cannot open %s as an NTFS volume\n",
            ToUtf8(args.target).c_str());
    return -1;
  }

  NTFS_BROWSER_NAMESPACE::Mft::MftTree::ScanOptions scan_options;
  scan_options.progress = ShowScanProgress;
  const NTFS_BROWSER_NAMESPACE::Mft::MftTree tree(*volume, scan_options);

  printf("%10s %5s %-3s %14s %-16s %-6s %2s %s\n", "Record", "Seq", "", "Size",
         "Last write", "Attrib", "Ln", "Path");
  for (const NTFS_BROWSER_NAMESPACE::Mft::MftTree::Entry& entry :
       tree.Entries()) {
    PrintEntry(tree, entry);
  }
  PrintJournalOnly(tree);
  PrintStats(tree.Stats());

  return 0;
}

// Parses the wide command line and lists the volume it names. Returns the
// exit code.
int Run(int argc, wchar_t* argv[]) {
  Arguments args;
  if (!ParseArguments(argc, argv, args)) {
    Usage();
    return -1;
  }
  return ListVolume(args);
}

}  // namespace

#ifdef _WIN32

// NOLINTNEXTLINE(readability-identifier-naming): wmain is the CRT entry point.
int wmain(int argc, wchar_t* argv[]) { return Run(argc, argv); }

#else

int main(int argc, char* argv[]) {
  std::vector<std::wstring> wide = NtfsDevice::ArgumentsAsWide(argc, argv);
  std::vector<wchar_t*> pointers = NtfsDevice::ArgumentPointers(wide);
  return Run(argc, pointers.data());
}

#endif
