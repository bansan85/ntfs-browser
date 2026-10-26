#include "os-api-walk.h"

#include <ntfs-browser/win-types.h>

#include <array>
#include <ctime>
#include <filesystem>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <gsl/narrow>

#ifdef _WIN32
  #include <windows.h>

  #include <ntfs-browser/io/extended-path.h>
#else
  #include <dirent.h>
  #include <fcntl.h>
  #include <linux/stat.h>
  #include <sys/stat.h>
  #include <sys/types.h>
  #ifdef __linux__
    #include <sys/xattr.h>
  #endif

  #include "text-convert.h"
#endif

#include "time-convert.h"

namespace NtfsCompare {

namespace {

// A directory still to list: its path on disk, and the key its entries are
// listed under.
struct Frame {
  std::filesystem::path dir;
  std::wstring prefix;
};

// Lists the directories on the stack one at a time, until none is left.
// list_directory adds each subdirectory it finds to the stack.
template <typename ListOne>
Listing WalkDirectories(const std::filesystem::path& root,
                        const ListOne& list_directory) {
  Listing result;

  std::vector<Frame> stack;
  stack.push_back({.dir = root, .prefix = L""});

  while (!stack.empty()) {
    const Frame frame = std::move(stack.back());
    stack.pop_back();
    list_directory(frame, result, stack);
  }

  return result;
}

// The key of the entry called name in frame's directory.
std::wstring ChildPath(const Frame& frame, std::wstring_view name) {
  return frame.prefix.empty() ? std::wstring(name)
                              : frame.prefix + L"/" + std::wstring(name);
}

}  // namespace

#ifdef _WIN32

namespace {

// The entry of one FindFirstFileW result, without its allocated size.
Entry EntryFromFindData(const WIN32_FIND_DATAW& fd) {
  const bool is_directory =
      (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;

  Entry entry;
  entry.is_directory = is_directory;
  entry.read_only = (fd.dwFileAttributes & FILE_ATTRIBUTE_READONLY) != 0;
  entry.hidden = (fd.dwFileAttributes & FILE_ATTRIBUTE_HIDDEN) != 0;
  entry.system = (fd.dwFileAttributes & FILE_ATTRIBUTE_SYSTEM) != 0;
  entry.archive = (fd.dwFileAttributes & FILE_ATTRIBUTE_ARCHIVE) != 0;
  entry.compressed = (fd.dwFileAttributes & FILE_ATTRIBUTE_COMPRESSED) != 0;
  entry.encrypted = (fd.dwFileAttributes & FILE_ATTRIBUTE_ENCRYPTED) != 0;
  entry.sparse = (fd.dwFileAttributes & FILE_ATTRIBUTE_SPARSE_FILE) != 0;

  entry.creation_time_utc = FiletimeToUtcTicks(fd.ftCreationTime);
  entry.modification_time_utc = FiletimeToUtcTicks(fd.ftLastWriteTime);
  entry.access_time_utc = FiletimeToUtcTicks(fd.ftLastAccessTime);
  // ChangeTimeUtc stays unset: no documented Win32 API exposes NTFS'
  // own MFT change time.

  if (!is_directory) {
    entry.logical_size =
        (static_cast<ULONGLONG>(fd.nFileSizeHigh) << 32) | fd.nFileSizeLow;
  }
  return entry;
}

// The bytes NTFS has allocated for a file, or nullopt when it cannot be
// opened.
std::optional<ULONGLONG> AllocatedSize(const std::filesystem::path& file) {
  // GetCompressedFileSizeW() is compression-specific: on an
  // uncompressed file it just returns the logical size again, not the
  // allocation. FileStandardInfo::AllocationSize is the one that always
  // reflects clusters actually allocated, compressed or not.
  // The path is extended (see WalkOsApi()), so the name is opened as spelled.
  const HANDLE file_handle =
      CreateFileW(file.c_str(), FILE_READ_ATTRIBUTES,
                  FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                  nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
  if (file_handle == INVALID_HANDLE_VALUE) {
    return std::nullopt;
  }

  std::optional<ULONGLONG> size;
  FILE_STANDARD_INFO info{};
  if (GetFileInformationByHandleEx(file_handle, FileStandardInfo, &info,
                                   sizeof(info)) != 0) {
    size = gsl::narrow<ULONGLONG>(info.AllocationSize.QuadPart);
  }
  CloseHandle(file_handle);
  return size;
}

// Adds the entry of one FindFirstFileW result found in frame. A subdirectory
// is queued on the stack, unless it is a reparse point.
void AddFindResult(const Frame& frame, const WIN32_FIND_DATAW& fd,
                   Listing& result, std::vector<Frame>& stack) {
  const std::filesystem::path full = frame.dir / fd.cFileName;
  const std::wstring path = ChildPath(frame, fd.cFileName);

  Entry entry = EntryFromFindData(fd);
  if (!entry.is_directory) {
    entry.physical_size = AllocatedSize(full);
  }
  result.emplace(path, entry);

  // Never recurse into a reparse point (symlink/junction): every method
  // in this tool follows the same policy.
  const bool is_reparse =
      (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
  if (entry.is_directory && !is_reparse) {
    stack.push_back({.dir = full, .prefix = path});
  }
}

// Lists the entries of one directory with FindFirstFileW.
void ListDirectory(const Frame& frame, Listing& result,
                   std::vector<Frame>& stack) {
  const std::wstring pattern = (frame.dir / L"*").wstring();
  WIN32_FIND_DATAW fd{};
  const HANDLE handle = FindFirstFileW(pattern.c_str(), &fd);
  if (handle == INVALID_HANDLE_VALUE) {
    return;
  }

  do {
    const std::wstring_view name = fd.cFileName;
    if (name == L"." || name == L"..") {
      continue;
    }
    AddFindResult(frame, fd, result, stack);
  } while (FindNextFileW(handle, &fd) != 0);

  FindClose(handle);
}

}  // namespace

const char* OsApiMethodName() noexcept { return "Windows API"; }

Listing WalkOsApi(const std::filesystem::path& root) {
  // Extended once, here: every path below it inherits the prefix, so a folder
  // of any depth lists, and a name Win32 cannot open as typed (a trailing dot,
  // CON, NUL) opens as it is spelled.
  return WalkDirectories(
      NTFS_BROWSER_NAMESPACE::Io::ToExtendedLengthFsPath(root), ListDirectory);
}

#else  // POSIX

namespace {

// Bits in a byte: Windows-style attribute DWORDs are assembled byte by byte.
constexpr unsigned xattr_bits_per_byte = 8;

// Size in bytes of one POSIX st_blocks unit.
constexpr ULONGLONG stat_block_size = 512ULL;

// FILE_ATTRIBUTE_* bits ntfs-3g/ntfs3 report through the xattr.
constexpr DWORD attr_archive = 0x20U;
constexpr DWORD attr_sparse = 0x200U;
constexpr DWORD attr_compressed = 0x800U;
constexpr DWORD attr_encrypted = 0x4000U;

  // The DWORD FILE_ATTRIBUTE_* bits ntfs-3g/ntfs3 expose verbatim through this
  // xattr, little-endian. No generic POSIX call carries them, so this is
  // best-effort: absent on a non-NTFS mount, or a driver too old to set it.
  #ifdef __linux__
bool ReadNtfsAttribXattr(const std::filesystem::path& path, DWORD& value) {
  std::array<unsigned char, 4> buf{};
  const ssize_t xattr_size =
      getxattr(path.c_str(), "system.ntfs_attrib", buf.data(), buf.size());
  if (std::cmp_not_equal(xattr_size, buf.size())) {
    return false;
  }
  value = 0;
  for (size_t i = 0; i < buf.size(); i++) {
    value |= static_cast<DWORD>(buf.at(i)) << (i * xattr_bits_per_byte);
  }
  return true;
}
  #else
bool ReadNtfsAttribXattr(const std::filesystem::path&, DWORD&) { return false; }
  #endif

// Sets the NTFS attribute flags of entry from the xattr of path, when the
// mount exposes it.
void SetNtfsAttributes(Entry& entry, const std::filesystem::path& full) {
  DWORD ntfs_attrib = 0;
  if (!ReadNtfsAttribXattr(full, ntfs_attrib)) {
    return;
  }
  entry.read_only = (ntfs_attrib & 0x1U) != 0;
  entry.hidden = (ntfs_attrib & 0x2U) != 0;
  entry.system = (ntfs_attrib & 0x4U) != 0;
  entry.archive = (ntfs_attrib & attr_archive) != 0;
  entry.sparse = (ntfs_attrib & attr_sparse) != 0;
  entry.compressed = (ntfs_attrib & attr_compressed) != 0;
  entry.encrypted = (ntfs_attrib & attr_encrypted) != 0;
}

// Sets the birth time of entry, where the platform reports one.
void SetCreationTime([[maybe_unused]] Entry& entry,
                     [[maybe_unused]] const std::filesystem::path& full) {
  #ifdef STATX_BTIME
  struct statx stx = {};
  if (statx(AT_FDCWD, full.c_str(), AT_SYMLINK_NOFOLLOW, STATX_BTIME, &stx) ==
          0 &&
      (stx.stx_mask & STATX_BTIME) != 0) {
    entry.creation_time_utc =
        SecondsNanosToUtcTicks(stx.stx_btime.tv_sec, stx.stx_btime.tv_nsec);
  }
  #endif
}

// The entry of one directory item, from its lstat() result.
Entry EntryFromStat(const struct stat& file_stat,
                    const std::filesystem::path& full) {
  Entry entry;
  entry.is_directory = S_ISDIR(file_stat.st_mode);
  if (!entry.is_directory) {
    entry.logical_size = gsl::narrow<ULONGLONG>(file_stat.st_size);
  }
  entry.physical_size =
      gsl::narrow<ULONGLONG>(file_stat.st_blocks) * stat_block_size;

  entry.modification_time_utc = SecondsNanosToUtcTicks(
      file_stat.st_mtim.tv_sec, file_stat.st_mtim.tv_nsec);
  entry.access_time_utc = SecondsNanosToUtcTicks(file_stat.st_atim.tv_sec,
                                                 file_stat.st_atim.tv_nsec);
  // ctime is Unix's own "inode change time": the closest analog to
  // NTFS' change/MFT-modification time, though a different OS's concept,
  // not literally the same field - the point of comparing it.
  entry.change_time_utc = SecondsNanosToUtcTicks(file_stat.st_ctim.tv_sec,
                                                 file_stat.st_ctim.tv_nsec);

  SetCreationTime(entry, full);
  SetNtfsAttributes(entry, full);
  return entry;
}

// Adds the entry called name in frame's directory. A subdirectory is queued
// on the stack.
void AddDirent(const Frame& frame, const char* name, Listing& result,
               std::vector<Frame>& stack) {
  const std::filesystem::path full = frame.dir / name;

  struct stat file_stat = {};
  if (lstat(full.c_str(), &file_stat) != 0) {
    return;
  }

  const Entry entry = EntryFromStat(file_stat, full);
  const std::wstring path = ChildPath(frame, NtfsDevice::Utf8ToWide(name));
  result.emplace(path, entry);

  // lstat() reports a symlink as a link, never as a directory, so symlinks
  // are not queued: every method in this tool follows the same policy.
  if (entry.is_directory) {
    stack.push_back({.dir = full, .prefix = path});
  }
}

// Lists the entries of one directory with opendir() and readdir().
void ListDirectory(const Frame& frame, Listing& result,
                   std::vector<Frame>& stack) {
  DIR* handle = opendir(frame.dir.c_str());
  if (handle == nullptr) {
    return;
  }

  for (struct dirent* de = readdir(handle); de != nullptr;
       de = readdir(handle)) {
    const std::string_view name = std::data(de->d_name);
    if (name == "." || name == "..") {
      continue;
    }
    AddDirent(frame, de->d_name, result, stack);
  }

  closedir(handle);
}

}  // namespace

const char* OsApiMethodName() noexcept { return "Linux API"; }

Listing WalkOsApi(const std::filesystem::path& root) {
  return WalkDirectories(root, ListDirectory);
}

#endif  // _WIN32

}  // namespace NtfsCompare
