#include <ntfs-browser/win-types.h>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <ntfs-browser/io/extended-path.h>
#include <ntfs-browser/namespace.h>
#include <ntfs-browser/tool/fuzzgen/resource-locator.h>

namespace NTFS_BROWSER_NAMESPACE::Tool::FuzzGen {

ResourceLocator::ResourceLocator(std::filesystem::path root)
    : root_(std::move(root)) {}

const std::filesystem::path& ResourceLocator::Root() const { return root_; }

std::filesystem::path ResourceLocator::Path(std::string_view name) const {
  return Io::ToExtendedLengthFsPath(root_ / std::filesystem::path(name));
}

bool ResourceLocator::Has(std::string_view name) const {
  std::error_code error;
  return std::filesystem::is_regular_file(Path(name), error);
}

std::vector<BYTE> ResourceLocator::Read(std::string_view name) const {
  const std::filesystem::path path = Path(name);
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    throw std::runtime_error("cannot read the resource " + path.string() +
                             ": did the build extract the archives?");
  }
  const std::vector<char> bytes((std::istreambuf_iterator<char>(in)),
                                std::istreambuf_iterator<char>());
  return {bytes.begin(), bytes.end()};
}

}  // namespace NTFS_BROWSER_NAMESPACE::Tool::FuzzGen
