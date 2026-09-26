#include "partition-disk-reader.h"

#include <filesystem>

namespace NtfsBrowserTests
{

PartitionDiskReader::PartitionDiskReader(ULONGLONG partitionOffset)
    : partition_offset_(partitionOffset)
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
  file_.seekg(static_cast<std::streamoff>(
      partition_offset_ + static_cast<ULONGLONG>(addr.QuadPart)));
  file_.read(reinterpret_cast<char*>(dest.data()),
             static_cast<std::streamsize>(dest.size()));
  return static_cast<size_t>(file_.gcount()) == dest.size();
}

}  // namespace NtfsBrowserTests
