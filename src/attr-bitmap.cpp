#include "attr-bitmap.h"
#include "attr-non-resident.h"
#include "attr-resident.h"

namespace NtfsBrowser
{

template <class TYPE_RESIDENT>
AttrBitmap<TYPE_RESIDENT>::AttrBitmap(const AttrHeaderCommon& ahc,
                                      const FileRecord& fr)
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

  ULONGLONG len = 0;
  if (!(this->ReadData(0, bitmap_buf_.data(), bitmap_size_, len) &&
        len == bitmap_size_))
  {
    bitmap_buf_.clear();
    NTFS_TRACE("Read Resident Bitmap data failed\n");
  }
  else
  {
    NTFS_TRACE1("%u bytes of resident Bitmap data read\n", len);
  }
}

template <class TYPE_RESIDENT>
bool AttrBitmap<TYPE_RESIDENT>::IsClusterFree(ULONGLONG cluster)
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
      ULONGLONG len = 0;
      if (this->ReadData(clusterOffset, bitmap_buf_.data(), clusterSize, len) &&
          len == clusterSize)
      {
        current_cluster_ = clusterOffset;
      }
      else
      {
        current_cluster_ = {};
        return false;
      }
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

template class AttrBitmap<AttrNonResident>;
template class AttrBitmap<AttrResident>;

}  // namespace NtfsBrowser