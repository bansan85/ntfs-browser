#pragma once

#include <ntfs-browser/file-record.h>
#include <ntfs-browser/data/attr-header-common.h>
#include <ntfs-browser/ntfs-common.h>

namespace NtfsBrowser
{

////////////////////////////////////////////
// Attribute: Bitmap
////////////////////////////////////////////
template <class TYPE_RESIDENT>
class AttrBitmap : public TYPE_RESIDENT
{
 public:
  AttrBitmap(const AttrHeaderCommon* ahc, const FileRecord* fr)
      : TYPE_RESIDENT(ahc, fr)
  {
    NTFS_TRACE1("Attribute: Bitmap (%sResident)\n",
                IsNonResident() ? "Non" : "");

    CurrentCluster = -1;

    if (IsDataRunOK())
    {
      BitmapSize = GetDataSize();

      if (IsNonResident())
        BitmapBuf = new BYTE[_ClusterSize];
      else
      {
        BitmapBuf = new BYTE[(DWORD)BitmapSize];

        DWORD len;
        if (!(ReadData(0, BitmapBuf, (DWORD)BitmapSize, &len) &&
              len == (DWORD)BitmapSize))
        {
          BitmapBuf = NULL;
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
      BitmapSize = 0;
      BitmapBuf = 0;
    }
  }
  virtual ~AttrBitmap()
  {
    if (BitmapBuf) delete BitmapBuf;

    NTFS_TRACE("AttrBitmap deleted\n");
  }

 private:
  ULONGLONG BitmapSize;  // Bitmap data size
  BYTE* BitmapBuf;       // Bitmap data buffer
  LONGLONG CurrentCluster;

 public:
  // Verify if a single cluster is free
  BOOL IsClusterFree(const ULONGLONG& cluster) const
  {
    if (!IsDataRunOK() || !BitmapBuf) return FALSE;

    if (IsNonResident())
    {
      LONGLONG idx = (LONGLONG)cluster >> 3;
      DWORD clusterSize = ((NtfsVolume*)Volume)->GetClusterSize();

      LONGLONG clusterOffset = idx / clusterSize;
      cluster -= (clusterOffset * clusterSize * 8);

      // Read one cluster of data if buffer mismatch
      if (CurrentCluster != clusterOffset)
      {
        DWORD len;
        if (ReadData(clusterOffset, BitmapBuf, clusterSize, &len) &&
            len == clusterSize)
        {
          CurrentCluster = clusterOffset;
        }
        else
        {
          CurrentCluster = -1;
          return FALSE;
        }
      }
    }

    // All the Bitmap data is already in BitmapBuf
    DWORD idx = (DWORD)(cluster >> 3);
    if (IsNonResident() == FALSE)
    {
      if (idx >= BitmapSize) return TRUE;  // Resident data bounds check error
    }

    BYTE fac = (BYTE)(cluster % 8);

    return ((BitmapBuf[idx] & (1 << fac)) == 0);
  }

};  // AttrBitmap

}  // namespace NtfsBrowser