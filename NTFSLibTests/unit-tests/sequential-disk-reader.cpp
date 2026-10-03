#include "sequential-disk-reader.h"

#include <ntfs-browser/win-types.h>

#include <cstring>
#include <fstream>
#include <memory>
#include <utility>

#include <gsl/narrow>

namespace NtfsBrowserTests
{

SequentialDiskReader::SequentialDiskReader(Producer producer)
    : producer_(std::move(producer))
{
}

bool SequentialDiskReader::Open(std::wstring_view /*path*/) { return true; }

bool SequentialDiskReader::ReadInto(LARGE_INTEGER& /*addr*/,
                                    std::span<BYTE> dest) const
{
  return producer_(dest);
}

SequentialDiskReader::Producer MakeMemoryProducer(std::vector<BYTE> data)
{
  return [data = std::move(data), pos = size_t{0}](std::span<BYTE> dest) mutable
  {
    if (pos + dest.size() > data.size())
    {
      return false;
    }

    // The check above bounds pos + dest.size() by data.size().
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    std::memcpy(dest.data(), &data[pos], dest.size());
    pos += dest.size();
    return true;
  };
}

SequentialDiskReader::Producer
    MakeFileStreamProducer(std::filesystem::path path)
{
  auto const input = std::make_shared<std::ifstream>(path, std::ios::binary);

  return [input](std::span<BYTE> dest)
  {
    return static_cast<bool>(
        input->read(reinterpret_cast<char*>(dest.data()),
                    gsl::narrow<std::streamsize>(dest.size())));
  };
}

SequentialDiskReader::Producer
    MakeGeneratorProducer(std::function<void(std::span<BYTE>)> generate)
{
  return [generate = std::move(generate)](std::span<BYTE> dest)
  {
    generate(dest);
    return true;
  };
}

}  // namespace NtfsBrowserTests
