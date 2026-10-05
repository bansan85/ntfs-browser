#include "partition-disk-reader.h"

#include <ntfs-browser/win-types.h>

#include <cstddef>
#include <filesystem>
#include <span>

#include <gsl/narrow>

namespace NtfsBrowserTests
{

PartitionDiskReader::PartitionDiskReader(ULONGLONG partition_offset)
    : partition_offset_(partition_offset)
{
}

bool PartitionDiskReader::Open(std::wstring_view path)
{
  file_.open(std::filesystem::path(path), std::ios::binary);
  return file_.is_open();
}

bool PartitionDiskReader::ReadInto(LARGE_INTEGER& addr,
                                   std::span<BYTE> dest) const
{
  if (addr.QuadPart < 0)
  {
    return false;
  }

  file_.clear();
  file_.seekg(gsl::narrow<std::streamoff>(
      partition_offset_ + gsl::narrow<ULONGLONG>(addr.QuadPart)));
  file_.read(reinterpret_cast<char*>(dest.data()),
             gsl::narrow<std::streamsize>(dest.size()));
  return gsl::narrow<size_t>(file_.gcount()) == dest.size();
}

}  // namespace NtfsBrowserTests
