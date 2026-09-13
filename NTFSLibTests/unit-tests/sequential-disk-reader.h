#pragma once

#include <filesystem>
#include <functional>
#include <span>
#include <string_view>
#include <vector>

#include <ntfs-browser/disk-reader.h>

#include <windows.h>

namespace NtfsBrowserTests
{

// A fake IDiskReader that ignores addr and serves bytes from a Producer.
class SequentialDiskReader : public NtfsBrowser::IDiskReader
{
 public:
  // Fills dest, returns false once the source is exhausted.
  using Producer = std::function<bool(std::span<BYTE> dest)>;

  explicit SequentialDiskReader(Producer producer);

  bool Open(std::wstring_view path) override;

  [[nodiscard]] bool ReadInto(LARGE_INTEGER& addr,
                              std::span<BYTE> dest) const override;

 private:
  mutable Producer producer_;
};

// Serves data out of a buffer already held in memory.
[[nodiscard]] SequentialDiskReader::Producer
    MakeMemoryProducer(std::vector<BYTE> data);

// Serves data read incrementally from a file, instead of preloading it.
[[nodiscard]] SequentialDiskReader::Producer
    MakeFileStreamProducer(std::filesystem::path path);

// Serves data generated on the fly, with no backing store.
[[nodiscard]] SequentialDiskReader::Producer
    MakeGeneratorProducer(std::function<void(std::span<BYTE>)> generate);

}
