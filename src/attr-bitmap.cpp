#include "attr-bitmap.h"

#include <ntfs-browser/strategy.h>

#include "attr-non-resident.h"
#include "attr-resident.h"
#include "ntfs-browser/win-types.h"

namespace NtfsBrowser {
namespace {

// Bits in one bitmap byte.
constexpr unsigned bits_per_byte = 8;

}  // namespace

struct AttrHeaderCommon;
template <Strategy S>
class FileRecord;

template <class Resident, Strategy S>
AttrBitmap<Resident, S>::AttrBitmap(const AttrHeaderCommon& ahc,
                                    const FileRecord<S>& file_record)
    : Resident(ahc, file_record), bitmap_size_(this->GetDataSize()) {
  Log::Trace("Attribute: Bitmap ({}Resident)",
             this->IsNonResident() ? "Non" : "");

  if (this->IsNonResident()) {
    bitmap_buf_.resize(this->GetClusterSize(), 0);
    return;
  }

  bitmap_buf_.resize(bitmap_size_, 0);

  std::optional<ULONGLONG> len =
      this->ReadData(0, {bitmap_buf_.data(), bitmap_buf_.size()});
  if (!len || *len != bitmap_size_) {
    bitmap_buf_.clear();
    Log::Warn("Read Resident Bitmap data failed");
    return;
  }

  Log::Debug("{} bytes of resident Bitmap data read", bitmap_size_);
}

template <class Resident, Strategy S>
bool AttrBitmap<Resident, S>::IsClusterFree(ULONGLONG cluster) {
  if (bitmap_buf_.empty()) {
    return false;
  }

  if (this->IsNonResident()) {
    const ULONGLONG idx = cluster >> 3U;
    const DWORD cluster_size = this->GetClusterSize();

    const ULONGLONG cluster_offset = idx / cluster_size;
    cluster -= (cluster_offset * cluster_size * bits_per_byte);

    // Read one cluster of data if buffer mismatch
    if (!current_cluster_ || *current_cluster_ != cluster_offset) {
      std::optional<ULONGLONG> len = this->ReadData(
          cluster_offset * cluster_size, {bitmap_buf_.data(), cluster_size});
      if (!len || *len != cluster_size) {
        current_cluster_ = {};
        return false;
      }

      current_cluster_ = cluster_offset;
    }
  }

  // All the Bitmap data is already in BitmapBuf
  const ULONGLONG idx = cluster >> 3U;
  // Resident data bounds check error
  if (!this->IsNonResident() && idx >= bitmap_size_) {
    return true;
  }

  const BYTE fac = cluster % 8;

  // idx is below the cluster size (non-resident) or bitmap_size_ (resident),
  // which is what bitmap_buf_ holds.
  // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
  return (bitmap_buf_[idx] & static_cast<BYTE>(1U << fac)) == 0;
}

template class AttrBitmap<AttrNonResident<Strategy::FullCache>,
                          Strategy::FullCache>;
template class AttrBitmap<AttrNonResident<Strategy::NoCache>,
                          Strategy::NoCache>;
template class AttrBitmap<AttrResidentFullCache, Strategy::FullCache>;
template class AttrBitmap<AttrResidentNoCache, Strategy::NoCache>;

}  // namespace NtfsBrowser
