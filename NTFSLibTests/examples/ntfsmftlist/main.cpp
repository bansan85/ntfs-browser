// Lists every file the $MFT records, with its characteristics. Paths come
// from each record's own $FILE_NAME parent reference, not from directory
// indexes, so files of a directory whose index is lost still show up. With
// --recover, the change journal ($UsnJrnl) adds what it knows of each file, and
// lists the files only it names.

#include <ntfs-browser/win-types.h>

#include <array>
#include <cerrno>
#include <charconv>
#include <cstdio>
#include <filesystem>
#include <format>
#include <functional>
#include <iterator>
#include <memory>
#include <optional>
#include <span>
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
#include "ntfs-browser/cache/strategy.h"
#include "text-convert.h"
#include "tool-support.h"

#ifdef _WIN32
  #include <cwctype>

  #include <fcntl.h>
  #include <io.h>
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

// Widths of the record, sequence, DEL, size and hard link columns: wide enough
// for the values a volume holds, and with the 16 of the date and the 6 of the
// attributes, plus a space after each, they add up to path_column.
constexpr size_t record_width = 10;
constexpr size_t sequence_width = 5;
constexpr size_t state_width = 3;
constexpr size_t size_width = 14;
constexpr size_t links_width = 2;

// Bytes of the listing gathered before stdout gets them. 1 MiB turns a listing
// of millions of lines into a few dozen writes.
constexpr size_t output_flush_bytes = size_t{1024} * 1024;

// Digits of the largest ULONGLONG.
constexpr size_t max_decimal_digits = 20;

#ifdef _WIN32
// The end of a line of the listing. stdout is binary while it is written, so
// the listing adds the CR the text mode would: that mode converts a few KiB at
// a time, and each chunk costs a write.
constexpr std::string_view line_end = "\r\n";
#else
constexpr std::string_view line_end = "\n";
#endif

// Gathers the listing, and hands it to stdout in big chunks. A chunk ends
// between two pieces of text, so a console never gets a UTF-8 sequence cut in
// two. The main line of each entry is built by hand: std::format() would cost
// more than the rest of the line.
class Output {
 public:
  Output() {
    buffer_.reserve(output_flush_bytes);
#ifdef _WIN32
    fflush(stdout);
    text_mode_ = _setmode(_fileno(stdout), _O_BINARY);
#endif
  }

  Output(const Output&) = delete;
  Output(Output&&) = delete;
  Output& operator=(const Output&) = delete;
  Output& operator=(Output&&) = delete;

  ~Output() {
    Flush();
#ifdef _WIN32
    fflush(stdout);
    if (text_mode_ != -1) {
      _setmode(_fileno(stdout), text_mode_);
    }
#endif
  }

  // Appends one piece of the listing, formatted as std::format() does.
  template <class... Args>
  void Print(std::format_string<Args...> fmt, Args&&... args) {
    std::format_to(std::back_inserter(buffer_), fmt,
                   std::forward<Args>(args)...);
    FlushIfFull();
  }

  // Same, then ends the line.
  template <class... Args>
  void PrintLine(std::format_string<Args...> fmt, Args&&... args) {
    std::format_to(std::back_inserter(buffer_), fmt,
                   std::forward<Args>(args)...);
    EndLine();
  }

  void Append(std::string_view text) { buffer_.append(text); }

  // Appends text right-aligned in width columns, or left-aligned.
  void AppendRight(std::string_view text, size_t width) {
    Pad(text.size(), width, ' ');
    buffer_.append(text);
  }

  void AppendLeft(std::string_view text, size_t width) {
    buffer_.append(text);
    Pad(text.size(), width, ' ');
  }

  // Appends value in decimal, right-aligned in width columns filled with fill.
  void AppendNumber(ULONGLONG value, size_t width, char fill = ' ') {
    std::array<char, max_decimal_digits> digits{};
    const char* end =
        std::to_chars(digits.data(), digits.data() + digits.size(), value).ptr;
    const auto length = static_cast<size_t>(end - digits.data());
    Pad(length, width, fill);
    buffer_.append(digits.data(), length);
  }

  // Ends the current line.
  void EndLine() {
    buffer_.append(line_end);
    FlushIfFull();
  }

  // Writes to stdout what is gathered so far.
  void Flush() {
    fwrite(buffer_.data(), 1, buffer_.size(), stdout);
    buffer_.clear();
  }

 private:
  // Appends the fill a text of length characters needs to span width columns.
  void Pad(size_t length, size_t width, char fill) {
    if (length < width) {
      buffer_.append(width - length, fill);
    }
  }

  void FlushIfFull() {
    if (buffer_.size() >= output_flush_bytes) {
      Flush();
    }
  }

  std::string buffer_;
#ifdef _WIN32
  // The mode stdout had before, restored once the listing is written. -1 when
  // it could not be changed.
  int text_mode_{-1};
#endif
};

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

// Replaces out with the path of one name of entry, as shown in the listing.
void DisplayPath(const NTFS_BROWSER_NAMESPACE::Mft::MftTree& tree,
                 const NTFS_BROWSER_NAMESPACE::Mft::MftTree::Entry& entry,
                 std::optional<size_t> name_index, std::string& out) {
  out.clear();
  std::optional<ULONGLONG> lost;
  const std::wstring path = name_index
                                ? tree.GetPath(entry.record, *name_index, &lost)
                                : tree.GetPath(entry.record, &lost);
  if (path.empty()) {
    out = "<no name>";
    return;
  }
  if (lost) {
    out += "<lost #";
    out += std::to_string(*lost);
    out += ">\\";
  }
  NtfsDevice::AppendAsUtf8(out, path);
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
void PrintOtherLinks(Output& out,
                     const NTFS_BROWSER_NAMESPACE::Mft::MftTree& tree,
                     const NTFS_BROWSER_NAMESPACE::Mft::MftTree::Entry& entry,
                     const std::string& path) {
  std::string other;
  for (size_t i = 0; i < entry.names.size(); i++) {
    // i < entry.names.size() by the loop condition.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    if (entry.names[i].dos_only) {
      continue;
    }
    DisplayPath(tree, entry, i, other);
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

// Prints one listing line for entry, then one line per other hard link, then
// what the journal knows of it. path is scratch space, reused from one entry to
// the next.
void PrintEntry(Output& out, const NTFS_BROWSER_NAMESPACE::Mft::MftTree& tree,
                const NTFS_BROWSER_NAMESPACE::Mft::MftTree::Entry& entry,
                std::string& path) {
  DisplayPath(tree, entry, std::nullopt, path);
  PrintMainLine(out, entry, path);
  // A lone non-DOS name is the one the path above went through.
  if (CountHardLinks(entry) > 1) {
    PrintOtherLinks(out, tree, entry, path);
  }
  if (entry.usn) {
    PrintJournalLine(out, *entry.usn);
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
void PrintStats(Output& out,
                const NTFS_BROWSER_NAMESPACE::Mft::MftTree::ScanStats& stats) {
  out.EndLine();
  out.PrintLine("Record slots: {}, in use: {}, deleted: {}, extensions: {}",
                stats.slots, stats.in_use, stats.deleted, stats.extensions);
  out.PrintLine("Unreadable: {}, damaged: {}, not linked to the root: {}",
                stats.unreadable, stats.damaged, stats.unreachable);
  if (stats.usn_records != 0) {
    out.PrintLine(
        "Journal: {} records, {} files annotated, {} only in the journal, {} "
        "damaged{}",
        stats.usn_records, stats.usn_annotated, stats.usn_journal_only,
        stats.usn_damaged, stats.usn_truncated ? " (truncated)" : "");
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

  Output out;
  out.PrintLine("{:>10} {:>5} {:<3} {:>14} {:<16} {:<6} {:>2} {}", "Record",
                "Seq", "", "Size", "Last write", "Attrib", "Ln", "Path");
  std::string path;
  for (const NTFS_BROWSER_NAMESPACE::Mft::MftTree::Entry& entry :
       tree.Entries()) {
    PrintEntry(out, tree, entry, path);
  }
  PrintJournalOnly(out, tree);
  PrintStats(out, tree.Stats());

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
