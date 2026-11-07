#include <ntfs-browser/win-types.h>

#include <filesystem>
#include <fstream>
#include <ios>
#include <span>
#include <stdexcept>
#include <string_view>
#include <utility>

#include <gsl/narrow>

#include <ntfs-browser/io/extended-path.h>
#include <ntfs-browser/namespace.h>
#include <ntfs-browser/tool/fuzzgen/byte-writer.h>

namespace NTFS_BROWSER_NAMESPACE::Tool::FuzzGen {

ByteWriter::ByteWriter(std::filesystem::path folder)
    : folder_(std::move(folder)) {}

void ByteWriter::Write(std::string_view name,
                       std::span<const BYTE> bytes) const {
  const std::filesystem::path path =
      Io::ToExtendedLengthFsPath(folder_ / std::filesystem::path(name));
  std::filesystem::create_directories(path.parent_path());
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  out.write(reinterpret_cast<const char*>(bytes.data()),
            gsl::narrow<std::streamsize>(bytes.size()));
  out.close();
  if (!out) {
    throw std::runtime_error("cannot write " + path.string());
  }
}

}  // namespace NTFS_BROWSER_NAMESPACE::Tool::FuzzGen
