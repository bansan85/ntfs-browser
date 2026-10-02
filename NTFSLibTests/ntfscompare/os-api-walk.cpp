#include "os-api-walk.h"

#include <ntfs-browser/win-types.h>

#include <ctime>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <gsl/narrow>

#ifdef _WIN32
  #include <windows.h>
#else
  #include <linux/stat.h>
  #include <sys/types.h>
#endif

#include "time-convert.h"

#ifdef _WIN32
namespace NtfsCompare
{

const char* OsApiMethodName() noexcept { return "Windows API"; }

Listing WalkOsApi(const std::filesystem::path& root)
{
  Listing result;

  struct Frame
  {
    std::filesystem::path dir;
    std::wstring prefix;
  };
  std::vector<Frame> stack;
  stack.push_back({.dir = root, .prefix = L""});

  while (!stack.empty())
  {
    const Frame frame = std::move(stack.back());
    stack.pop_back();

    const std::wstring pattern = (frame.dir / L"*").wstring();
    WIN32_FIND_DATAW fd{};
    const HANDLE handle = FindFirstFileW(pattern.c_str(), &fd);
    if (handle == INVALID_HANDLE_VALUE)
    {
      continue;
    }

    do
    {
      const std::wstring_view name = fd.cFileName;
      if (name == L"." || name == L"..")
      {
        continue;
      }

      const std::filesystem::path full = frame.dir / fd.cFileName;
      const std::wstring path = frame.prefix.empty()
                                    ? std::wstring(fd.cFileName)
                                    : frame.prefix + L"/" + fd.cFileName;

      // Never recurse into a reparse point (symlink/junction): every method
      // in this tool follows the same policy.
      const bool isReparse =
          (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
      const bool isDirectory =
          (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;

      Entry entry;
      entry.is_directory = isDirectory;
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

      if (!isDirectory)
      {
        entry.logical_size =
            (static_cast<ULONGLONG>(fd.nFileSizeHigh) << 32) | fd.nFileSizeLow;

        // GetCompressedFileSizeW() is compression-specific: on an
        // uncompressed file it just returns the logical size again, not the
        // allocation. FileStandardInfo::AllocationSize is the one that always
        // reflects clusters actually allocated, compressed or not.
        const HANDLE fileHandle = CreateFileW(
            full.c_str(), FILE_READ_ATTRIBUTES,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
            OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
        if (fileHandle != INVALID_HANDLE_VALUE)
        {
          FILE_STANDARD_INFO info{};
          if (GetFileInformationByHandleEx(fileHandle, FileStandardInfo, &info,
                                           sizeof(info)) != 0)
          {
            entry.physical_size =
                gsl::narrow<ULONGLONG>(info.AllocationSize.QuadPart);
          }
          CloseHandle(fileHandle);
        }
      }

      result.emplace(path, entry);

      if (isDirectory && !isReparse)
      {
        stack.push_back({.dir = full, .prefix = path});
      }
    }
    while (FindNextFileW(handle, &fd) != 0);

    FindClose(handle);
  }

  return result;
}

}  // namespace NtfsCompare

#else  // POSIX

  #include <dirent.h>
  #include <fcntl.h>
  #include <sys/stat.h>
  #ifdef __linux__
    #include <sys/xattr.h>
  #endif

  #include "linux-utf8.h"

namespace NtfsCompare
{

namespace
{

// Bits in a byte: Windows-style attribute DWORDs are assembled byte by byte.
constexpr unsigned kXattrBitsPerByte = 8;

// Size in bytes of one POSIX st_blocks unit.
constexpr ULONGLONG kStatBlockSize = 512ULL;

// FILE_ATTRIBUTE_* bits ntfs-3g/ntfs3 report through the xattr.
constexpr DWORD kAttrArchive = 0x20U;
constexpr DWORD kAttrSparse = 0x200U;
constexpr DWORD kAttrCompressed = 0x800U;
constexpr DWORD kAttrEncrypted = 0x4000U;

  // The DWORD FILE_ATTRIBUTE_* bits ntfs-3g/ntfs3 expose verbatim through this
  // xattr, little-endian. No generic POSIX call carries them, so this is
  // best-effort: absent on a non-NTFS mount, or a driver too old to set it.
  #ifdef __linux__
bool ReadNtfsAttribXattr(const std::filesystem::path& path, DWORD& value)
{
  unsigned char buf[4];
  const ssize_t n =
      getxattr(path.c_str(), "system.ntfs_attrib", buf, sizeof(buf));
  if (n != static_cast<ssize_t>(sizeof(buf)))
  {
    return false;
  }
  value = static_cast<DWORD>(buf[0]) |
          (static_cast<DWORD>(buf[1]) << kXattrBitsPerByte) |
          (static_cast<DWORD>(buf[2]) << (2 * kXattrBitsPerByte)) |
          (static_cast<DWORD>(buf[3]) << (3 * kXattrBitsPerByte));
  return true;
}
  #else
bool ReadNtfsAttribXattr(const std::filesystem::path&, DWORD&) { return false; }
  #endif

}  // namespace

const char* OsApiMethodName() noexcept { return "Linux API"; }

Listing WalkOsApi(const std::filesystem::path& root)
{
  Listing result;

  struct Frame
  {
    std::filesystem::path dir;
    std::wstring prefix;
  };
  std::vector<Frame> stack;
  stack.push_back({.dir = root, .prefix = L""});

  while (!stack.empty())
  {
    const Frame frame = std::move(stack.back());
    stack.pop_back();

    DIR* handle = opendir(frame.dir.c_str());
    if (handle == nullptr)
    {
      continue;
    }

    for (struct dirent* de = readdir(handle); de != nullptr;
         de = readdir(handle))
    {
      const std::string_view name = de->d_name;
      if (name == "." || name == "..")
      {
        continue;
      }

      const std::filesystem::path full = frame.dir / de->d_name;

      struct stat st = {};
      if (lstat(full.c_str(), &st) != 0)
      {
        continue;
      }

      // Never recurse into a symlink: every method in this tool follows the
      // same policy.
      const bool isSymlink = S_ISLNK(st.st_mode);
      const bool isDirectory = S_ISDIR(st.st_mode);

      Entry entry;
      entry.is_directory = isDirectory;
      if (!isDirectory)
      {
        entry.logical_size = gsl::narrow<ULONGLONG>(st.st_size);
      }
      entry.physical_size =
          gsl::narrow<ULONGLONG>(st.st_blocks) * kStatBlockSize;

      entry.modification_time_utc =
          SecondsNanosToUtcTicks(st.st_mtim.tv_sec, st.st_mtim.tv_nsec);
      entry.access_time_utc =
          SecondsNanosToUtcTicks(st.st_atim.tv_sec, st.st_atim.tv_nsec);
      // ctime is Unix's own "inode change time": the closest analog to
      // NTFS' change/MFT-modification time, though a different OS's concept,
      // not literally the same field - the point of comparing it.
      entry.change_time_utc =
          SecondsNanosToUtcTicks(st.st_ctim.tv_sec, st.st_ctim.tv_nsec);

  #ifdef STATX_BTIME
      struct statx stx = {};
      if (statx(AT_FDCWD, full.c_str(), AT_SYMLINK_NOFOLLOW, STATX_BTIME,
                &stx) == 0 &&
          (stx.stx_mask & STATX_BTIME) != 0)
      {
        entry.creation_time_utc =
            SecondsNanosToUtcTicks(stx.stx_btime.tv_sec, stx.stx_btime.tv_nsec);
      }
  #endif

      DWORD ntfsAttrib = 0;
      if (ReadNtfsAttribXattr(full, ntfsAttrib))
      {
        entry.read_only = (ntfsAttrib & 0x1U) != 0;
        entry.hidden = (ntfsAttrib & 0x2U) != 0;
        entry.system = (ntfsAttrib & 0x4U) != 0;
        entry.archive = (ntfsAttrib & kAttrArchive) != 0;
        entry.sparse = (ntfsAttrib & kAttrSparse) != 0;
        entry.compressed = (ntfsAttrib & kAttrCompressed) != 0;
        entry.encrypted = (ntfsAttrib & kAttrEncrypted) != 0;
      }

      const std::wstring wname = Utf8ToWide(de->d_name);
      const std::wstring path =
          frame.prefix.empty() ? wname : frame.prefix + L"/" + wname;
      result.emplace(path, entry);

      if (isDirectory && !isSymlink)
      {
        stack.push_back({.dir = full, .prefix = path});
      }
    }

    closedir(handle);
  }

  return result;
}

}  // namespace NtfsCompare

#endif  // _WIN32
