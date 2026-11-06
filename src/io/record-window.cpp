#include "io/record-window.h"

#include <ntfs-browser/win-types.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>
#include <utility>
#include <vector>

#include <ntfs-browser/namespace.h>

namespace NTFS_BROWSER_NAMESPACE::Io {

RecordWindow::RecordWindow(std::uint64_t budget, std::size_t record_size)
    : budget_(budget), record_size_((std::max<std::size_t>)(1, record_size)) {}

std::span<const BYTE> RecordWindow::Find(ULONGLONG slot) const {
  const auto place = places_.find(slot);
  if (place == places_.end()) {
    return {};
  }
  // A place names a run that is still held: DropOldest() erases the others.
  const Run& run = runs_.at(place->second.serial - first_serial_);
  return std::span<const BYTE>(run.bytes).subspan(
      place->second.index * record_size_, record_size_);
}

void RecordWindow::Put(ULONGLONG first_slot, std::span<const BYTE> bytes) {
  const std::size_t count = bytes.size() / record_size_;
  const std::uint64_t size = std::uint64_t{count} * record_size_;
  if (count == 0 || size > budget_) {
    return;
  }
  while (held_ + size > budget_) {
    DropOldest();
  }

  const std::uint64_t serial = first_serial_ + runs_.size();
  runs_.push_back(
      {.first_slot = first_slot,
       .bytes = std::vector<BYTE>(bytes.begin(), bytes.begin() + size)});
  held_ += size;
  for (std::size_t index = 0; index < count; index++) {
    places_.insert_or_assign(first_slot + index,
                             Place{.serial = serial, .index = index});
  }
}

void RecordWindow::DropOldest() {
  const Run& oldest = runs_.front();
  const std::size_t count = oldest.bytes.size() / record_size_;
  for (std::size_t index = 0; index < count; index++) {
    const auto place = places_.find(oldest.first_slot + index);
    // A newer run that read the same record took the place over.
    if (place != places_.end() && place->second.serial == first_serial_) {
      places_.erase(place);
    }
  }
  held_ -= oldest.bytes.size();
  runs_.pop_front();
  first_serial_++;
}

}  // namespace NTFS_BROWSER_NAMESPACE::Io
