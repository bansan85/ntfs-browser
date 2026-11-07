#include <filesystem>
#include <string>
#include <string_view>

#include <ntfs-browser/io/extended-path.h>
#include <ntfs-browser/namespace.h>

namespace NTFS_BROWSER_NAMESPACE::Io {

std::wstring ToExtendedLengthPath(std::wstring_view path) {
  return std::wstring(path);
}

std::filesystem::path
    ToExtendedLengthFsPath(const std::filesystem::path& path) {
  return path;
}

}  // namespace NTFS_BROWSER_NAMESPACE::Io
