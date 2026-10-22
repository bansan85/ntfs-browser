#pragma once

#include <ntfs-browser/win-types.h>

#include <cstddef>
#include <memory>
#include <string_view>
#include <vector>

#include <ntfs-browser/disk-reader.h>
#include <ntfs-browser/namespace.h>

namespace NTFS_BROWSER_NAMESPACE::Tests {

// Forwards every call to another IDiskReader and keeps a list of them. A test
// reads it to count the reads a feature makes, to find which ranges they
// touch, and to check that only read operations reach the reader.
class CountingDiskReader : public NTFS_BROWSER_NAMESPACE::IDiskReader {
 public:
  // The operation a recorded call was.
  enum class Kind : unsigned char { Open, Read, Size };

  // One call the reader received.
  struct Call {
    Kind kind{Kind::Read};
    // Address and length of a read; 0 for the other calls.
    ULONGLONG offset{0};
    ULONGLONG length{0};
  };

  explicit CountingDiskReader(
      std::unique_ptr<NTFS_BROWSER_NAMESPACE::IDiskReader> inner);

  bool Open(std::wstring_view path) override;

  [[nodiscard]] bool ReadInto(LARGE_INTEGER& addr,
                              std::span<BYTE> dest) const override;

  [[nodiscard]] std::optional<ULONGLONG> Size() const override;

  // Every call so far, in order.
  [[nodiscard]] const std::vector<Call>& Calls() const noexcept;

  // Reads so far.
  [[nodiscard]] size_t ReadCount() const noexcept;

  // Reads whose byte range touches [first, last).
  [[nodiscard]] size_t ReadsInRange(ULONGLONG first,
                                    ULONGLONG last) const noexcept;

  // Forgets the calls so far.
  void Reset() noexcept;

 private:
  std::unique_ptr<NTFS_BROWSER_NAMESPACE::IDiskReader> inner_;
  mutable std::vector<Call> calls_;
};

}  // namespace NTFS_BROWSER_NAMESPACE::Tests
