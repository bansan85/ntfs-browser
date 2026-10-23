#include "counting-disk-reader.h"

#include <ntfs-browser/win-types.h>

#include <algorithm>
#include <cstddef>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

#include <ntfs-browser/disk-reader.h>
#include <ntfs-browser/namespace.h>

namespace NTFS_BROWSER_NAMESPACE::Tests {

CountingDiskReader::CountingDiskReader(
    std::unique_ptr<NTFS_BROWSER_NAMESPACE::IDiskReader> inner)
    : inner_(std::move(inner)) {}

bool CountingDiskReader::Open(std::wstring_view path) {
  calls_.push_back({.kind = Kind::Open});
  return inner_->Open(path);
}

bool CountingDiskReader::ReadInto(LARGE_INTEGER& addr,
                                  std::span<BYTE> dest) const {
  const auto offset = static_cast<ULONGLONG>(addr.QuadPart);
  calls_.push_back(
      {.kind = Kind::Read, .offset = offset, .length = dest.size()});
  if (!dest.empty() && offset < fail_last_ &&
      offset + dest.size() > fail_first_) {
    return false;
  }
  return inner_->ReadInto(addr, dest);
}

std::optional<ULONGLONG> CountingDiskReader::Size() const {
  calls_.push_back({.kind = Kind::Size});
  return inner_->Size();
}

const std::vector<CountingDiskReader::Call>&
    CountingDiskReader::Calls() const noexcept {
  return calls_;
}

size_t CountingDiskReader::ReadCount() const noexcept {
  return static_cast<size_t>(
      std::ranges::count(calls_, Kind::Read, &Call::kind));
}

size_t CountingDiskReader::ReadsInRange(ULONGLONG first,
                                        ULONGLONG last) const noexcept {
  return static_cast<size_t>(
      std::ranges::count_if(calls_, [first, last](const Call& call) {
        return call.kind == Kind::Read && call.length != 0 &&
               call.offset < last && call.offset + call.length > first;
      }));
}

void CountingDiskReader::Reset() noexcept { calls_.clear(); }

void CountingDiskReader::FailReadsIn(ULONGLONG first, ULONGLONG last) noexcept {
  fail_first_ = first;
  fail_last_ = last;
}

}  // namespace NTFS_BROWSER_NAMESPACE::Tests
