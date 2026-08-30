#include "sequential-disk-reader.h"

#include <cstring>
#include <fstream>
#include <memory>

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
  return [data = std::move(data), pos = size_t{0}](
             std::span<BYTE> dest) mutable
  {
    if (pos + dest.size() > data.size())
    {
      return false;
    }

    std::memcpy(dest.data(), data.data() + pos, dest.size());
    pos += dest.size();
    return true;
  };
}

SequentialDiskReader::Producer MakeFileStreamProducer(
    std::filesystem::path path)
{
  auto in = std::make_shared<std::ifstream>(path, std::ios::binary);

  return [in](std::span<BYTE> dest)
  {
    return static_cast<bool>(
        in->read(reinterpret_cast<char*>(dest.data()),
                  static_cast<std::streamsize>(dest.size())));
  };
}

SequentialDiskReader::Producer MakeGeneratorProducer(
    std::function<void(std::span<BYTE>)> generate)
{
  return [generate = std::move(generate)](std::span<BYTE> dest)
  {
    generate(dest);
    return true;
  };
}

}  // namespace NtfsBrowserTests
