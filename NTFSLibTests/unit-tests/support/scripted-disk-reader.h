#pragma once

#include <ntfs-browser/win-types.h>

#include <optional>
#include <span>
#include <stdexcept>
#include <string_view>
#include <utility>
#include <vector>

#include <ntfs-browser/disk-reader.h>
#include <ntfs-browser/namespace.h>

#include "memory-disk-reader.h"

namespace NTFS_BROWSER_NAMESPACE::Tests {

// A MemoryDiskReader whose size and failures a test scripts: it can declare
// another size, fail the reads that touch a byte range, or throw from them.
class ScriptedDiskReader final : public NTFS_BROWSER_NAMESPACE::IDiskReader {
 public:
  // What a read that touches the range does.
  enum class Fault : unsigned char { Fail, Throw };

  explicit ScriptedDiskReader(std::vector<BYTE> image)
      : inner_(std::move(image)) {}

  bool Open(std::wstring_view path) override { return inner_.Open(path); }

  [[nodiscard]] bool ReadInto(LARGE_INTEGER& addr,
                              std::span<BYTE> dest) const override {
    const auto begin = static_cast<ULONGLONG>(addr.QuadPart);
    if (fault_ && begin < last_ && begin + dest.size() > first_) {
      if (*fault_ == Fault::Throw) {
        throw std::runtime_error("scripted read failure");
      }
      return false;
    }
    return inner_.ReadInto(addr, dest);
  }

  [[nodiscard]] std::optional<ULONGLONG> Size() const override {
    return size_set_ ? size_ : inner_.Size();
  }

  // Declares this size instead of the image's, or none at all.
  void DeclareSize(std::optional<ULONGLONG> size) {
    size_set_ = true;
    size_ = size;
  }

  // Makes every read that touches [first, last) fail or throw.
  void FaultRange(ULONGLONG first, ULONGLONG last, Fault fault) {
    first_ = first;
    last_ = last;
    fault_ = fault;
  }

 private:
  MemoryDiskReader inner_;
  bool size_set_{false};
  std::optional<ULONGLONG> size_;
  ULONGLONG first_{0};
  ULONGLONG last_{0};
  std::optional<Fault> fault_;
};

}  // namespace NTFS_BROWSER_NAMESPACE::Tests
