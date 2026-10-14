#include "ntfsdir-core.h"

#include <cstdio>
#include <optional>
#include <string>
#include <string_view>

#include <gsl/narrow>

#include <ntfs-browser/attr/base.h>
#include <ntfs-browser/index-entry.h>
#include <ntfs-browser/io/file-record.h>
#include <ntfs-browser/log/log.h>
#include <ntfs-browser/mft/idx.h>
#include <ntfs-browser/ntfs-volume.h>

using namespace NtfsBrowser;

void Usage() {
  printf("Invalid parameter\n");
  printf("Usage: ntfsdir [--log=...] \"path\"\n");
  printf("  %s\n", std::string(Log::option_usage).c_str());
  printf("eg. ntfsdir c:\n");
  printf("eg. ntfsdir c:\\windows\n");
  printf("eg. ntfsdir \"c:\\program files\\common files\"\n");
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

// Prints the modification time, the size or <DIR>, the attribute flags and
// the name of one entry. st is the entry time in local time.
void PrintLine(const IndexEntryView& ie, const SYSTEMTIME& st) {
  printf("%u-%02u-%02u  %02u:%02u\t%s    ", st.wYear, st.wMonth, st.wDay,
         st.wHour, st.wMinute, ie.IsDirectory() ? "<DIR>" : "     ");

  if (!ie.IsDirectory()) {
    printf("%I64u\t", ie.GetFileSize());
  } else {
    printf("\t");
  }

  const std::wstring_view fn = ie.GetFilename();
  printf("<%c%c%c>\t%.*ls\n", ie.IsReadOnly() ? 'R' : ' ',
         ie.IsHidden() ? 'H' : ' ', ie.IsSystem() ? 'S' : ' ',
         gsl::narrow<int>(fn.size()), fn.data());
}

// Moves fr into the subdirectory named pathname. Returns false after
// printing why it cannot.
template <Cache::Strategy S>
bool EnterSubdirectory(Io::FileRecord<S>& fr, const std::wstring& pathname) {
  std::optional<IndexEntry> ie = fr.FindSubEntry(pathname);
  if (!ie) {
    printf("Cannot find directory %ls\n", pathname.c_str());
    return false;
  }

  if (!ie->IsDirectory()) {
    printf("%ls is not a directory\n", pathname.c_str());
    return false;
  }

  if (!fr.ParseFileRecord(ie->GetFileReference())) {
    printf("Cannot read directory %ls\n", pathname.c_str());
    return false;
  }
  if (!fr.ParseAttrs()) {
    printf("Cannot parse attributes\n");
    return false;
  }
  return true;
}

// Moves fr to the root directory, then to each directory path names in turn.
// Returns false after printing why it cannot.
template <Cache::Strategy S>
bool EnterDirectories(Io::FileRecord<S>& fr, wchar_t volname,
                      std::wstring path) {
  if (!fr.ParseFileRecord(static_cast<ULONGLONG>(Mft::Idx::Root))) {
    printf("Cannot read root directory of volume %lc\n", volname);
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

// Lists the directory that path names. Returns the exit code.
template <Cache::Strategy S>
int ListDirectory(wchar_t* path) {
  const wchar_t volname = Getvolume(&path);
  if (volname == L'\0') {
    Usage();
    return -1;
  }

  NtfsVolume<S> volume(volname);
  if (!volume.IsVolumeOK()) {
    printf("Cannot get NTFS BPB from boot sector of volume %lc\n", volname);
    return -1;
  }

  Io::FileRecord<S> fr(volume);
  // we only need INDEX_ROOT and INDEX_ALLOCATION
  // don't waste time and ram to parse unwanted attributes
  fr.SetAttrMask(Attr::Mask::IndexRoot | Attr::Mask::IndexAllocation);
  if (!EnterDirectories(fr, volname, path)) {
    return -1;
  }

  Total total;
  fr.TraverseSubEntries(Printfile, &total);
  printf("Files: %d, Directories: %d\n", total.files, total.dirs);
  return 0;
}

}  // namespace

void Printfile(const IndexEntryView& ie, void* context) {
  if (!IsListed(ie)) {
    return;
  }

  FILETIME ft;
  ie.GetFileTime(&ft, nullptr, nullptr);
  SYSTEMTIME st;
  if (FileTimeToSystemTime(&ft, &st) == TRUE) {
    PrintLine(ie, st);
  }

  Total& total = *static_cast<Total*>(context);
  if (ie.IsDirectory()) {
    total.dirs++;
  } else {
    total.files++;
  }
}

std::optional<Arguments> ParseArguments(int argc, wchar_t* argv[]) {
  Arguments args;
  for (int i = 1; i < argc; i++) {
    if (std::wstring_view(argv[i]).starts_with(Log::option_prefix_w)) {
      if (!Log::ParseOption(argv[i], args.log_config)) {
        return std::nullopt;
      }
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
    fprintf(stderr, "Cannot open log file %ls\n",
            args->log_config.file_path.c_str());
  }

  return ListDirectory<S>(args->path);
}

template int RunNtfsDir<Cache::Strategy::FullCache>(int argc, wchar_t* argv[]);
template int RunNtfsDir<Cache::Strategy::NoCache>(int argc, wchar_t* argv[]);
