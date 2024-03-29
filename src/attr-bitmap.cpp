#include "attr-bitmap.h"
#include "attr-non-resident.h"
#include "attr-resident.h"

namespace NtfsBrowser
{

template <class TYPE_RESIDENT, Strategy S>
AttrBitmap<TYPE_RESIDENT, S>::AttrBitmap(const AttrHeaderCommon& ahc,
                                         const FileRecord<S>& fr)
    : TYPE_RESIDENT(ahc, fr)
{
  NTFS_TRACE1("Attribute: Bitmap (%sResident)\n",
              this->IsNonResident() ? "Non" : "");

  if (!this->IsDataRunOK())
  {
    bitmap_size_ = 0;
    return;
  }

  bitmap_size_ = this->GetDataSize();

  if (this->IsNonResident())
  {
    bitmap_buf_.resize(this->GetClusterSize(), 0);
    return;
  }

  bitmap_buf_.resize(bitmap_size_, 0);

  std::optional<ULONGLONG> len =
      this->ReadData(0, {bitmap_buf_.data(), bitmap_size_});
  if (len && *len == bitmap_size_)
  {
    bitmap_buf_.clear();
    NTFS_TRACE("Read Resident Bitmap data failed\n");
    return;
  }

  NTFS_TRACE1("%u bytes of resident Bitmap data read\n", bitmap_size_);
}

template <class TYPE_RESIDENT, Strategy S>
bool AttrBitmap<TYPE_RESIDENT, S>::IsClusterFree(ULONGLONG cluster)
{
  if (!this->IsDataRunOK() || bitmap_buf_.empty())
  {
    return false;
  }

  if (this->IsNonResident())
  {
    const ULONGLONG idx = cluster >> 3U;
    const DWORD clusterSize = this->GetClusterSize();

    const ULONGLONG clusterOffset = idx / clusterSize;
    cluster -= (clusterOffset * clusterSize * 8);

    // Read one cluster of data if buffer mismatch
    if (!current_cluster_ || *current_cluster_ != clusterOffset)
    {
      std::optional<ULONGLONG> len =
          this->ReadData(clusterOffset, {bitmap_buf_.data(), clusterSize});
      if (!len || *len != clusterSize)
      {
        current_cluster_ = {};
        return false;
      }

      current_cluster_ = clusterOffset;
    }
  }

  // All the Bitmap data is already in BitmapBuf
  const ULONGLONG idx = cluster >> 3U;
  // Resident data bounds check error
  if (!this->IsNonResident() && idx >= bitmap_size_)
  {
    return true;
  }

  const BYTE fac = cluster % 8;

  return (bitmap_buf_[idx] & static_cast<BYTE>(1U << fac)) == 0;
}

template class AttrBitmap<AttrNonResident<Strategy::FULL_CACHE>,
                          Strategy::FULL_CACHE>;
template class AttrBitmap<AttrNonResident<Strategy::NO_CACHE>,
                          Strategy::NO_CACHE>;
template class AttrBitmap<AttrResidentFullCache, Strategy::FULL_CACHE>;
template class AttrBitmap<AttrResidentNoCache, Strategy::NO_CACHE>;

}  // namespace NtfsBrowser