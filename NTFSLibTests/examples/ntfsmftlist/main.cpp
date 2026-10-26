// Lists every file the $MFT records, with its characteristics. Paths come
// from each record's own $FILE_NAME parent reference, not from directory
// indexes, so files of a directory whose index is lost still show up. With
// --recover, the change journal ($UsnJrnl) adds what it knows of each file, and
// lists the files only it names. With --clusters, each freed file, and each
// file that is damaged or salvaged, gets a line that counts its clusters by
// condition: intact, rewritten by another file, and so on.

#include <ntfs-browser/win-types.h>

#include <array>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include <ntfs-browser/alloc/cluster-account.h>
#include <ntfs-browser/log/log.h>
#include <ntfs-browser/mft/idx.h>
#include <ntfs-browser/mft/tree.h>
#include <ntfs-browser/namespace.h>
#include <ntfs-browser/ntfs-volume.h>
#include <ntfs-browser/usn/types.h>
#include <ntfs-browser/volume-options.h>

#include "device-path.h"
#include "local-time.h"
#include "ntfs-browser/cache/strategy.h"
#include "output.h"
#include "text-convert.h"
#include "tool-support.h"

#ifdef _WIN32
  #include <cwctype>

#else
  #include "argv-wide.h"
  #include "posix-device-disk-reader.h"
#endif

using namespace NTFS_BROWSER_NAMESPACE;
using NtfsDevice::Output;

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
// Command-line prefix of the switch that sets how many threads scan $MFT.
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

// Width of every column before the path, separators included, so an extra
// hard link's "=" line lines up under the path it follows.
constexpr int path_column = 63;

// Widths of the record, sequence, DEL, size and hard link columns: wide enough
// for the values a volume holds, and with the 16 of the date and the 6 of the
// attributes, plus a space after each, they add up to path_column.
constexpr size_t record_width = 10;
constexpr size_t sequence_width = 5;
constexpr size_t state_width = 3;
constexpr size_t size_width = 14;
constexpr size_t links_width = 2;

// The record of the root directory, where a path starts.
constexpr ULONGLONG root_record = static_cast<ULONGLONG>(Mft::Idx::Root);

// Bytes of directory path DisplayPaths keeps at most, in all. A chain of
// directories that runs deep has paths that grow with the depth, so keeping
// each one costs memory that grows with its square. 64 MiB is far above what a
// real volume needs.
constexpr size_t max_kept_path_bytes = size_t{64} << 20;

// Bytes a directory path can have and still be kept. A longer path is built
// each time it is asked for, which costs no more than printing it.
constexpr size_t max_kept_path_length = 4096;

// Prints command-line usage help.
void Usage() {
  printf(
      "Usage: ntfsmftlist [--log=...] [--deleted] [--recover] [--threads=N] "
      "[--clusters[=map]] [--verify-data[=sample|full]] <volume>\n");
  printf("  %s\n", std::string(Log::option_usage).c_str());
  printf("  --deleted     also list the records NTFS freed\n");
  printf(
      "  --threads=N   threads that parse $MFT, 1 for one; by default the\n");
  printf("                machine's, on a volume big enough to pay for them\n");
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

// The name MftTree::GetPath() goes through when it is given none: the first
// non-DOS name with a valid parent, else the first non-DOS name, else the
// first name. Empty when entry has no name.
std::optional<size_t>
    PrimaryName(const NTFS_BROWSER_NAMESPACE::Mft::MftTree::Entry& entry) {
  std::optional<size_t> fallback;
  for (size_t i = 0; i < entry.names.size(); i++) {
    // i < entry.names.size() by the loop condition.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    const NTFS_BROWSER_NAMESPACE::Mft::MftTree::Name& name = entry.names[i];
    if (name.dos_only) {
      continue;
    }
    if (name.parent_valid) {
      return i;
    }
    if (!fallback) {
      fallback = i;
    }
  }
  if (fallback) {
    return fallback;
  }
  return entry.names.empty() ? std::nullopt : std::optional<size_t>{0};
}

// Appends the "<lost #N>\" that starts a path relative to the lost record N.
void AppendLostPrefix(std::string& out, const std::optional<ULONGLONG>& lost) {
  if (lost) {
    out += "<lost #";
    out += std::to_string(*lost);
    out += ">\\";
  }
}

// Builds the paths of the listing, the ones MftTree::GetPath() gives, in
// UTF-8. Converting the whole path of each file costs more than the rest of
// its line, so the path of each directory is converted once and kept: a file's
// path is its directory's, plus its own name.
class DisplayPaths {
 public:
  explicit DisplayPaths(const NTFS_BROWSER_NAMESPACE::Mft::MftTree& tree)
      : tree_(tree) {}

  // Replaces out with the path of entry through entry.names[name_index], or
  // through its primary name when name_index is empty, as the listing shows
  // it.
  void Display(const NTFS_BROWSER_NAMESPACE::Mft::MftTree::Entry& entry,
               std::optional<size_t> name_index, std::string& out) {
    out.clear();
    const std::optional<size_t> primary = PrimaryName(entry);
    const std::optional<size_t> index = name_index ? name_index : primary;
    if (!index || *index >= entry.names.size()) {
      out = "<no name>";
      return;
    }

    if (entry.directory && index == primary) {
      const Directory& own = DirectoryOf(entry.record);
      if (own.path.empty()) {
        out = "<no name>";
        return;
      }
      AppendLostPrefix(out, own.lost);
      out += own.path;
      return;
    }

    // *index < entry.names.size() was checked above.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    const NTFS_BROWSER_NAMESPACE::Mft::MftTree::Name& name =
        entry.names[*index];
    // A valid parent other than the root is a directory the scan found, whose
    // path GetPath() gives through its primary name, as it does for its files.
    const bool through_directory =
        !entry.directory && entry.record != root_record && name.parent_valid &&
        name.parent_record != root_record;
    if (through_directory) {
      const NTFS_BROWSER_NAMESPACE::Mft::MftTree::Entry* parent =
          tree_.Find(name.parent_record);
      if (parent != nullptr && !parent->names.empty()) {
        const Directory& directory = DirectoryOf(name.parent_record);
        AppendLostPrefix(out, directory.lost);
        out += directory.path;
        out += '\\';
        NtfsDevice::AppendAsUtf8(out, name.name);
        return;
      }
    }

    std::optional<ULONGLONG> lost;
    const std::wstring path = tree_.GetPath(entry.record, *index, &lost);
    if (path.empty()) {
      out = "<no name>";
      return;
    }
    AppendLostPrefix(out, lost);
    NtfsDevice::AppendAsUtf8(out, path);
  }

 private:
  // The path of a directory through its primary name, and the record where
  // its chain of valid parent references breaks, if it does.
  struct Directory {
    std::optional<ULONGLONG> lost;
    std::string path;
  };

  // The path of the directory record, converted the first time. A path that is
  // not kept is valid until the next call.
  const Directory& DirectoryOf(ULONGLONG record) {
    if (const auto found = directories_.find(record);
        found != directories_.end()) {
      return found->second;
    }
    Directory directory;
    const std::wstring path = tree_.GetPath(record, &directory.lost);
    NtfsDevice::AppendAsUtf8(directory.path, path);
    // A path that is too long, or that the budget has no room for, is not kept.
    if (directory.path.size() > max_kept_path_length ||
        kept_bytes_ + directory.path.size() > max_kept_path_bytes) {
      unkept_ = std::move(directory);
      return unkept_;
    }
    kept_bytes_ += directory.path.size();
    return directories_.emplace(record, std::move(directory)).first->second;
  }

  const NTFS_BROWSER_NAMESPACE::Mft::MftTree& tree_;
  std::unordered_map<ULONGLONG, Directory> directories_;
  // Bytes of path directories_ holds.
  size_t kept_bytes_{0};
  // The last path DirectoryOf() did not keep.
  Directory unkept_;
};

// The letter of one attribute flag when it is set, '-' when it is not.
char FlagLetter(bool set, char letter) { return set ? letter : '-'; }

// The six attribute flags as letters, each shown when its flag is set.
std::string
    AttributeColumn(const NTFS_BROWSER_NAMESPACE::Mft::MftTree::Entry& entry) {
  return {FlagLetter(entry.read_only, 'R'), FlagLetter(entry.hidden, 'H'),
          FlagLetter(entry.system, 'S'),    FlagLetter(entry.compressed, 'C'),
          FlagLetter(entry.encrypted, 'E'), FlagLetter(entry.sparse, 'P')};
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
void PrintMainLine(Output& out,
                   const NTFS_BROWSER_NAMESPACE::Mft::MftTree::Entry& entry,
                   const std::string& path) {
  const NtfsDevice::LocalTime local_time = LocalWriteTime(entry.write_time);

  out.AppendNumber(entry.record, record_width);
  out.Append(" ");
  out.AppendNumber(entry.sequence, sequence_width);
  out.Append(" ");
  out.AppendLeft(entry.in_use ? "" : "DEL", state_width);
  out.Append(" ");
  if (entry.directory) {
    out.AppendRight("<DIR>", size_width);
  } else {
    out.AppendNumber(entry.size, size_width);
  }
  out.Append(" ");
  out.AppendNumber(local_time.year, 4, '0');
  out.Append("-");
  out.AppendNumber(local_time.month, 2, '0');
  out.Append("-");
  out.AppendNumber(local_time.day, 2, '0');
  out.Append(" ");
  out.AppendNumber(local_time.hour, 2, '0');
  out.Append(":");
  out.AppendNumber(local_time.minute, 2, '0');
  out.Append(" ");
  out.Append(AttributeColumn(entry));
  out.Append(" ");
  out.AppendNumber(CountHardLinks(entry), links_width);
  out.Append(" ");
  out.Append(path);
  out.EndLine();
}

// Prints one "=" line for each hard link whose path differs from path.
void PrintOtherLinks(Output& out, DisplayPaths& paths,
                     const NTFS_BROWSER_NAMESPACE::Mft::MftTree::Entry& entry,
                     const std::string& path) {
  std::string other;
  for (size_t i = 0; i < entry.names.size(); i++) {
    // i < entry.names.size() by the loop condition.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    if (entry.names[i].dos_only) {
      continue;
    }
    paths.Display(entry, i, other);
    if (other != path) {
      out.PrintLine("{:{}}= {}", "", path_column, other);
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
    Output& out, const NTFS_BROWSER_NAMESPACE::Mft::MftTree::UsnInfo& info) {
  out.Print("{:{}}~ {} {}", "", path_column, TimeText(info.last_time),
            ReasonText(info.last_reason));
  if (info.delete_time) {
    out.Print(", deleted {}", TimeText(*info.delete_time));
  }
  out.PrintLine(" ({} records)", info.record_count);
}

// The non-zero counters of tally, as "name count, name count".
std::string TallyText(const Alloc::ClusterTally& tally) {
  std::string text;
  for (size_t i = 0; i < Alloc::cluster_condition_count; i++) {
    const auto condition = static_cast<Alloc::ClusterCondition>(i);
    if (tally.Count(condition) == 0) {
      continue;
    }
    text += text.empty() ? "" : ", ";
    text += Alloc::ConditionName(condition);
    text += " " + std::to_string(tally.Count(condition));
  }
  return text.empty() ? "-" : text;
}

// Prints what the scan counted of an entry's clusters, and with the map, the
// byte ranges of each stream in file order.
void PrintClusterLines(Output& out, const Alloc::ClusterAccount& account,
                       ULONGLONG cluster_size) {
  out.PrintLine("{:{}}# clusters: {}", "", path_column,
                TallyText(account.tally));
  for (const Alloc::StreamClusterMap& map : account.streams) {
    out.Print("{:{}}# stream {}:", "", path_column,
              map.name.empty() ? std::string("(unnamed)") : ToUtf8(map.name));
    for (const Alloc::StreamByteRange& range :
         Alloc::ToByteRanges(map, cluster_size)) {
      out.Print(" {}..{} {}", range.offset, range.offset + range.length,
                Alloc::ConditionName(range.condition));
    }
    out.EndLine();
  }
}

// Prints one listing line for entry, then one line per other hard link, then
// what the journal knows of it, then what is known of its clusters. path is
// scratch space, reused from one entry to the next.
void PrintEntry(Output& out, DisplayPaths& paths,
                const NTFS_BROWSER_NAMESPACE::Mft::MftTree::Entry& entry,
                const Alloc::ClusterAccount* account, ULONGLONG cluster_size,
                std::string& path) {
  paths.Display(entry, std::nullopt, path);
  PrintMainLine(out, entry, path);
  // A lone non-DOS name is the one the path above went through.
  if (CountHardLinks(entry) > 1) {
    PrintOtherLinks(out, paths, entry, path);
  }
  if (entry.usn) {
    PrintJournalLine(out, *entry.usn);
  }
  if (account != nullptr) {
    PrintClusterLines(out, *account, cluster_size);
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
void PrintJournalOnly(Output& out,
                      const NTFS_BROWSER_NAMESPACE::Mft::MftTree& tree) {
  const auto files = tree.JournalOnly();
  if (files.empty()) {
    return;
  }
  out.EndLine();
  out.PrintLine("Files only the journal names: {}", files.size());
  out.PrintLine("{:>10} {:>5} {:<5} {:<16} {}", "Record", "Seq", "",
                "Last event", "Path [reasons]");
  for (size_t i = 0; i < files.size(); i++) {
    // i < files.size() by the loop condition.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    const auto& file = files[i];
    out.PrintLine("{:>10} {:>5} {:<5} {:<16} {} [{}]", file.record,
                  file.sequence, file.directory ? "<DIR>" : "",
                  TimeText(file.usn.last_time), JournalDisplayPath(tree, i),
                  ReasonText(file.usn.all_reasons));
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
  // Threads that scan $MFT, 0 for the library's choice.
  unsigned threads = 0;
  Alloc::ClusterAccountLevel clusters = Alloc::ClusterAccountLevel::Off;
  Alloc::DataCheckMode verify = Alloc::DataCheckMode::Off;
  // Stays null until the command line names a volume.
  const wchar_t* target = nullptr;
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
    if (arg == clusters_option || arg == clusters_map_option) {
      args.clusters = arg == clusters_map_option
                          ? Alloc::ClusterAccountLevel::CountsAndMap
                          : Alloc::ClusterAccountLevel::Counts;
      continue;
    }
    if (arg == verify_option || arg == verify_sample_option ||
        arg == verify_full_option) {
      args.verify = arg == verify_full_option ? Alloc::DataCheckMode::Full
                                              : Alloc::DataCheckMode::Sample;
      continue;
    }
    if (arg.starts_with(threads_option)) {
      if (!ParseThreads(arg.substr(threads_option.size()), args.threads)) {
        return false;
      }
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
// An update takes a write of stderr, which costs milliseconds when that is a
// file on a busy disk: the line is redrawn a few times a second, not once per
// call.
class ScanProgress {
 public:
  bool operator()(ULONGLONG done, ULONGLONG total) {
    const auto now = std::chrono::steady_clock::now();
    if (done != total && drawn_ && now - last_draw_ < redraw_interval) {
      return true;
    }
    drawn_ = true;
    last_draw_ = now;
    // One write per update: stderr is unbuffered, and fprintf() would write it
    // piece by piece.
    std::array<char, 80> line{};
    const int length = snprintf(
        line.data(), line.size(), "\rScanning $MFT: %llu / %llu%s",
        static_cast<unsigned long long>(done),
        static_cast<unsigned long long>(total), done == total ? "\n" : "");
    if (length > 0) {
      fwrite(line.data(), 1, static_cast<size_t>(length), stderr);
    }
    return true;
  }

 private:
  // Time between two redraws of the progress line: often enough to look alive.
  static constexpr std::chrono::milliseconds redraw_interval{100};

  std::chrono::steady_clock::time_point last_draw_;
  bool drawn_{false};
};

// Prints the scan totals that close the listing. clusters says the scan
// counted clusters.
void PrintStats(Output& out,
                const NTFS_BROWSER_NAMESPACE::Mft::MftTree::ScanStats& stats,
                bool clusters) {
  out.EndLine();
  out.PrintLine(
      "Record slots: {}, in use: {}, deleted: {} ({} with an overwritten "
      "attribute list), extensions: {} ({} reused by others)",
      stats.slots, stats.in_use, stats.deleted, stats.overwritten_lists,
      stats.extensions, stats.reused_extensions);
  out.PrintLine("Unreadable: {}, damaged: {}, not linked to the root: {}",
                stats.unreadable, stats.damaged, stats.unreachable);
  if (stats.usn_records != 0) {
    out.PrintLine(
        "Journal: {} records, {} files annotated, {} only in the journal, {} "
        "damaged{}",
        stats.usn_records, stats.usn_annotated, stats.usn_journal_only,
        stats.usn_damaged, stats.usn_truncated ? " (truncated)" : "");
  }
  if (clusters) {
    out.PrintLine("Clusters of {} files: {}", stats.cluster_accounts,
                  TallyText(stats.clusters));
  }
}

// Lists every $MFT record of the volume args names. Returns the exit code.
int ListVolume(const Arguments& args) {
  if (!Log::Configure(args.log_config)) {
    NtfsDevice::ReportLogFileError(args.log_config);
  }

  // Names print as UTF-8: a console shows them right with its code page set.
  const NtfsDevice::Utf8ConsoleOutput console;

  const std::unique_ptr<NtfsVolume<Cache::Strategy::NoCache>> volume =
      OpenVolume(args.target, args.volume_options);
  if (!volume || !volume->IsVolumeOK()) {
    fprintf(stderr, "Cannot open %s as an NTFS volume\n",
            ToUtf8(args.target).c_str());
    return -1;
  }

  NTFS_BROWSER_NAMESPACE::Mft::MftTree::ScanOptions scan_options;
  scan_options.progress = ScanProgress{};
  scan_options.threads = args.threads;
  scan_options.cluster_account = args.clusters;
  scan_options.verify_data = args.verify;
  // Never freed: the process ends with the listing, and freeing millions of
  // entries one by one would take seconds of it. A static keeps it reachable.
  // NOLINTNEXTLINE(cppcoreguidelines-owning-memory)
  static const NTFS_BROWSER_NAMESPACE::Mft::MftTree& tree =
      *new NTFS_BROWSER_NAMESPACE::Mft::MftTree(*volume, scan_options);

  Output out;
  out.PrintLine("{:>10} {:>5} {:<3} {:>14} {:<16} {:<6} {:>2} {}", "Record",
                "Seq", "", "Size", "Last write", "Attrib", "Ln", "Path");
  DisplayPaths paths(tree);
  std::string path;
  const ULONGLONG cluster_size = volume->GetClusterSize();
  const bool counted = args.clusters != Alloc::ClusterAccountLevel::Off ||
                       args.verify != Alloc::DataCheckMode::Off;
  for (const NTFS_BROWSER_NAMESPACE::Mft::MftTree::Entry& entry :
       tree.Entries()) {
    PrintEntry(out, paths, entry,
               counted ? tree.ClusterAccountOf(entry.record) : nullptr,
               cluster_size, path);
  }
  PrintJournalOnly(out, tree);
  PrintStats(out, tree.Stats(), counted);

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
