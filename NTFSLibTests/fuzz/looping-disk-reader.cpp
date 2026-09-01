#include "looping-disk-reader.h"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <iterator>

namespace NtfsFuzz
{

std::optional<std::vector<BYTE>>
    LoopingDiskReader::LoadFile(const std::filesystem::path& path)
{
  std::ifstream in(path, std::ios::binary);
  if (!in)
  {
    return {};
  }

  std::vector<BYTE> data(std::istreambuf_iterator<char>(in), {});
  if (data.empty())
  {
    return {};
  }

  return data;
}

LoopingDiskReader::LoopingDiskReader(std::vector<BYTE> data)
    : data_(std::move(data))
{
}

bool LoopingDiskReader::Open(std::wstring_view /*path*/) { return true; }

bool LoopingDiskReader::ReadInto(LARGE_INTEGER& /*addr*/,
                                 std::span<BYTE> dest) const
{
  size_t filled = 0;
  while (filled < dest.size())
  {
    const size_t chunk = std::min(dest.size() - filled, data_.size() - pos_);
    std::memcpy(dest.data() + filled, data_.data() + pos_, chunk);
    filled += chunk;
    pos_ += chunk;
    if (pos_ >= data_.size())
    {
      pos_ = 0;
    }
  }

  return true;
}

}  // namespace NtfsFuzz
