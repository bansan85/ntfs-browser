#include "attr-non-resident.h"
#include "ntfs-common.h"

#include "data/run-entry.h"
#include "attr/header-non-resident.h"

namespace NtfsBrowser
{

AttrNonResident::AttrNonResident(const AttrHeaderCommon& ahc,
                                 const FileRecord& fr)
    : AttrBase(ahc, fr), AttrHeaderNR((Attr::HeaderNonResident&)ahc)
{
  UnalignedBuf = new BYTE[GetClusterSize()];

  bDataRunOK = ParseDataRun();
}

AttrNonResident::~AttrNonResident()
{
  delete UnalignedBuf;

  DataRunList.clear();
}

// Parse a single DataRun unit
BOOL AttrNonResident::PickData(const BYTE** dataRun, LONGLONG* length,
                               LONGLONG* LCNOffset)
{
  BYTE size = **dataRun;
  (*dataRun)++;
  int lengthBytes = size & 0x0F;
  int offsetBytes = size >> 4;

  if (lengthBytes > 8 || offsetBytes > 8)
  {
    NTFS_TRACE1("DataRun decode error 1: 0x%02X\n", size);
    return FALSE;
  }

  *length = 0;
  memcpy(length, *dataRun, lengthBytes);
  if (*length < 0)
  {
    NTFS_TRACE1("DataRun length error: %I64d\n", *length);
    return FALSE;
  }

  (*dataRun) += lengthBytes;
  *LCNOffset = 0;
  if (offsetBytes)  // Not Sparse File
  {
    if ((*dataRun)[offsetBytes - 1] & 0x80) *LCNOffset = -1;
    memcpy(LCNOffset, *dataRun, offsetBytes);

    (*dataRun) += offsetBytes;
  }

  return TRUE;
}

// Travers DataRun and insert into a link list
BOOL AttrNonResident::ParseDataRun()
{
  NTFS_TRACE("Parsing Non Resident DataRun\n");
  NTFS_TRACE2("Start VCN = %I64u, End VCN = %I64u\n", AttrHeaderNR.StartVCN,
              AttrHeaderNR.LastVCN);

  const BYTE* dataRun = (BYTE*)&AttrHeaderNR + AttrHeaderNR.DataRunOffset;
  LONGLONG length;
  LONGLONG LCNOffset;
  LONGLONG LCN = 0;
  ULONGLONG VCN = 0;

  while (*dataRun)
  {
    if (PickData(&dataRun, &length, &LCNOffset))
    {
      LCN += LCNOffset;
      if (LCN < 0)
      {
        NTFS_TRACE("DataRun decode error 2\n");
        return FALSE;
      }

      NTFS_TRACE2("Data length = %I64d clusters, LCN = %I64d", length, LCN);
      NTFS_TRACE(LCNOffset == 0 ? ", Sparse Data\n" : "\n");

      // Store LCN, Data size (clusters) into list
      Data::RunEntry* dr = new Data::RunEntry;
      dr->LCN = (LCNOffset == 0) ? -1 : LCN;
      dr->Clusters = length;
      dr->StartVCN = VCN;
      VCN += length;
      dr->LastVCN = VCN - 1;

      if (dr->LastVCN <= (AttrHeaderNR.LastVCN - AttrHeaderNR.StartVCN))
      {
        DataRunList.push_back(*dr);
      }
      else
      {
        NTFS_TRACE("DataRun decode error: VCN exceeds bound\n");

        // Remove entries
        DataRunList.clear();

        return FALSE;
      }
    }
    else
      break;
  }

  return TRUE;
}

// Read clusters from disk, or sparse data
// *actural = Clusters acturally read
BOOL AttrNonResident::ReadClusters(void* buf, DWORD clusters,
                                   LONGLONG lcn) const
{
  if (lcn == -1)  // sparse data
  {
    NTFS_TRACE("Sparse Data, Fill the buffer with 0\n");

    // Fill the buffer with 0
    memset(buf, 0, clusters * GetClusterSize());

    return TRUE;
  }

  LARGE_INTEGER addr;
  DWORD len;

  addr.QuadPart = lcn * GetClusterSize();
  len = SetFilePointer(GetHandle(), addr.LowPart, &addr.HighPart, FILE_BEGIN);

  if (len == (DWORD)-1 && GetLastError() != NO_ERROR)
  {
    NTFS_TRACE1("Cannot locate cluster with LCN %I64d\n", lcn);
  }
  else
  {
    if (ReadFile(GetHandle(), buf, clusters * GetClusterSize(), &len,
                 nullptr) &&
        len == clusters * GetClusterSize())
    {
      NTFS_TRACE2("Successfully read %u clusters from LCN %I64d\n", clusters,
                  lcn);
      return TRUE;
    }
    else
    {
      NTFS_TRACE1("Cannot read cluster with LCN %I64d\n", lcn);
    }
  }

  return FALSE;
}

// Read Data, cluster based
// clusterNo: Begnning cluster Number
// clusters: Clusters to read
// bufv, bufLen: Returned data
// *actural = Number of bytes acturally read
BOOL AttrNonResident::ReadVirtualClusters(ULONGLONG vcn, DWORD clusters,
                                          void* bufv, DWORD bufLen,
                                          DWORD* actural) const
{
  _ASSERT(bufv);
  _ASSERT(clusters);

  *actural = 0;
  BYTE* buf = (BYTE*)bufv;

  // Verify if clusters exceeds DataRun bounds
  if (vcn + clusters > (AttrHeaderNR.LastVCN - AttrHeaderNR.StartVCN + 1))
  {
    NTFS_TRACE("Cluster exceeds DataRun bounds\n");
    return FALSE;
  }

  // Verify buffer size
  if (bufLen < clusters * GetClusterSize())
  {
    NTFS_TRACE("Buffer size too small\n");
    return FALSE;
  }

  // Traverse the DataRun List to find the according LCN
  for (const Data::RunEntry& dr : DataRunList)
  {
    if (vcn >= dr.StartVCN && vcn <= dr.LastVCN)
    {
      DWORD clustersToRead;

      ULONGLONG vcns =
          dr.LastVCN - vcn + 1;  // Clusters from read pointer to the end

      if ((ULONGLONG)clusters > vcns)  // Fragmented data, we must go on
        clustersToRead = (DWORD)vcns;
      else
        clustersToRead = clusters;
      if (ReadClusters(buf, clustersToRead, dr.LCN + (vcn - dr.StartVCN)))
      {
        buf += clustersToRead * GetClusterSize();
        clusters -= clustersToRead;
        *actural += clustersToRead;
        vcn += clustersToRead;
      }
      else
        break;

      if (clusters == 0) break;
    }
  }

  *actural *= GetClusterSize();
  return TRUE;
}

// Judge if the DataRun is successfully parsed
BOOL AttrNonResident::IsDataRunOK() const { return bDataRunOK; }

// Return Actural Data Size
// *allocSize = Allocated Size
ULONGLONG AttrNonResident::GetDataSize() const { return AttrHeaderNR.RealSize; }

// Read "bufLen" bytes from "offset" into "bufv"
// Number of bytes acturally read is returned in "*actural"
BOOL AttrNonResident::ReadData(ULONGLONG offset, void* bufv, DWORD bufLen,
                               DWORD* actural) const
{
  // Hard disks can only be accessed by sectors
  // To be simple and efficient, only implemented cluster based accessing
  // So cluster unaligned data address should be processed carefully here

  _ASSERT(bufv);

  *actural = 0;
  if (bufLen == 0) return TRUE;

  // Bounds check
  if (offset > AttrHeaderNR.RealSize) return FALSE;
  if ((offset + bufLen) > AttrHeaderNR.RealSize)
    bufLen = (DWORD)(AttrHeaderNR.RealSize - offset);

  DWORD len;
  BYTE* buf = (BYTE*)bufv;

  // First cluster Number
  ULONGLONG startVCN = offset / GetClusterSize();
  // Bytes in first cluster
  DWORD startBytes = GetClusterSize() - (DWORD)(offset % GetClusterSize());
  // Read first cluster
  if (startBytes != GetClusterSize())
  {
    // First cluster, Unaligned
    if (ReadVirtualClusters(startVCN, 1, UnalignedBuf, GetClusterSize(),
                            &len) &&
        len == GetClusterSize())
    {
      len = (startBytes < bufLen) ? startBytes : bufLen;
      memcpy(buf, UnalignedBuf + GetClusterSize() - startBytes, len);
      buf += len;
      bufLen -= len;
      *actural += len;
      startVCN++;
    }
    else
      return FALSE;
  }
  if (bufLen == 0) return TRUE;

  DWORD alignedClusters = bufLen / GetClusterSize();
  if (alignedClusters)
  {
    // Aligned clusters
    DWORD alignedSize = alignedClusters * GetClusterSize();
    if (ReadVirtualClusters(startVCN, alignedClusters, buf, alignedSize,
                            &len) &&
        len == alignedSize)
    {
      startVCN += alignedClusters;
      buf += alignedSize;
      bufLen %= GetClusterSize();
      *actural += len;

      if (bufLen == 0) return TRUE;
    }
    else
      return FALSE;
  }

  // Last cluster, Unaligned
  if (ReadVirtualClusters(startVCN, 1, UnalignedBuf, GetClusterSize(), &len) &&
      len == GetClusterSize())
  {
    memcpy(buf, UnalignedBuf, bufLen);
    *actural += bufLen;

    return TRUE;
  }
  else
    return FALSE;
}

}  // namespace NtfsBrowser