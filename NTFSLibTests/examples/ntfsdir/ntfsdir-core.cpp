#include "ntfsdir-core.h"

#include <cerrno>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <ntfs-browser/index-entry.h>
#include <ntfs-browser/io/file-record.h>
#include <ntfs-browser/log/log.h>
#include <ntfs-browser/mft/idx.h>
#include <ntfs-browser/namespace.h>
#include <ntfs-browser/ntfs-volume.h>

#include "device-path.h"
#include "local-time.h"
#include "ntfs-browser/attr/mask.h"
#include "ntfs-browser/cache/strategy.h"
#include "ntfs-browser/win-types.h"
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
  printf("Usage: ntfsdir [--recursive] [--log=...] \"path\"\n");
  printf("  --recursive, -r  also list every subdirectory of path\n");
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

std::wstring Getpathname(std::wstring& ppath) {
  std::wstring pathname;
  size_t len = 0;
  const wchar_t* p = ppath.c_str();

  // copy until '\' or " or string ends or buffer full
  while (*p && len < ppath.length()) {
    pathname.append(1, *p);
    len++;
    p++;

    if (*p == '\\' || *p == '\"') {
      break;
    }
  }

  // forward to not '\' and not ", or string end
  while (*p) {
    if (*p == '\\' || *p == '\"') {
      p++;
    } else {
      break;
    }
  }

  ppath = ppath.substr(p - ppath.c_str());
  return pathname;
}

namespace {

#ifndef _WIN32
// Bits in a DWORD: where a FILETIME's high half starts.
constexpr unsigned dword_bits = 32;
#endif

// Prints message and a newline to stdout.
void Say(const std::wstring& message) {
  NtfsDevice::WriteText(stdout, message);
  printf("\n");
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

// Prints the modification time, the size or <DIR>, the attribute flags and
// the name of one entry. time is the entry time in local time.
void PrintLine(const IndexEntryView& ie, const NtfsDevice::LocalTime& time) {
  printf("%u-%02u-%02u  %02u:%02u\t%s    ", time.year, time.month, time.day,
         time.hour, time.minute, ie.IsDirectory() ? "<DIR>" : "     ");

  if (!ie.IsDirectory()) {
    printf("%llu\t", static_cast<unsigned long long>(ie.GetFileSize()));
  } else {
    printf("\t");
  }

  printf("<%c%c%c>\t", ie.IsReadOnly() ? 'R' : ' ', ie.IsHidden() ? 'H' : ' ',
         ie.IsSystem() ? 'S' : ' ');
  NtfsDevice::WriteText(stdout, ie.GetFilename());
  printf("\n");
}

// Separates the directory names of a path in the headers of a recursive
// listing.
#ifdef _WIN32
constexpr wchar_t path_separator = L'\\';
#else
constexpr wchar_t path_separator = L'/';
#endif

// What one directory of a recursive listing holds: the tally of its
// entries, and the subdirectories to enter next.
struct DirectoryEntries {
  struct Child {
    ULONGLONG reference;
    std::wstring name;
  };

  Total total;
  std::vector<Child> children;
};

// TraverseSubEntries callback of a recursive listing: prints ie like
// Printfile, and remembers it when it is a listed directory.
void PrintAndCollect(const IndexEntryView& ie, void* context) {
  auto& entries = *static_cast<DirectoryEntries*>(context);
  Printfile(ie, &entries.total);
  if (IsListed(ie) && ie.IsDirectory()) {
    entries.children.push_back(
        {ie.GetFileReference(), std::wstring(ie.GetFilename())});
  }
}

// Lists the directory fr holds and every directory below it, depth first.
// root names that directory in the header of its block. A directory that
// cannot be read is reported and skipped. Returns the totals of the tree.
template <Cache::Strategy S>
Total ListTree(Io::FileRecord<S>& fr, const std::wstring& root) {
  struct Pending {
    ULONGLONG reference;
    std::wstring path;
  };

  Total tree;
  std::vector<Pending> pending;
  std::wstring path = root;
  while (true) {
    Say(L"\nDirectory of " + path);
    DirectoryEntries entries;
    fr.TraverseSubEntries(PrintAndCollect, &entries);
    tree.files += entries.total.files;
    tree.dirs += entries.total.dirs;

    // The stack pops the last pushed first, so push in reverse to list the
    // subdirectories in index order.
    for (auto child = entries.children.rbegin();
         child != entries.children.rend(); ++child) {
      pending.push_back(
          {child->reference, path + path_separator + child->name});
    }

    // Take the next directory that parses, and report the ones that do not.
    bool entered = false;
    while (!pending.empty() && !entered) {
      Pending next = std::move(pending.back());
      pending.pop_back();
      entered = fr.ParseFileRecord(next.reference) && fr.ParseAttrs();
      if (entered) {
        path = std::move(next.path);
      } else {
        Say(L"Cannot read directory " + next.path);
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
bool EnterSubdirectory(Io::FileRecord<S>& fr, const std::wstring& pathname) {
  std::optional<IndexEntry> ie = fr.FindSubEntry(pathname);
  if (!ie) {
    Say(L"Cannot find directory " + pathname);
    return false;
  }

  if (!ie->IsDirectory()) {
    Say(pathname + L" is not a directory");
    return false;
  }

  if (!fr.ParseFileRecord(ie->GetFileReference())) {
    Say(L"Cannot read directory " + pathname);
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
                      std::wstring path) {
  if (!fr.ParseFileRecord(static_cast<ULONGLONG>(Mft::Idx::Root))) {
    Say(L"Cannot read root directory of volume " + label);
    return false;
  }
  if (!fr.ParseAttrs()) {
    printf("Cannot parse attributes\n");
    return false;
  }

  while (true) {
    const std::wstring pathname = Getpathname(path);
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

  Total total;
  if (recursive) {
    total = ListTree(fr, root);
  } else {
    fr.TraverseSubEntries(Printfile, &total);
  }
  printf("Files: %d, Directories: %d\n", total.files, total.dirs);
  return 0;
}

// Opens the device or image file at device, which label names in messages.
// Returns null after printing why it cannot.
template <Cache::Strategy S>
std::unique_ptr<NtfsVolume<S>>
    OpenDeviceVolume(const std::filesystem::path& device,
                     const std::wstring& label) {
#ifdef _WIN32
  // Win32DiskReader opens a plain file the same way as a device.
  return std::make_unique<NtfsVolume<S>>(std::wstring_view(label));
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
  return std::make_unique<NtfsVolume<S>>(std::move(reader));
#endif
}

// Lists the directory of an image file or device. Returns the exit code.
template <Cache::Strategy S>
int ListDevice(const NtfsDevice::DeviceTarget& target, bool recursive) {
  const std::wstring label = NtfsDevice::WideFromPath(target.device);
  const std::unique_ptr<NtfsVolume<S>> volume =
      OpenDeviceVolume<S>(target.device, label);
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
int ListDrive(wchar_t* path, bool recursive) {
  const wchar_t volname = Getvolume(&path);
  if (volname == L'\0') {
    Usage();
    return -1;
  }

  NtfsVolume<S> volume(volname);
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
int ListDirectory(wchar_t* path, bool recursive) {
  if (const std::optional<NtfsDevice::DeviceTarget> target =
          NtfsDevice::SplitDeviceTarget(path)) {
    return ListDevice<S>(*target, recursive);
  }

#ifdef _WIN32
  return ListDrive<S>(path, recursive);
#else
  Say(L"Cannot find a device or image file in " + std::wstring(path));
  return -1;
#endif
}

}  // namespace

void Printfile(const IndexEntryView& ie, void* context) {
  if (!IsListed(ie)) {
    return;
  }

  if (const std::optional<NtfsDevice::LocalTime> time = LocalWriteTime(ie)) {
    PrintLine(ie, *time);
  }

  Total& total = *static_cast<Total*>(context);
  if (ie.IsDirectory()) {
    total.dirs++;
  } else {
    total.files++;
  }
}

namespace {

// Reports whether arg asks for a recursive listing.
bool IsRecursiveOption(std::wstring_view arg) {
  return arg == L"--recursive" || arg == L"-r";
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

  if (!Log::Configure(args->log_config)) {
    NtfsDevice::ReportLogFileError(args->log_config);
  }

  return ListDirectory<S>(args->path, args->recursive);
}

template int RunNtfsDir<Cache::Strategy::FullCache>(int argc, wchar_t* argv[]);
template int RunNtfsDir<Cache::Strategy::NoCache>(int argc, wchar_t* argv[]);
