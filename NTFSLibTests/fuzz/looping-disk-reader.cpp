#include "looping-disk-reader.h"

#include <ntfs-browser/win-types.h>

#include <algorithm>
#include <cstring>
#include <fstream>
#include <iterator>
#include <utility>

namespace NtfsFuzz
{

namespace
{

// Length of the boot sector read a NO_CACHE volume issues first: the largest
// supported sector.
constexpr size_t kBootSectorReadBytes = 4096;

// How much of that read the stream position advances by. The saved corpus was
// built when the boot sector was read in 512 bytes, and every later read of a
// testcase depends on the position.
constexpr size_t kBootSectorStreamBytes = 512;

}  // namespace

std::optional<std::vector<BYTE>>
    LoopingDiskReader::LoadFile(const std::filesystem::path& path)
{
  std::ifstream input(path, std::ios::binary);
  if (!input)
  {
    return {};
  }

  std::vector<BYTE> data(std::istreambuf_iterator<char>(input), {});
  if (data.empty())
  {
    return {};
  }

  return data;
}

LoopingDiskReader::LoopingDiskReader(std::span<const BYTE> data,
                                     std::optional<size_t> failingRead)
    : data_(data), failing_read_(failingRead)
{
}

bool LoopingDiskReader::Open(std::wstring_view /*path*/) { return true; }

bool LoopingDiskReader::ReadInto(LARGE_INTEGER& /*addr*/,
                                 std::span<BYTE> dest) const
{
  const bool firstRead = std::exchange(first_read_, false);

  // The stream position is left untouched, as after a real failed read.
  if (failing_read_ && reads_++ == *failing_read_)
  {
    return false;
  }

  size_t filled = 0;
  while (filled < dest.size())
  {
    const size_t chunk = std::min(dest.size() - filled, data_.size() - pos_);
    // filled < dest.size() by the loop condition; pos_ < data_.size() because
    // it wraps to 0 below.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    std::memcpy(&dest[filled], &data_[pos_], chunk);
    filled += chunk;
    pos_ += chunk;
    if (pos_ >= data_.size())
    {
      pos_ = 0;
    }
  }

  if (firstRead && dest.size() == kBootSectorReadBytes)
  {
    pos_ = kBootSectorStreamBytes % data_.size();
  }

  return true;
}

}  // namespace NtfsFuzz
