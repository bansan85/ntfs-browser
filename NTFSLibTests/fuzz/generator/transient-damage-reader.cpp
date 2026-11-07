#include <ntfs-browser/win-types.h>

#include <algorithm>
#include <cstddef>
#include <mutex>
#include <optional>
#include <span>
#include <stdexcept>
#include <string_view>
#include <utility>
#include <vector>

#include <ntfs-browser/namespace.h>
#include <ntfs-browser/tool/fuzzgen/transient-damage-reader.h>

namespace NTFS_BROWSER_NAMESPACE::Tool::FuzzGen {

// The first and the last byte that differ. The scan walks both images once.
TransientDamageReader::Range
    TransientDamageReader::Differing(const std::vector<BYTE>& intact,
                                     const std::vector<BYTE>& damaged) {
  if (intact.size() != damaged.size()) {
    throw std::invalid_argument("the two copies of the image differ in size");
  }
  const auto [first, ignored] = std::ranges::mismatch(intact, damaged);
  if (first == intact.end()) {
    return {};
  }
  const auto from_end = std::ranges::mismatch(intact.rbegin(), intact.rend(),
                                              damaged.rbegin(), damaged.rend());
  return {.begin = static_cast<ULONGLONG>(first - intact.begin()),
          .end = static_cast<ULONGLONG>(intact.rend() - from_end.in1)};
}

TransientDamageReader::TransientDamageReader(std::vector<BYTE> intact,
                                             std::vector<BYTE> damaged,
                                             std::size_t damaged_reads)
    : damage_(Differing(intact, damaged)),
      intact_(std::move(intact)),
      damaged_(std::move(damaged)),
      damaged_reads_(damaged_reads) {}

bool TransientDamageReader::Open(std::wstring_view path) {
  return intact_.Open(path);
}

bool TransientDamageReader::ReadInto(LARGE_INTEGER& addr,
                                     std::span<BYTE> dest) const {
  const auto begin = static_cast<ULONGLONG>(addr.QuadPart);
  const bool touches_damage = addr.QuadPart >= 0 && begin < damage_.end &&
                              begin + dest.size() > damage_.begin;
  if (touches_damage) {
    const std::scoped_lock lock(mutex_);
    if (reads_so_far_ < damaged_reads_) {
      ++reads_so_far_;
      return damaged_.ReadInto(addr, dest);
    }
  }
  return intact_.ReadInto(addr, dest);
}

std::optional<ULONGLONG> TransientDamageReader::Size() const {
  return intact_.Size();
}

}  // namespace NTFS_BROWSER_NAMESPACE::Tool::FuzzGen
