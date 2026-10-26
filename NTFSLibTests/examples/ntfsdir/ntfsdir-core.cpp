#include "ntfsdir-core.h"

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

#include <ntfs-browser/index-entry.h>
#include <ntfs-browser/io/file-record.h>
#include <ntfs-browser/io/record-standing.h>
#include <ntfs-browser/log/log.h>
#include <ntfs-browser/mft/idx.h>
#include <ntfs-browser/namespace.h>
#include <ntfs-browser/ntfs-volume.h>
#include <ntfs-browser/volume-options.h>

#include "device-path.h"
#include "local-time.h"
#include "ntfs-browser/attr/mask.h"
#include "ntfs-browser/cache/strategy.h"
#include "ntfs-browser/win-types.h"
#include "output.h"
#include "text-convert.h"
#include "tool-support.h"

namespace NTFS_BROWSER_NAMESPACE {

template <Cache::Strategy S>
class NtfsVolume;

}  // namespace NTFS_BROWSER_NAMESPACE

#ifndef _WIN32
  #include "posix-device-disk-reader.h"
#endif

using namespace NTFS_BROWSER_NAMESPACE;

void Usage() {
  printf("Invalid parameter\n");
  printf(
      "Usage: ntfsdir [--recursive] [--deleted] [--recover] [--log=...] "
      "\"path\"\n");
  printf("  --recursive, -r  also list every subdirectory of path\n");
  printf("  --deleted        also list deleted files, marked with *\n");
  printf(
      "  --recover        salvage damaged directory data (? marks a name "
      "whose file record cannot be read)\n");
  printf("  %s\n", std::string(Log::option_usage).c_str());
#ifdef _WIN32
  printf("eg. ntfsdir c:\n");
  printf("eg. ntfsdir c:\\windows\n");
  printf("eg. ntfsdir \"c:\\program files\\common files\"\n");
  printf("eg. ntfsdir c:\\image.img\\Windows\n");
#else
  printf("eg. ntfsdir /dev/sda1\n");
  printf("eg. ntfsdir /dev/sda1/Windows\n");
  printf("eg. ntfsdir \"/dev/sda1/Program Files/Common Files\"\n");
  printf("eg. ntfsdir ./disk.img/Windows\n");
#endif
}

wchar_t Getvolume(wchar_t** ppath) {
  wchar_t* p = *ppath;

  while (*p == L' ' || *p == L'"') {
    p++;
  }
  if (*p == L'\0') {
    return L'\0';
  }
  const wchar_t volname = *p++;

  while (*p == L' ') {
    p++;
  }
  if (*p != L':') {
    return L'\0';
  }

  // Skips the colon and the first directory name, up to a backslash.
  while (*p != L'\\' && *p != L'\0') {
    p++;
  }
  while (*p == L'\\' || *p == L'"') {
    p++;
  }

  *ppath = p;
  return volname;
}

std::wstring_view Getpathname(std::wstring_view& ppath) {
  const auto is_separator = [](wchar_t character) {
    return character == L'\\' || character == L'"';
  };

  // The first character belongs to the segment whatever it is.
  size_t length = ppath.empty() ? 0 : 1;
  while (length < ppath.size() && !is_separator(ppath[length])) {
    length++;
  }
  const std::wstring_view pathname = ppath.substr(0, length);

  size_t next = length;
  while (next < ppath.size() && is_separator(ppath[next])) {
    next++;
  }
  // Moves the start of the view: a path of many segments costs no copy.
  ppath.remove_prefix(next);
  return pathname;
}

namespace {

#ifndef _WIN32
// Bits in a DWORD: where a FILETIME's high half starts.
constexpr unsigned dword_bits = 32;
#endif

// Prints message and a newline to stdout, before the listing starts.
void Say(const std::wstring& message) {
  NtfsDevice::WriteText(stdout, message);
  printf("\n");
}

// Prints message and a newline in the listing.
void Report(NtfsDevice::Output& out, const std::wstring& message) {
  out.AppendText(message);
  out.EndLine();
}

// Reports whether the listing shows ie. Hides system metafiles, DOS alias
// file names and entries with no name.
bool IsListed(const IndexEntryView& ie) {
  // Record numbers below the first user record are system metafiles.
  if (ie.GetFileReference() < static_cast<ULONGLONG>(Mft::Idx::User)) {
    return false;
  }

  // Ignore DOS alias file names
  if (!ie.IsWin32Name()) {
    return false;
  }

  return !ie.GetFilename().empty();
}

// The modification time of ie in the host's local zone, or nothing for a
// time the platform cannot show.
std::optional<NtfsDevice::LocalTime> LocalWriteTime(const IndexEntryView& ie) {
  FILETIME ft;
  ie.GetFileTime(&ft, nullptr, nullptr);
#ifdef _WIN32
  // The library already converts the time to local on Windows.
  SYSTEMTIME st;
  if (FileTimeToSystemTime(&ft, &st) != TRUE) {
    return std::nullopt;
  }
  return NtfsDevice::LocalTime{.year = st.wYear,
                               .month = st.wMonth,
                               .day = st.wDay,
                               .hour = st.wHour,
                               .minute = st.wMinute};
#else
  // The library leaves the time in UTC off Windows.
  return NtfsDevice::UtcTicksToLocalTime(
      (static_cast<ULONGLONG>(ft.dwHighDateTime) << dword_bits) |
      ft.dwLowDateTime);
#endif
}

// Marks an entry in a listing that shows deleted items: ' ' for a file
// that exists, '*' for a deleted one, '?' for a name whose record cannot be
// read.
using Marker = char;

// Prints the modification time, the size or <DIR>, the attribute flags and
// the name of one entry. time is the entry time in local time. marker is
// null when the listing has no marker column.
void PrintLine(NtfsDevice::Output& out, const IndexEntryView& ie,
               const NtfsDevice::LocalTime& time,
               std::optional<Marker> marker) {
  out.AppendNumber(time.year, 0);
  out.Append("-");
  out.AppendNumber(time.month, 2, '0');
  out.Append("-");
  out.AppendNumber(time.day, 2, '0');
  out.Append("  ");
  out.AppendNumber(time.hour, 2, '0');
  out.Append(":");
  out.AppendNumber(time.minute, 2, '0');
  out.Append(ie.IsDirectory() ? "\t<DIR>    " : "\t         ");

  if (!ie.IsDirectory()) {
    out.AppendNumber(ie.GetFileSize(), 0);
  }
  out.Append("\t<");
  out.Append(ie.IsReadOnly() ? "R" : " ");
  out.Append(ie.IsHidden() ? "H" : " ");
  out.Append(ie.IsSystem() ? "S" : " ");
  out.Append(">\t");
  if (marker) {
    out.Append(std::string_view(&*marker, 1));
    out.Append(" ");
  }
  out.AppendText(ie.GetFilename());
  out.EndLine();
}

// Separates the directory names of a path in the headers of a recursive
// listing.
#ifdef _WIN32
constexpr wchar_t path_separator = L'\\';
#else
constexpr wchar_t path_separator = L'/';
#endif

// The marker column of an entry whose record stands as given.
Marker MarkerOf(Io::RecordStanding standing) {
  switch (standing) {
    case Io::RecordStanding::Live:
      return ' ';
    case Io::RecordStanding::Unreadable:
      return '?';
    case Io::RecordStanding::Deleted:
    case Io::RecordStanding::Reused:
      break;
  }
  return '*';
}

// What one TraverseSubEntriesWithStanding pass over a directory gathers: the
// tally of its listed entries, and the subdirectories to enter next.
template <Cache::Strategy S>
struct DirectoryEntries {
  struct Child {
    ULONGLONG reference;
    std::wstring name;
  };

  // Where the listing goes.
  NtfsDevice::Output* out = nullptr;
  // Whether the listing has a marker column: only a volume that shows deleted
  // items does.
  bool marking = false;
  // Whether to collect the subdirectories.
  bool recursive = false;
  Total total;
  std::vector<Child> children;
};

// TraverseSubEntriesWithStanding callback: prints one entry, and tallies it
// and, in a recursive listing, collects a subdirectory that is still the
// entry's own.
template <Cache::Strategy S>
void VisitEntry(const IndexEntryView& ie, Io::RecordStanding standing,
                void* context) {
  if (!IsListed(ie)) {
    return;
  }
  auto& entries = *static_cast<DirectoryEntries<S>*>(context);

  if (const std::optional<NtfsDevice::LocalTime> time = LocalWriteTime(ie)) {
    PrintLine(*entries.out, ie, *time,
              entries.marking ? std::optional<Marker>(MarkerOf(standing))
                              : std::nullopt);
  }

  if (!ie.IsDirectory()) {
    entries.total.files++;
    return;
  }
  entries.total.dirs++;
  if (entries.recursive && standing != Io::RecordStanding::Reused) {
    entries.children.push_back(
        {ie.GetFileReference(), std::wstring(ie.GetFilename())});
  }
}

// Lists the directory fr holds and every directory below it, depth first.
// root names that directory in the header of its block. A directory that
// cannot be read is reported and skipped. So is one already listed: NTFS
// allows no hard link to a directory, so an entry that leads back to one is a
// loop or a corruption. marking says whether the listing has a marker column.
// Returns the totals of the tree. A directory waits as its name and its depth,
// not as its whole path: the paths of a tree that runs deep grow with its
// depth, and one for each directory that waits would cost memory that grows
// with the square of it.
template <Cache::Strategy S>
Total ListTree(NtfsDevice::Output& out, Io::FileRecord<S>& fr, bool marking,
               const std::wstring& root) {
  struct Pending {
    ULONGLONG reference;
    std::wstring name;
    // Levels below the directory the listing starts at.
    size_t depth;
  };

  Total tree;
  std::vector<Pending> pending;
  // The path of the directory being listed. Its first lengths[d] units are the
  // path of its ancestor d levels below the start, so the path of the next
  // directory is its parent's prefix and its own name.
  std::wstring path = root;
  std::vector<size_t> lengths{root.size()};
  size_t depth = 0;
  // Record numbers of the directories listed so far. An index entry gives a
  // number with its sequence apart, so a number alone identifies a directory.
  std::unordered_set<ULONGLONG> listed;
  if (const std::optional<ULONGLONG> start = fr.GetFileReference()) {
    listed.insert(*start);
  }
  while (true) {
    Report(out, L"\nDirectory of " + path);
    DirectoryEntries<S> entries;
    entries.out = &out;
    entries.marking = marking;
    entries.recursive = true;
    fr.TraverseSubEntriesWithStanding(VisitEntry<S>, &entries);
    tree.files += entries.total.files;
    tree.dirs += entries.total.dirs;

    // The stack pops the last pushed first, so push in reverse to list the
    // subdirectories in index order.
    for (auto child = entries.children.rbegin();
         child != entries.children.rend(); ++child) {
      pending.push_back({.reference = child->reference,
                         .name = std::move(child->name),
                         .depth = depth + 1});
    }

    // Take the next directory that is new and parses, and report the ones
    // that are not.
    bool entered = false;
    while (!pending.empty() && !entered) {
      Pending next = std::move(pending.back());
      pending.pop_back();
      // Back to the parent's path: a pending directory is never deeper than
      // one level below the last one listed, and its parent is on the way.
      lengths.resize(next.depth);
      path.resize(lengths.back());
      path += path_separator;
      path += next.name;
      if (listed.contains(next.reference)) {
        Report(out, L"Skipping " + path + L": directory already listed");
        continue;
      }
      entered = fr.ParseFileRecord(next.reference) && fr.ParseAttrs();
      if (entered) {
        listed.insert(next.reference);
        lengths.push_back(path.size());
        depth = next.depth;
      } else {
        Report(out, L"Cannot read directory " + path);
      }
    }
    if (!entered) {
      return tree;
    }
  }
}

// Moves fr into the subdirectory named pathname. Returns false after
// printing why it cannot.
template <Cache::Strategy S>
bool EnterSubdirectory(Io::FileRecord<S>& fr, std::wstring_view pathname) {
  std::optional<IndexEntry> ie = fr.FindSubEntry(pathname);
  if (!ie) {
    Say(L"Cannot find directory " + std::wstring(pathname));
    return false;
  }

  if (!ie->IsDirectory()) {
    Say(std::wstring(pathname) + L" is not a directory");
    return false;
  }

  if (!fr.ParseFileRecord(ie->GetFileReference())) {
    Say(L"Cannot read directory " + std::wstring(pathname));
    return false;
  }
  if (!fr.ParseAttrs()) {
    printf("Cannot parse attributes\n");
    return false;
  }
  return true;
}

// Moves fr to the root directory of the volume called label, then to each
// directory path names in turn. Returns false after printing why it cannot.
template <Cache::Strategy S>
bool EnterDirectories(Io::FileRecord<S>& fr, const std::wstring& label,
                      std::wstring_view path) {
  if (!fr.ParseFileRecord(static_cast<ULONGLONG>(Mft::Idx::Root))) {
    Say(L"Cannot read root directory of volume " + label);
    return false;
  }
  if (!fr.ParseAttrs()) {
    printf("Cannot parse attributes\n");
    return false;
  }

  while (true) {
    const std::wstring_view pathname = Getpathname(path);
    // An empty segment means the path is used up.
    if (pathname.empty()) {
      return true;
    }
    if (!EnterSubdirectory(fr, pathname)) {
      return false;
    }
  }
}

// Lists the directory path names on volume, which label names in messages
// and root in the headers of a recursive listing. Returns the exit code.
template <Cache::Strategy S>
int ListFrom(NtfsVolume<S>& volume, const std::wstring& label,
             const std::wstring& path, const std::wstring& root,
             bool recursive) {
  Io::FileRecord<S> fr(volume);
  // we only need INDEX_ROOT and INDEX_ALLOCATION
  // don't waste time and ram to parse unwanted attributes
  fr.SetAttrMask(Attr::Mask::IndexRoot | Attr::Mask::IndexAllocation);
  if (!EnterDirectories(fr, label, path)) {
    return -1;
  }

  // Without --deleted every entry names a file that exists: no marker column.
  const bool marking = volume.GetOptions().include_deleted;

  NtfsDevice::Output out;
  Total total;
  if (recursive) {
    total = ListTree(out, fr, marking, root);
  } else {
    DirectoryEntries<S> entries;
    entries.out = &out;
    entries.marking = marking;
    fr.TraverseSubEntriesWithStanding(VisitEntry<S>, &entries);
    total = entries.total;
  }
  out.PrintLine("Files: {}, Directories: {}", total.files, total.dirs);
  return 0;
}

// Opens the device or image file at device, which label names in messages.
// Returns null after printing why it cannot.
template <Cache::Strategy S>
std::unique_ptr<NtfsVolume<S>>
    OpenDeviceVolume(const std::filesystem::path& device,
                     const std::wstring& label, const VolumeOptions& options) {
#ifdef _WIN32
  // Win32DiskReader opens a plain file the same way as a device.
  return std::make_unique<NtfsVolume<S>>(std::wstring_view(label), options);
#else
  auto reader = std::make_unique<NtfsDevice::PosixDeviceDiskReader>();
  if (!reader->Open(NtfsDevice::WideFromPath(device))) {
    const int error = reader->LastError();
    if (error == EACCES || error == EPERM) {
      Say(L"Cannot read device " + label + L": read access is needed");
    } else {
      Say(L"Cannot open " + label);
    }
    return nullptr;
  }
  return std::make_unique<NtfsVolume<S>>(std::move(reader), options);
#endif
}

// Lists the directory of an image file or device. Returns the exit code.
template <Cache::Strategy S>
int ListDevice(const NtfsDevice::DeviceTarget& target, bool recursive,
               const VolumeOptions& options) {
  const std::wstring label = NtfsDevice::WideFromPath(target.device);
  const std::unique_ptr<NtfsVolume<S>> volume =
      OpenDeviceVolume<S>(target.device, label, options);
  if (!volume) {
    return -1;
  }
  if (!volume->IsVolumeOK()) {
    Say(L"Cannot get NTFS BPB from boot sector of " + label);
    return -1;
  }
  const std::wstring root = target.directory.empty()
                                ? label
                                : label + path_separator + target.directory;
  return ListFrom(*volume, label, target.directory, root, recursive);
}

#ifdef _WIN32

// Lists the directory of a drive, from a path such as "c:\windows". Returns
// the exit code.
template <Cache::Strategy S>
int ListDrive(wchar_t* path, bool recursive, const VolumeOptions& options) {
  const wchar_t volname = Getvolume(&path);
  if (volname == L'\0') {
    Usage();
    return -1;
  }

  NtfsVolume<S> volume(volname, options);
  const std::wstring label(1, volname);
  if (!volume.IsVolumeOK()) {
    Say(L"Cannot get NTFS BPB from boot sector of volume " + label);
    return -1;
  }
  const std::wstring root = label + L':' + path_separator + path;
  return ListFrom(volume, label, path, root, recursive);
}

#endif  // _WIN32

// Lists the directory that path names. Returns the exit code.
template <Cache::Strategy S>
int ListDirectory(wchar_t* path, bool recursive, const VolumeOptions& options) {
  if (const std::optional<NtfsDevice::DeviceTarget> target =
          NtfsDevice::SplitDeviceTarget(path)) {
    return ListDevice<S>(*target, recursive, options);
  }

#ifdef _WIN32
  return ListDrive<S>(path, recursive, options);
#else
  Say(L"Cannot find a device or image file in " + std::wstring(path));
  return -1;
#endif
}

}  // namespace

namespace {

// Reports whether arg asks for a recursive listing.
bool IsRecursiveOption(std::wstring_view arg) {
  return arg == L"--recursive" || arg == L"-r";
}

// Sets the volume option arg names, and reports whether it names one.
bool ApplyVolumeOption(std::wstring_view arg, VolumeOptions& options) {
  if (arg == L"--deleted") {
    options.include_deleted = true;
    return true;
  }
  if (arg == L"--recover") {
    options.recover_errors = true;
    return true;
  }
  return false;
}

}  // namespace

std::optional<Arguments> ParseArguments(int argc, wchar_t* argv[]) {
  Arguments args;
  for (int i = 1; i < argc; i++) {
    if (NtfsDevice::IsLogOption(argv[i])) {
      if (!NtfsDevice::ParseLogOption(argv[i], args.log_config)) {
        return std::nullopt;
      }
    } else if (IsRecursiveOption(argv[i])) {
      args.recursive = true;
    } else if (ApplyVolumeOption(argv[i], args.volume_options)) {
      continue;
    } else if (args.path == nullptr) {
      args.path = argv[i];
    } else {
      return std::nullopt;
    }
  }

  if (args.path == nullptr) {
    return std::nullopt;
  }
  return args;
}

template <Cache::Strategy S>
int RunNtfsDir(int argc, wchar_t* argv[]) {
  const std::optional<Arguments> args = ParseArguments(argc, argv);
  if (!args) {
    Usage();
    return -1;
  }

  // Names print as UTF-8: a console shows them right with its code page set.
  const NtfsDevice::Utf8ConsoleOutput console;
  if (!Log::Configure(args->log_config)) {
    NtfsDevice::ReportLogFileError(args->log_config);
  }

  return ListDirectory<S>(args->path, args->recursive, args->volume_options);
}

template int RunNtfsDir<Cache::Strategy::FullCache>(int argc, wchar_t* argv[]);
template int RunNtfsDir<Cache::Strategy::NoCache>(int argc, wchar_t* argv[]);
