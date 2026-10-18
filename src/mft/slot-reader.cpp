#include "mft/slot-reader.h"

#include <ntfs-browser/win-types.h>

#include <algorithm>
#include <cstddef>
#include <span>

#include <gsl/narrow>

#include <ntfs-browser/cache/strategy.h>
#include <ntfs-browser/io/file-record.h>
#include <ntfs-browser/mft/idx.h>
#include <ntfs-browser/namespace.h>
#include <ntfs-browser/ntfs-volume.h>  // IWYU pragma: keep

#include "io/file-record-impl.h"
#include "log/ntfs-common.h"

namespace NTFS_BROWSER_NAMESPACE::Mft {

namespace {

// First slot a window may serve: below it, records are read from a fixed
// address, whether or not $MFT's extents are known.
constexpr ULONGLONG first_mapped_slot = static_cast<ULONGLONG>(Idx::User);

}  // namespace

template <Cache::Strategy S>
SlotReader<S>::SlotReader(const NtfsVolume<S>& volume, size_t block_bytes)
    : volume_(&volume),
      record_size_(volume.GetFileRecordSize()),
      // A volume that is not OK has no record size: the guards keep the
      // division defined, and GetRecordsCount() is 0 then.
      window_slots_(record_size_ != 0 ? block_bytes / record_size_ : 1),
      cluster_slots_(
          (std::max<ULONGLONG>)(1, record_size_ != 0
                                       ? volume.GetClusterSize() / record_size_
                                       : 1)),
      range_last_(volume.GetRecordsCount()) {
  // A window holds at least a whole cluster, so the one that starts at
  // record's cluster always reaches record.
  window_slots_ = (std::max)(window_slots_, cluster_slots_);
}

template <Cache::Strategy S>
void SlotReader<S>::Expect(ULONGLONG first, ULONGLONG last) noexcept {
  range_first_ = first;
  range_last_ = last;
}

template <Cache::Strategy S>
bool SlotReader<S>::ParseFileRecord(Io::FileRecord<S>& file_record,
                                    ULONGLONG record) {
  const std::span<const BYTE> bytes = Slot(file_record, record);
  if (bytes.empty()) {
    return file_record.ParseFileRecord(record);
  }
  return file_record.impl_->ParseRecord(record, bytes);
}

template <Cache::Strategy S>
std::span<const BYTE> SlotReader<S>::Slot(Io::FileRecord<S>& file_record,
                                          ULONGLONG record) {
  if (record < first_mapped_slot || record < range_first_ ||
      record >= range_last_) {
    return {};
  }
  if (record >= failed_first_ && record < failed_last_) {
    return {};
  }
  if ((record < window_first_ || record >= window_last_) &&
      !LoadWindow(file_record, record)) {
    return {};
  }

  // record lies in [window_first_, window_last_), and window_ holds exactly
  // those slots.
  const size_t offset =
      gsl::narrow<size_t>((record - window_first_) * record_size_);
  return std::span<const BYTE>{window_}.subspan(offset, record_size_);
}

template <Cache::Strategy S>
bool SlotReader<S>::LoadWindow(Io::FileRecord<S>& file_record,
                               ULONGLONG record) {
  // Back to the start of record's cluster, but not before what is allowed.
  const ULONGLONG first = (std::max)({record - record % cluster_slots_,
                                      range_first_, first_mapped_slot});
  const ULONGLONG last = first + (std::min)(range_last_ - first, window_slots_);

  // A failed read leaves no window behind, whatever it wrote.
  window_first_ = 0;
  window_last_ = 0;
  window_.resize(gsl::narrow<size_t>((last - first) * record_size_));
  if (!file_record.GetVolume().ReadMftSlots(first, window_)) {
    Log::Debug("Block read of $MFT slots [{}, {}) failed; reading them alone",
               first, last);
    failed_first_ = first;
    failed_last_ = last;
    return false;
  }

  window_first_ = first;
  window_last_ = last;
  return true;
}

template class SlotReader<Cache::Strategy::NoCache>;
template class SlotReader<Cache::Strategy::FullCache>;

}  // namespace NTFS_BROWSER_NAMESPACE::Mft
