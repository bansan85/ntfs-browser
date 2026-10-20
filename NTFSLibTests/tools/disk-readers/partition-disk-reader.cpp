#include "partition-disk-reader.h"

#include <ntfs-browser/win-types.h>

#include <cstddef>
#include <filesystem>
#include <optional>
#include <span>

#include <gsl/narrow>

#include <ntfs-browser/namespace.h>

namespace NTFS_BROWSER_NAMESPACE::Tests {

PartitionDiskReader::PartitionDiskReader(
    ULONGLONG partition_offset, std::optional<ULONGLONG> partition_length)
    : partition_offset_(partition_offset),
      partition_length_(partition_length) {}

bool PartitionDiskReader::Open(std::wstring_view path) {
  file_.open(std::filesystem::path(path), std::ios::binary);
  return file_.is_open();
}

bool PartitionDiskReader::ReadInto(LARGE_INTEGER& addr,
                                   std::span<BYTE> dest) const {
  if (addr.QuadPart < 0) {
    return false;
  }

  file_.clear();
  file_.seekg(gsl::narrow<std::streamoff>(
      partition_offset_ + gsl::narrow<ULONGLONG>(addr.QuadPart)));
  file_.read(reinterpret_cast<char*>(dest.data()),
             gsl::narrow<std::streamsize>(dest.size()));
  return gsl::narrow<size_t>(file_.gcount()) == dest.size();
}

std::optional<ULONGLONG> PartitionDiskReader::Size() const {
  return partition_length_;
}

}  // namespace NTFS_BROWSER_NAMESPACE::Tests
