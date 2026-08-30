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

// A fake reader for tests that only care about how many bytes are read, not
// where from: addr is ignored entirely, and each ReadInto() call returns the
// next dest.size() bytes from a sequential source (Producer). Open() is a
// no-op - the source is fixed at construction time via one of the
// Make*Producer() factories below.
class SequentialDiskReader : public NtfsBrowser::IDiskReader
{
 public:
  // Fills dest with the next dest.size() bytes; returns false once the
  // source is exhausted (mirrors a real reader failing past EOF).
  using Producer = std::function<bool(std::span<BYTE> dest)>;

  explicit SequentialDiskReader(Producer producer);

  bool Open(std::wstring_view path) override;

  [[nodiscard]] bool ReadInto(LARGE_INTEGER& addr,
                              std::span<BYTE> dest) const override;

 private:
  mutable Producer producer_;
};

// Serves data out of a buffer already held in memory.
[[nodiscard]] SequentialDiskReader::Producer MakeMemoryProducer(
    std::vector<BYTE> data);

// Serves data read incrementally from a file, a chunk at a time, instead of
// preloading it all in memory.
[[nodiscard]] SequentialDiskReader::Producer MakeFileStreamProducer(
    std::filesystem::path path);

// Serves data generated on the fly - no backing store at all, useful to
// exercise huge sequential reads without materializing them.
[[nodiscard]] SequentialDiskReader::Producer MakeGeneratorProducer(
    std::function<void(std::span<BYTE>)> generate);

}  // namespace NtfsBrowserTests
