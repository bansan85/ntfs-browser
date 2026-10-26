#pragma once

#include <ntfs-browser/win-types.h>

#include <cstddef>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include <ntfs-browser/io/extended-path.h>
#include <ntfs-browser/namespace.h>

namespace NTFS_BROWSER_NAMESPACE::Tests {

// Code units of one level of a LongTempDir: the name and a separator. Under
// every limit of a component, 255 on NTFS.
inline constexpr std::size_t long_level_units = 100;

// A chain of directories under the system temp directory, deep enough that a
// file in it has a path of at least minimum_units units, removed with all it
// holds. It is made and removed through extended-length paths: the standard
// library hands a path to Win32 as it is, and one of more than 260 units would
// fail there.
class LongTempDir final {
 public:
  explicit LongTempDir(std::size_t minimum_units) {
    std::random_device random_device;
    root_ = std::filesystem::temp_directory_path() /
            ("ntfs-long-" + std::to_string(random_device()));
    deepest_ = root_;
    for (std::size_t level = 0; deepest_.native().size() < minimum_units;
         level++) {
      deepest_ /= std::string(long_level_units - 1,
                              static_cast<char>('a' + (level % 26)));
    }
    std::error_code error;
    std::filesystem::create_directories(Io::ToExtendedLengthFsPath(deepest_),
                                        error);
    if (error) {
      throw std::system_error(error);
    }
  }

  LongTempDir(const LongTempDir&) = delete;
  LongTempDir& operator=(const LongTempDir&) = delete;
  LongTempDir(LongTempDir&&) = delete;
  LongTempDir& operator=(LongTempDir&&) = delete;

  ~LongTempDir() {
    std::error_code ignored;
    std::filesystem::remove_all(Io::ToExtendedLengthFsPath(root_), ignored);
  }

  // The deepest directory, as a user would type it: no extended-length prefix.
  [[nodiscard]] const std::filesystem::path& Directory() const {
    return deepest_;
  }

  // The path of the file called name in the deepest directory, as a user would
  // type it.
  [[nodiscard]] std::filesystem::path FileIn(std::string_view name) const {
    return deepest_ / name;
  }

  // Writes bytes to the file called name in the deepest directory, and returns
  // its path.
  std::filesystem::path Write(std::string_view name,
                              const std::vector<BYTE>& bytes) const {
    const std::filesystem::path file = FileIn(name);
    std::ofstream output(Io::ToExtendedLengthFsPath(file),
                         std::ios::binary | std::ios::trunc);
    output.write(reinterpret_cast<const char*>(bytes.data()),
                 static_cast<std::streamsize>(bytes.size()));
    return file;
  }

 private:
  std::filesystem::path root_;
  std::filesystem::path deepest_;
};

}  // namespace NTFS_BROWSER_NAMESPACE::Tests
