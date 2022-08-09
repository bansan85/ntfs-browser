#pragma once

#include <ntfs-browser/file-record.h>
#include <ntfs-browser/data/attr-header-common.h>
#include "ntfs-common.h"

// OK

namespace NtfsBrowser
{

////////////////////////////////////////////
// Attribute: Bitmap
////////////////////////////////////////////
template <class TYPE_RESIDENT>
class AttrBitmap : public TYPE_RESIDENT
{
 public:
  AttrBitmap(const AttrHeaderCommon& ahc, const FileRecord& fr)
      : TYPE_RESIDENT(ahc, fr)
  {
    NTFS_TRACE1("Attribute: Bitmap (%sResident)\n",
                this->IsNonResident() ? "Non" : "");

    current_cluster_ = -1;

    if (this->IsDataRunOK())
    {
      bitmap_size_ = this->GetDataSize();

      if (this->IsNonResident())
        bitmap_buf_.resize(this->GetClusterSize(), 0);
      else
      {
        bitmap_buf_.resize(bitmap_size_, 0);

        DWORD len;
        if (!(this->ReadData(0, bitmap_buf_.data(), (DWORD)bitmap_size_, len) &&
              len == (DWORD)bitmap_size_))
        {
          bitmap_buf_.clear();
          NTFS_TRACE("Read Resident Bitmap data failed\n");
        }
        else
        {
          NTFS_TRACE1("%u bytes of resident Bitmap data read\n", len);
        }
      }
    }
    else
    {
      bitmap_size_ = 0;
    }
  }
  AttrBitmap(AttrBitmap&& other) noexcept = delete;
  AttrBitmap(AttrBitmap const& other) = delete;
  AttrBitmap& operator=(AttrBitmap&& other) noexcept = delete;
  AttrBitmap& operator=(AttrBitmap const& other) = delete;
  virtual ~AttrBitmap() { NTFS_TRACE("AttrBitmap deleted\n"); }

 private:
  ULONGLONG bitmap_size_;         // Bitmap data size
  std::vector<BYTE> bitmap_buf_;  // Bitmap data buffer
  LONGLONG current_cluster_;

 public:
  // Verify if a single cluster is free
  bool IsClusterFree(ULONGLONG cluster) const
  {
    if (!this->IsDataRunOK() || bitmap_buf_.empty()) return false;

    if (this->IsNonResident())
    {
      LONGLONG idx = (LONGLONG)cluster >> 3;
      DWORD clusterSize = ((NtfsVolume*)this->Volume)->GetClusterSize();

      LONGLONG clusterOffset = idx / clusterSize;
      cluster -= (clusterOffset * clusterSize * 8);

      // Read one cluster of data if buffer mismatch
      if (current_cluster_ != clusterOffset)
      {
        DWORD len;
        if (this->ReadData(clusterOffset, bitmap_buf_, clusterSize, &len) &&
            len == clusterSize)
        {
          current_cluster_ = clusterOffset;
        }
        else
        {
          current_cluster_ = -1;
          return false;
        }
      }
    }

    // All the Bitmap data is already in BitmapBuf
    DWORD idx = (DWORD)(cluster >> 3);
    if (!this->IsNonResident())
    {
      if (idx >= bitmap_size_) return TRUE;  // Resident data bounds check error
    }

    BYTE fac = (BYTE)(cluster % 8);

    return (bitmap_buf_[idx] & (1 << fac)) == 0;
  }

};  // AttrBitmap

}  // namespace NtfsBrowser