#include <gsl/narrow>
#include <gsl/pointers>

#include <ntfs-browser/ntfs-volume.h>

#include "attr-non-resident.h"
#include "attr/header-non-resident.h"
#include "data/run-entry.h"
#include "ntfs-common.h"

namespace NtfsBrowser
{

AttrNonResident::AttrNonResident(const AttrHeaderCommon& ahc,
                                 const FileRecord& fr)
    : AttrBase(ahc, fr),
      attr_header_nr_(reinterpret_cast<const Attr::HeaderNonResident&>(ahc)),
      data_run_ok_(ParseDataRun())
{
}

// Parse a single DataRun unit
bool AttrNonResident::PickData(const BYTE*& dataRun, ULONGLONG& length,
                               LONGLONG& LCNOffset) noexcept
{
  union Length
  {
    struct
    {
      unsigned char lengthBytes : 4;
      unsigned char offsetBytes : 4;
    };
    BYTE size;
  };
  const Length size{.size = *dataRun};
  dataRun++;

  if (size.lengthBytes > sizeof(ULONGLONG) ||
      size.offsetBytes > sizeof(LONGLONG))
  {
    NTFS_TRACE1("DataRun decode error 1: 0x%02X\n", size);
    return false;
  }

  length = 0;
  memcpy(&length, dataRun, size.lengthBytes);

  dataRun += size.lengthBytes;
  if (size.offsetBytes != 0)  // Not Sparse File
  {
    if (static_cast<CHAR>(dataRun[size.offsetBytes - 1]) < 0)
    {
      // Negative the number read.
      LCNOffset = -1;
    }
    else
    {
      LCNOffset = 0;
    }
    memcpy(&LCNOffset, dataRun, size.offsetBytes);

    dataRun += size.offsetBytes;
  }
  else
  {
    LCNOffset = 0;
  }

  return true;
}

// Travers DataRun and insert into a link list
bool AttrNonResident::ParseDataRun()
{
  NTFS_TRACE("Parsing Non Resident DataRun\n");
  NTFS_TRACE2("Start VCN = %I64u, End VCN = %I64u\n", attr_header_nr_.start_vcn,
              attr_header_nr_.last_vcn);

  const BYTE* data_run = reinterpret_cast<const BYTE*>(&attr_header_nr_) +
                         attr_header_nr_.data_run_offset;
  ULONGLONG length = 0;
  LONGLONG lcn_offset = 0;
  LONGLONG lcn = 0;
  ULONGLONG vcn = 0;

  while (*data_run != 0)
  {
    if (!PickData(data_run, length, lcn_offset))
    {
      break;
    }

    lcn += lcn_offset;
    if (lcn < 0)
    {
      NTFS_TRACE("DataRun decode error 2\n");
      return false;
    }

    NTFS_TRACE2("Data length = %I64d clusters, LCN = %I64d", length, lcn);
    NTFS_TRACE(lcn_offset == 0 ? ", Sparse Data\n" : "\n");

    // Store LCN, Data size (clusters) into list
    Data::RunEntry dr;
    dr.lcn = (lcn_offset == 0) ? std::optional<ULONGLONG>{} : lcn;
    dr.clusters = length;
    dr.start_vcn = vcn;
    vcn += length;
    dr.last_vcn = vcn - 1;

    if (dr.last_vcn > (attr_header_nr_.last_vcn - attr_header_nr_.start_vcn))
    {
      NTFS_TRACE("DataRun decode error: VCN exceeds bound\n");

      // Remove entries
      data_run_list_.clear();

      return false;
    }

    data_run_list_.push_back(dr);
  }

  return true;
}

// Read clusters from disk, or sparse data
// *actural = Clusters acturally read
bool AttrNonResident::ReadClusters(void* buf, ULONGLONG clusters,
                                   std::optional<ULONGLONG> start_lcn,
                                   ULONGLONG offset) const
{
  if (!start_lcn)  // sparse data
  {
    NTFS_TRACE("Sparse Data, Fill the buffer with 0\n");

    // Fill the buffer with 0
    memset(buf, 0, clusters * GetClusterSize());

    return true;
  }
  const ULONGLONG lcn = *start_lcn + offset;

  LARGE_INTEGER addr;

  addr.QuadPart = gsl::narrow<LONGLONG>(lcn * GetClusterSize());

  if (!volume_.Read(addr, gsl::narrow<DWORD>(clusters * GetClusterSize()), buf))
  {
    NTFS_TRACE1("Cannot read cluster with LCN %I64d\n", lcn);
    return false;
  }

  NTFS_TRACE2("Successfully read %u clusters from LCN %I64d\n", clusters, lcn);
  return true;
}

// Read Data, cluster based
// clusterNo: Begnning cluster Number
// clusters: Clusters to read
// bufv, bufLen: Returned data
// *actural = Number of bytes acturally read
bool AttrNonResident::ReadVirtualClusters(ULONGLONG vcn, ULONGLONG clusters,
                                          void* bufv, ULONGLONG bufLen,
                                          ULONGLONG& actural) const
{
  _ASSERT(bufv);
  _ASSERT(clusters);

  actural = 0;
  BYTE* buf = static_cast<BYTE*>(bufv);

  // Verify if clusters exceeds DataRun bounds
  if (vcn + clusters > attr_header_nr_.last_vcn - attr_header_nr_.start_vcn + 1)
  {
    NTFS_TRACE("Cluster exceeds DataRun bounds\n");
    return false;
  }

  // Verify buffer size
  if (bufLen < clusters * GetClusterSize())
  {
    NTFS_TRACE("Buffer size too small\n");
    return false;
  }

  // Traverse the DataRun List to find the according LCN
  for (const Data::RunEntry& dr : data_run_list_)
  {
    if (vcn >= dr.start_vcn && vcn <= dr.last_vcn)
    {
      // Clusters from read pointer to the end
      const ULONGLONG vcns = dr.last_vcn - vcn + 1;
      // Fragmented data, we must go on
      const ULONGLONG clustersToRead = clusters > vcns ? vcns : clusters;

      if (!ReadClusters(buf, clustersToRead, dr.lcn, vcn - dr.start_vcn))
      {
        break;
      }

      buf += static_cast<ULONGLONG>(clustersToRead) * GetClusterSize();
      clusters -= clustersToRead;
      actural += clustersToRead;
      vcn += clustersToRead;

      if (clusters == 0)
      {
        break;
      }
    }
  }

  actural *= GetClusterSize();
  return true;
}

// Judge if the DataRun is successfully parsed
bool AttrNonResident::IsDataRunOK() const noexcept { return data_run_ok_; }

// Return Actural Data Size
// *allocSize = Allocated Size
// not no except
ULONGLONG AttrNonResident::GetDataSize() const noexcept
{
  return attr_header_nr_.real_size;
}

// Read "bufLen" bytes from "offset" into "bufv"
// Number of bytes acturally read is returned in "*actural"
bool AttrNonResident::ReadData(ULONGLONG offset, gsl::not_null<void*> bufv,
                               ULONGLONG bufLen, ULONGLONG& actural) const
{
  std::vector<BYTE> unaligned_buf;
  unaligned_buf.resize(GetClusterSize());

  // Hard disks can only be accessed by sectors
  // To be simple and efficient, only implemented cluster based accessing
  // So cluster unaligned data address should be processed carefully here

  BYTE* buf = static_cast<BYTE*>(bufv.get());

  actural = 0;
  if (bufLen == 0)
  {
    return true;
  }

  // Bounds check
  if (offset > attr_header_nr_.real_size)
  {
    return false;
  }
  if (offset + bufLen > attr_header_nr_.real_size)
  {
    bufLen = gsl::narrow<DWORD>(attr_header_nr_.real_size - offset);
  }

  // First cluster Number
  ULONGLONG start_vcn = offset / GetClusterSize();
  // Bytes in first cluster
  const auto start_bytes =
      gsl::narrow<DWORD>(GetClusterSize() - (offset % GetClusterSize()));
  // Read first cluster
  if (start_bytes != GetClusterSize())
  {
    ULONGLONG len = 0;
    // First cluster, Unaligned
    if (!ReadVirtualClusters(start_vcn, 1, unaligned_buf.data(),
                             GetClusterSize(), len) ||
        len != GetClusterSize())
    {
      return false;
    }

    len = (start_bytes < bufLen) ? start_bytes : bufLen;
    memcpy(buf, &unaligned_buf[GetClusterSize() - start_bytes], len);
    buf += len;
    bufLen -= len;
    actural += len;
    start_vcn++;
  }
  if (bufLen == 0)
  {
    return true;
  }

  const ULONGLONG alignedClusters = bufLen / GetClusterSize();
  if (alignedClusters != 0)
  {
    // Aligned clusters
    ULONGLONG alignedSize = alignedClusters * GetClusterSize();
    ULONGLONG len = 0;
    if (!ReadVirtualClusters(start_vcn, alignedClusters, buf, alignedSize,
                             len) ||
        len != alignedSize)
    {
      return false;
    }

    start_vcn += alignedClusters;
    buf += alignedSize;
    bufLen %= GetClusterSize();
    actural += len;

    if (bufLen == 0)
    {
      return true;
    }
  }

  // Last cluster, Unaligned
  ULONGLONG len = 0;
  if (!ReadVirtualClusters(start_vcn, 1, unaligned_buf.data(), GetClusterSize(),
                           len) ||
      len != GetClusterSize())
  {
    return false;
  }

  memcpy(buf, unaligned_buf.data(), bufLen);
  actural += bufLen;

  return true;
}

}  // namespace NtfsBrowser