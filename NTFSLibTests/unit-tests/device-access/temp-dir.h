#pragma once

#include <filesystem>
#include <fstream>
#include <random>
#include <string>
#include <string_view>
#include <system_error>

#include <ntfs-browser/namespace.h>

namespace NTFS_BROWSER_NAMESPACE::Tests {

// A fresh empty directory under the system temp directory, removed with
// everything in it when the object goes out of scope.
class TempDir final {
 public:
  TempDir() {
    std::random_device random_device;
    path_ = std::filesystem::temp_directory_path() /
            ("ntfs-device-access-" + std::to_string(random_device()));
    std::filesystem::create_directories(path_);
  }

  TempDir(const TempDir&) = delete;
  TempDir& operator=(const TempDir&) = delete;
  TempDir(TempDir&&) = delete;
  TempDir& operator=(TempDir&&) = delete;

  ~TempDir() {
    std::error_code ignored;
    // A test may have made a file unreadable.
    for (const auto& entry :
         std::filesystem::recursive_directory_iterator(path_, ignored)) {
      std::filesystem::permissions(entry.path(),
                                   std::filesystem::perms::owner_all,
                                   std::filesystem::perm_options::add, ignored);
    }
    std::filesystem::remove_all(path_, ignored);
  }

  [[nodiscard]] const std::filesystem::path& Path() const { return path_; }

  // Writes content to the file name in this directory, and returns its path.
  [[nodiscard]] std::filesystem::path Write(std::string_view name,
                                            std::string_view content) const {
    const std::filesystem::path file = path_ / name;
    std::ofstream output(file, std::ios::binary);
    output.write(content.data(), static_cast<std::streamsize>(content.size()));
    return file;
  }

 private:
  std::filesystem::path path_;
};

}  // namespace NTFS_BROWSER_NAMESPACE::Tests
