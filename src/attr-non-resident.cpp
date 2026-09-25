#include "attr-non-resident.h"

#include <cassert>
#include <cstring>
#include <exception>
#include <limits>
#include <stdexcept>

#include <gsl/narrow>
#include <gsl/pointers>

#include <ntfs-browser/ntfs-volume.h>

#include "attr/header-non-resident.h"
#include "data/run-entry.h"
#include "ntfs-common.h"

#if defined(NTFS_BROWSER_ENABLE_EFS_CRYPTOPP) || \
    (defined(_WIN32) && defined(NTFS_BROWSER_ENABLE_EFS_BCRYPT))
  #include "efs/efs-context.h"
#endif
#ifdef NTFS_BROWSER_ENABLE_DECOMPRESSION
  #include "lznt1/decompress.h"
#endif

namespace NtfsBrowser
{

namespace
{
// Max comp_unit_size exponent; keeps 2^comp_unit_size from overflowing
// before use.
constexpr WORD kMaxCompUnitSizeShift = 16;

// Real units are <=64KiB (16 clusters * 4KB); 1MiB caps a forged
// comp_unit_size from over-allocating.
constexpr ULONGLONG kMaxCompressionUnitSize = 1024ULL * 1024ULL;
}  // namespace

template <Strategy S>
AttrNonResident<S>::AttrNonResident(const AttrHeaderCommon& ahc,
                                    const FileRecord<S>& fr)
    : AttrBase<S>(ahc, fr),
      attr_header_nr_(reinterpret_cast<const Attr::HeaderNonResident&>(ahc))
{
  // total_size already covers this field (ParseAttrs()); start_vcn must be
  // unit-aligned or units decode against the wrong window.
  if (Attr::HasCompressedSizeField(attr_header_nr_))
  {
#ifndef NTFS_BROWSER_ENABLE_DECOMPRESSION
    // Decompression is not compiled in: reject a compressed attribute
    // outright, exactly as before compression support existed.
    throw std::runtime_error(
        "Compressed attribute rejected: decompression is not compiled "
        "in.\n");
#else
    if (attr_header_nr_.comp_unit_size > kMaxCompUnitSizeShift)
    {
      throw std::runtime_error("Compression unit size is out of range.\n");
    }

    comp_unit_clusters_ = 1ULL << attr_header_nr_.comp_unit_size;
    const ULONGLONG unitSize = comp_unit_clusters_ * this->GetClusterSize();
    if (unitSize == 0 || unitSize > kMaxCompressionUnitSize)
    {
      throw std::runtime_error("Compression unit size is implausibly large.\n");
    }

    if (attr_header_nr_.start_vcn % comp_unit_clusters_ != 0)
    {
      throw std::runtime_error(
          "Compressed attribute start VCN is not compression unit "
          "aligned.\n");
    }

    LogDebug(
        "Compressed attribute: {} clusters ({} bytes) per compression unit",
        comp_unit_clusters_, unitSize);
    LogDebug("Compressed size = {} bytes",
             Attr::CompressedSize(attr_header_nr_));
#endif
  }

  ParseDataRun();
}

// Parse a single DataRun unit. "end" bounds dataRun to the attribute
// (already validated against the record buffer by FileRecord::ParseAttrs);
// data_run_offset and the run stream itself are attacker-controlled and
// otherwise unbounded.
template <Strategy S>
bool AttrNonResident<S>::PickData(const BYTE*& dataRun, const BYTE* end,
                                  ULONGLONG& length,
                                  LONGLONG& LCNOffset) noexcept
{
  if (dataRun >= end)
  {
    return false;
  }

  union Length
  {
    struct
    {
      BYTE lengthBytes : 4;
      BYTE offsetBytes : 4;
    };
    BYTE size;
  };
  const Length size{.size = *dataRun};
  dataRun++;

  if (size.lengthBytes > sizeof(ULONGLONG) ||
      size.offsetBytes > sizeof(LONGLONG))
  {
    LogWarn("DataRun decode error 1: 0x{:02X}", size.size);
    return false;
  }

  if (end - dataRun < static_cast<ptrdiff_t>(size.lengthBytes) +
                          static_cast<ptrdiff_t>(size.offsetBytes))
  {
    LogWarn("DataRun decode error: run exceeds attribute bounds");
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

// Traverse DataRun and append entries to the run list. Stops at the first
// decode or bounds error; entries parsed before that error are kept.
template <Strategy S>
void AttrNonResident<S>::ParseDataRun()
{
  LogTrace("Parsing Non Resident DataRun");
  LogDebug("Start VCN = {}, End VCN = {}", attr_header_nr_.start_vcn,
           attr_header_nr_.last_vcn);

  const BYTE* const attr_start =
      reinterpret_cast<const BYTE*>(&attr_header_nr_);
  const BYTE* data_run = attr_start + attr_header_nr_.data_run_offset;
  const BYTE* const end = attr_start + attr_header_nr_.header.total_size;
  ULONGLONG length = 0;
  LONGLONG lcn_offset = 0;
  LONGLONG lcn = 0;
  ULONGLONG vcn = 0;

  while (data_run < end && *data_run != 0)
  {
    if (!PickData(data_run, end, length, lcn_offset))
    {
      break;
    }

    lcn += lcn_offset;
    if (lcn < 0)
    {
      LogWarn("DataRun decode error 2");
      break;
    }

    LogDebug("Data length = {} clusters, LCN = {}{}", length, lcn,
             lcn_offset == 0 ? ", Sparse Data" : "");

    // Store LCN, Data size (clusters) into list
    Data::RunEntry dr;
    dr.lcn = (lcn_offset == 0) ? std::optional<ULONGLONG>{} : lcn;
    dr.clusters = length;
    dr.start_vcn = vcn;
    vcn += length;
    dr.last_vcn = vcn - 1;

    if (dr.last_vcn > (attr_header_nr_.last_vcn - attr_header_nr_.start_vcn))
    {
      LogWarn("DataRun decode error: VCN exceeds bound");
      break;
    }

    data_run_list_.push_back(dr);
  }
}

// Read clusters from disk, or sparse data
// *actural = Clusters acturally read
template <Strategy S>
std::optional<std::span<const BYTE>>
    AttrNonResident<S>::ReadClusters(ULONGLONG clusters, ULONGLONG start_lcn,
                                     ULONGLONG offset) const
{
  const ULONGLONG lcn = start_lcn + offset;

  LARGE_INTEGER addr;

  // lcn and clusters are both attacker-controlled and otherwise unbounded,
  // so gsl::narrow() below can throw a gsl::narrowing_error.
  try
  {
    addr.QuadPart = gsl::narrow<LONGLONG>(lcn * this->GetClusterSize());
  }
  catch (const std::exception& e)
  {
    LogError("Cannot read cluster with LCN {}", lcn);
    LogException(e);
    return {};
  }

  std::optional<std::span<const BYTE>> buffer;
  try
  {
    buffer = this->volume_.Read(
        addr, gsl::narrow<DWORD>(clusters * this->GetClusterSize()));
  }
  catch (const std::exception& e)
  {
    LogError("Cannot read cluster with LCN {}", lcn);
    LogException(e);
    return {};
  }

  if (!buffer)
  {
    LogError("Cannot read cluster with LCN {}", lcn);
    return {};
  }

  LogTrace("Successfully read {} clusters from LCN {}", clusters, lcn);
  return buffer;
}

// Number of virtual clusters this attribute (or, for an attribute split
// across an $ATTRIBUTE_LIST, this fragment of it) describes.
template <Strategy S>
ULONGLONG AttrNonResident<S>::TotalClusters() const noexcept
{
  return attr_header_nr_.last_vcn - attr_header_nr_.start_vcn + 1;
}

// Clusters belonging to the compression unit starting at "unitFirstVcn":
// a whole unit, except for a trailing partial unit at the attribute's end.
template <Strategy S>
ULONGLONG
    AttrNonResident<S>::UnitClusters(ULONGLONG unitFirstVcn) const noexcept
{
  const ULONGLONG remaining = TotalClusters() - unitFirstVcn;
  return (remaining < comp_unit_clusters_) ? remaining : comp_unit_clusters_;
}

// Counts the real (non-sparse) clusters at the start of a compression unit.
// Returns an empty optional if the unit is not fully mapped, or if a real
// run follows a hole within the unit - layouts a compression unit cannot
// legally have.
template <Strategy S>
std::optional<ULONGLONG> AttrNonResident<S>::LeadingRealClusters(
    ULONGLONG unitFirstVcn, ULONGLONG unitClusters) const noexcept
{
  const ULONGLONG unitEnd = unitFirstVcn + unitClusters;
  ULONGLONG vcn = unitFirstVcn;
  ULONGLONG realClusters = 0;
  bool sawHole = false;

  for (const Data::RunEntry& dr : data_run_list_)
  {
    if (vcn >= unitEnd)
    {
      break;
    }
    if (dr.last_vcn < vcn)
    {
      // Entirely before the unit (or before what has been counted so far).
      continue;
    }
    if (dr.start_vcn > vcn)
    {
      LogWarn("Compression unit at VCN {} is not fully mapped", unitFirstVcn);
      return {};
    }

    const ULONGLONG inRun = dr.last_vcn - vcn + 1;
    const ULONGLONG left = unitEnd - vcn;
    const ULONGLONG take = (inRun < left) ? inRun : left;

    if (dr.lcn)
    {
      if (sawHole)
      {
        LogWarn("Compression unit at VCN {} has real clusters after a hole",
                unitFirstVcn);
        return {};
      }
      realClusters += take;
    }
    else
    {
      sawHole = true;
    }

    vcn += take;
  }

  if (vcn != unitEnd)
  {
    // The run list ran out before the unit did.
    LogWarn("Compression unit at VCN {} is not fully mapped", unitFirstVcn);
    return {};
  }

  return realClusters;
}

// Materializes one whole compression unit - decompressing it if needed - and
// returns it, or nullptr on a read/decompression failure. The returned unit
// is retained in comp_unit_cache_, so it is never decompressed twice within
// one ReadData() call, nor - under FULL_CACHE - across calls.
template <Strategy S>
const std::vector<BYTE>*
    AttrNonResident<S>::GetCompressionUnit(ULONGLONG unitIndex) const
{
  const auto cached = comp_unit_cache_.find(unitIndex);
  if (cached != comp_unit_cache_.end())
  {
    LogDebug("Compression unit {} served from cache", unitIndex);
    return &cached->second;
  }

  const ULONGLONG unitFirstVcn = unitIndex * comp_unit_clusters_;
  if (unitFirstVcn >= TotalClusters())
  {
    LogWarn("Compression unit {} exceeds DataRun bounds", unitIndex);
    return nullptr;
  }

  const ULONGLONG unitClusters = UnitClusters(unitFirstVcn);
  const ULONGLONG unitSize = unitClusters * this->GetClusterSize();

  // Bounded by kMaxCompressionUnitSize (constructor-validated), so this
  // can't be driven arbitrarily large.
  std::vector<BYTE> unit;
  try
  {
    unit.assign(static_cast<size_t>(unitSize), 0);
  }
  catch (const std::exception& e)
  {
    LogError("Cannot allocate compression unit {}", unitIndex);
    LogException(e);
    return nullptr;
  }

  const std::optional<ULONGLONG> realClustersOpt =
      LeadingRealClusters(unitFirstVcn, unitClusters);
  if (!realClustersOpt)
  {
    // LeadingRealClusters() already traced which layout it rejected.
    return nullptr;
  }
  const ULONGLONG realClusters = *realClustersOpt;

  if (realClusters == 0)
  {
    LogDebug("Compression unit {} is sparse", unitIndex);
  }
  else if (realClusters == unitClusters)
  {
    // Stored unit: raw, uncompressed bytes.
    const std::optional<ULONGLONG> len =
        ReadVirtualClustersRaw(unitFirstVcn, unitClusters, unit);
    if (!len || *len != unitSize)
    {
      LogError("Cannot read stored compression unit {}", unitIndex);
      return nullptr;
    }
  }
  else
  {
    // Compressed unit.
#ifndef NTFS_BROWSER_ENABLE_DECOMPRESSION
    // Unreachable: the constructor already rejects a compressed attribute
    // when decompression is not compiled in. Kept so this still compiles.
    LogError("Decompression is not compiled in.");
    return nullptr;
#else
    std::vector<BYTE> compressed;
    try
    {
      compressed.assign(
          static_cast<size_t>(realClusters * this->GetClusterSize()), 0);
    }
    catch (const std::exception& e)
    {
      LogError("Cannot allocate compressed data of unit {}", unitIndex);
      LogException(e);
      return nullptr;
    }

    const std::optional<ULONGLONG> len =
        ReadVirtualClustersRaw(unitFirstVcn, realClusters, compressed);
    if (!len || *len != compressed.size())
    {
      LogError("Cannot read compressed compression unit {}", unitIndex);
      return nullptr;
    }

    // requiredSize: expected output length - the trailing unit may compress
    // short of unitSize, else short output is zero-padded as if valid.
    const ULONGLONG unitFirstByte = unitFirstVcn * this->GetClusterSize();
    ULONGLONG requiredSize = 0;
    if (attr_header_nr_.real_size > unitFirstByte)
    {
      const ULONGLONG left = attr_header_nr_.real_size - unitFirstByte;
      requiredSize = (left < unitSize) ? left : unitSize;
    }

    try
    {
      const size_t produced = Lznt1::Decompress(compressed, unit);
      LogDebug("Decompressed compression unit {} into {} bytes", unitIndex,
               static_cast<ULONGLONG>(produced));
      if (produced < requiredSize)
      {
        LogWarn(
            "Compression unit {} decompressed to {} bytes, expected at "
            "least {}",
            unitIndex, static_cast<ULONGLONG>(produced), requiredSize);
        return nullptr;
      }
    }
    catch (const std::exception& e)
    {
      LogError("Cannot decompress compression unit {}", unitIndex);
      LogException(e);
      return nullptr;
    }
#endif
  }

  if constexpr (S == Strategy::NO_CACHE)
  {
    // Evicting here still holds "decompressed at most once per call": unit
    // indices only increase within a call.
    comp_unit_cache_.clear();
  }

  // Guarded like other allocations here: a bad_alloc must not escape
  // ReadData() into consumer code.
  try
  {
    return &comp_unit_cache_.emplace(unitIndex, std::move(unit)).first->second;
  }
  catch (const std::exception& e)
  {
    LogError("Cannot cache compression unit {}", unitIndex);
    LogException(e);
    return nullptr;
  }
}

// Compressed counterpart of ReadVirtualClustersRaw() below: serves the
// requested virtual clusters out of whole compression units instead of
// straight off the data runs.
template <Strategy S>
std::optional<ULONGLONG> AttrNonResident<S>::ReadVirtualClustersCompressed(
    ULONGLONG vcn, ULONGLONG clusters, std::span<BYTE> buffer) const
{
  assert(comp_unit_clusters_);

  // Same two bounds checks as the raw path.
  if (vcn + clusters > TotalClusters())
  {
    LogWarn("Cluster exceeds DataRun bounds");
    return {};
  }
  if (buffer.size() != clusters * this->GetClusterSize())
  {
    LogWarn("Invalid buffer size");
    return {};
  }

  BYTE* buf = buffer.data();
  ULONGLONG actural = 0;

  while (clusters != 0)
  {
    const ULONGLONG unitIndex = vcn / comp_unit_clusters_;
    const ULONGLONG unitFirstVcn = unitIndex * comp_unit_clusters_;

    const std::vector<BYTE>* unit = GetCompressionUnit(unitIndex);
    if (unit == nullptr)
    {
      break;
    }

    const ULONGLONG offsetInUnit = vcn - unitFirstVcn;
    const ULONGLONG unitClusters = unit->size() / this->GetClusterSize();
    if (offsetInUnit >= unitClusters)
    {
      LogWarn("Compression unit {} is shorter than expected", unitIndex);
      break;
    }

    const ULONGLONG available = unitClusters - offsetInUnit;
    const ULONGLONG toCopy = (clusters < available) ? clusters : available;
    const ULONGLONG bytes = toCopy * this->GetClusterSize();

    memcpy(buf, unit->data() + offsetInUnit * this->GetClusterSize(),
           static_cast<size_t>(bytes));

    buf += bytes;
    clusters -= toCopy;
    actural += toCopy;
    vcn += toCopy;
  }

  actural *= this->GetClusterSize();
  return actural;
}

// Dispatches to the compressed read path for a compressed attribute
// (comp_unit_size != 0), else the raw data-run walk.
template <Strategy S>
std::optional<ULONGLONG>
    AttrNonResident<S>::ReadVirtualClusters(ULONGLONG vcn, ULONGLONG clusters,
                                            std::span<BYTE> buffer) const
{
  assert(clusters);

  if (comp_unit_clusters_ != 0)
  {
    return ReadVirtualClustersCompressed(vcn, clusters, buffer);
  }

  return ReadVirtualClustersRaw(vcn, clusters, buffer);
}

// Uncompressed read path: walk the data runs, reading real clusters off disk
// and zero-filling sparse ones. Also the primitive the compressed path reads
// a unit's real (LZNT1 or stored) clusters through.
template <Strategy S>
std::optional<ULONGLONG> AttrNonResident<S>::ReadVirtualClustersRaw(
    ULONGLONG vcn, ULONGLONG clusters, std::span<BYTE> buffer) const
{
  assert(clusters);

  ULONGLONG actural = 0;

  // Verify if clusters exceeds DataRun bounds
  if (vcn + clusters > TotalClusters())
  {
    LogWarn("Cluster exceeds DataRun bounds");
    return {};
  }

  // Verify if clusters exceeds DataRun bounds
  if (buffer.size() != clusters * this->GetClusterSize())
  {
    LogWarn("Invalid buffer size");
    return {};
  }

  BYTE* buf = buffer.data();

  // Traverse the DataRun List to find the according LCN
  for (const Data::RunEntry& dr : data_run_list_)
  {
    if (vcn >= dr.start_vcn && vcn <= dr.last_vcn)
    {
      // Clusters from read pointer to the end
      const ULONGLONG vcns = dr.last_vcn - vcn + 1;
      // Fragmented data, we must go on
      const ULONGLONG clustersToRead = clusters > vcns ? vcns : clusters;

      if (dr.lcn)
      {
        std::optional<std::span<const BYTE>> bufferi =
            ReadClusters(clustersToRead, *dr.lcn, vcn - dr.start_vcn);
        if (!bufferi)
        {
          break;
        }
        memcpy(buf, bufferi->data(), bufferi->size());

#if defined(NTFS_BROWSER_ENABLE_EFS_CRYPTOPP) || \
    (defined(_WIN32) && defined(NTFS_BROWSER_ENABLE_EFS_BCRYPT))
        // Decrypts the copy in buf, never the span, which may be the cache.
        if (efs_context_ && !efs_context_->Decrypt(vcn * this->GetClusterSize(),
                                                   {buf, bufferi->size()}))
        {
          return {};
        }
#endif
      }
      else
      {
        memset(buf, 0, clustersToRead * this->GetClusterSize());
      }

      buf += static_cast<ULONGLONG>(clustersToRead) * this->GetClusterSize();
      clusters -= clustersToRead;
      actural += clustersToRead;
      vcn += clustersToRead;

      if (clusters == 0)
      {
        break;
      }
    }
  }

  actural *= this->GetClusterSize();
  return actural;
}

template <Strategy S>
void AttrNonResident<S>::SetEfsContext(
    std::shared_ptr<const Efs::Context> context) noexcept
{
  efs_context_ = std::move(context);
}

template <Strategy S>
const BYTE* AttrNonResident<S>::GetData() const noexcept
{
  return reinterpret_cast<const BYTE*>(&attr_header_nr_);
}

// Return Actural Data Size
// *allocSize = Allocated Size
// not no except
template <Strategy S>
ULONGLONG AttrNonResident<S>::GetDataSize() const noexcept
{
  return attr_header_nr_.real_size;
}

// Read "bufLen" bytes from "offset" into "bufv", bounded by "limit" total
// bytes. Number of bytes acturally read is returned in "*actural"
template <Strategy S>
std::optional<ULONGLONG> AttrNonResident<S>::ReadDataBounded(
    ULONGLONG offset, const std::span<BYTE>& buffer, ULONGLONG limit) const
{
  // Hard disks can only be accessed by sectors
  // To be simple and efficient, only implemented cluster based accessing
  // So cluster unaligned data address should be processed carefully here

  // NO_CACHE keeps no state across calls (its raw cluster reads are redone
  // every time too), so any decompressed unit left over from a previous
  // ReadData() is dropped here. Within this call the cache still stands, so
  // the up-to-3 partial/aligned ReadVirtualClusters() calls below share one
  // decompression per unit they overlap. FULL_CACHE keeps its units for the
  // attribute's whole lifetime instead - see comp_unit_cache_'s declaration.
  if constexpr (S == Strategy::NO_CACHE)
  {
    comp_unit_cache_.clear();
  }

  ULONGLONG bufLen = buffer.size();
  BYTE* buf = buffer.data();

  ULONGLONG actural = 0;
  if (bufLen == 0)
  {
    return actural;
  }

  // Bounds check
  if (offset > limit)
  {
    return {};
  }
  if (offset + bufLen > limit)
  {
    bufLen = gsl::narrow<DWORD>(limit - offset);
  }

  // First cluster Number
  ULONGLONG start_vcn = offset / this->GetClusterSize();
  // Bytes in first cluster
  const auto start_bytes = gsl::narrow<DWORD>(
      this->GetClusterSize() - (offset % this->GetClusterSize()));
  // Read first cluster
  if (start_bytes != this->GetClusterSize())
  {
    ULONGLONG len = 0;
    // First cluster, Unaligned
    std::span<BYTE> unaligned_buf_first = this->volume_.GetClusterBuffer();
    std::optional<ULONGLONG> lenc =
        ReadVirtualClusters(start_vcn, 1, unaligned_buf_first);
    if (!lenc || *lenc != this->GetClusterSize())
    {
      return {};
    }

    len = (start_bytes < bufLen) ? start_bytes : bufLen;
    memcpy(buf, &unaligned_buf_first[this->GetClusterSize() - start_bytes],
           len);
    buf += len;
    bufLen -= len;
    actural += len;
    start_vcn++;
  }
  if (bufLen == 0)
  {
    return actural;
  }

  const ULONGLONG alignedClusters = bufLen / this->GetClusterSize();
  if (alignedClusters != 0)
  {
    // Aligned clusters
    ULONGLONG alignedSize = alignedClusters * this->GetClusterSize();

    std::optional<ULONGLONG> lenc =
        ReadVirtualClusters(start_vcn, alignedClusters, {buf, alignedSize});
    if (!lenc || *lenc != alignedSize)
    {
      return {};
    }

    start_vcn += alignedClusters;
    buf += alignedSize;
    bufLen %= this->GetClusterSize();
    actural += *lenc;

    if (bufLen == 0)
    {
      return actural;
    }
  }

  // Last cluster, Unaligned
  std::span<BYTE> unaligned_buf_last = this->volume_.GetClusterBuffer();
  std::optional<ULONGLONG> lenc =
      ReadVirtualClusters(start_vcn, 1, unaligned_buf_last);
  if (!lenc || *lenc != this->GetClusterSize())
  {
    return {};
  }

  memcpy(buf, unaligned_buf_last.data(), bufLen);
  actural += bufLen;

  return actural;
}

// real_size is 0 on continuation instances; only start_vcn == 0 sets it.
template <Strategy S>
std::optional<ULONGLONG>
    AttrNonResident<S>::ReadData(ULONGLONG offset,
                                 const std::span<BYTE>& buffer) const
{
  return ReadDataBounded(offset, buffer, attr_header_nr_.real_size);
}

template <Strategy S>
std::optional<ULONGLONG>
    AttrNonResident<S>::ReadExtentData(ULONGLONG offset,
                                       const std::span<BYTE>& buffer) const
{
  const ULONGLONG clusters = TotalClusters();
  const DWORD clusterSize = this->GetClusterSize();

  // clusters/clusterSize are untrusted; guard the multiply against overflow.
  if (clusterSize != 0 &&
      clusters > (std::numeric_limits<ULONGLONG>::max)() / clusterSize)
  {
    LogError("Extent size overflows: {} clusters of {} bytes", clusters,
             clusterSize);
    return {};
  }

  return ReadDataBounded(offset, buffer, clusters * clusterSize);
}

template <Strategy S>
ULONGLONG AttrNonResident<S>::GetStartVcn() const noexcept
{
  return attr_header_nr_.start_vcn;
}

template <Strategy S>
ULONGLONG AttrNonResident<S>::GetLastVcn() const noexcept
{
  return attr_header_nr_.last_vcn;
}

template class AttrNonResident<Strategy::NO_CACHE>;
template class AttrNonResident<Strategy::FULL_CACHE>;

}  // namespace NtfsBrowser