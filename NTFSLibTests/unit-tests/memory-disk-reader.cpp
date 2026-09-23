#include "memory-disk-reader.h"

#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>

namespace NtfsBrowserTests
{

MemoryDiskReader::MemoryDiskReader(std::vector<BYTE> data)
    : data_(std::move(data))
{
}

bool MemoryDiskReader::Open(std::wstring_view path)
{
  std::ifstream in(std::filesystem::path(path), std::ios::binary);
  if (!in)
  {
    return false;
  }

  data_.assign(std::istreambuf_iterator<char>(in),
               std::istreambuf_iterator<char>());
  return true;
}

bool MemoryDiskReader::ReadInto(LARGE_INTEGER& addr, std::span<BYTE> dest) const
{
  if (addr.QuadPart < 0 ||
      static_cast<ULONGLONG>(addr.QuadPart) + dest.size() > data_.size())
  {
    return false;
  }

  std::memcpy(dest.data(), data_.data() + addr.QuadPart, dest.size());
  return true;
}

}  // namespace NtfsBrowserTests
