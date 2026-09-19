#include <algorithm>
#include <cstring>

#include <ntfs-browser/attr-base.h>
#include <ntfs-browser/data/file-record-header.h>
#include <ntfs-browser/mask.h>
#include <ntfs-browser/mft-idx.h>
#include <ntfs-browser/ntfs-volume.h>

#include "attr-non-resident.h"
#include "attr-vol-info.h"
#include "attr-vol-name.h"
#include "data/index-block.h"
#include "data/ntfs-bpb.h"
#include "ntfs-common.h"

namespace NtfsBrowser
{

#ifdef _WIN32
template <Strategy S>
NtfsVolume<S>::NtfsVolume(_TCHAR volume) : mft_record_(*this)
{
  ClearAttrRawCB();

  if (OpenVolume(volume))
  {
    Init();
  }
}

template <Strategy S>
NtfsVolume<S>::NtfsVolume(std::wstring_view path) : mft_record_(*this)
{
  ClearAttrRawCB();

  if (OpenVolume(path))
  {
    Init();
  }
}
#endif

template <Strategy S>
NtfsVolume<S>::NtfsVolume(std::unique_ptr<IDiskReader> reader)
    : mft_record_(*this)
{
  ClearAttrRawCB();

  if (OpenVolume(std::move(reader)))
  {
    Init();
  }
}

// Verify NTFS volume version (must >= 3.0) and locate $MFT's Data attribute
template <Strategy S>
void NtfsVolume<S>::Init()
{
  FileRecord vol(*this);
  vol.SetAttrMask(Mask::VOLUME_NAME | Mask::VOLUME_INFORMATION);
  if (!vol.ParseFileRecord(static_cast<DWORD>(Enum::MftIdx::VOLUME)))
  {
    return;
  }

  if (!vol.ParseAttrs())
  {
    return;
  }
  const auto& vec = vol.getAttr(AttrType::VOLUME_INFORMATION);
  if (vec.empty())
  {
    return;
  }

  if constexpr (S == Strategy::NO_CACHE)
  {
    std::tie(version_major_, version_minor_) =
        reinterpret_cast<
            const AttrVolInfo<AttrResidentNoCache, Strategy::NO_CACHE>*>(
            vec.front().get())
            ->GetVersion();
  }
  else
  {
    std::tie(version_major_, version_minor_) =
        reinterpret_cast<
            const AttrVolInfo<AttrResidentFullCache, Strategy::FULL_CACHE>*>(
            vec.front().get())
            ->GetVersion();
  }
  NTFS_TRACE2("NTFS volume version: %u.%u\n", version_major_, version_minor_);
  if (version_major_ < 3)  // NT4 ?
  {
    return;
  }

#ifdef _DEBUG
  const auto& vec2 = vol.getAttr(AttrType::VOLUME_NAME);
  if (!vec2.empty())
  {
    if (S == Strategy::NO_CACHE)
    {
      const std::wstring_view volname =
          reinterpret_cast<
              const AttrVolName<AttrResidentNoCache, Strategy::NO_CACHE>*>(
              vec2.front().get())
              ->GetName();
      NTFS_TRACE1("NTFS volume name: %ls\n", volname.data());
    }
    else
    {
      const std::wstring_view volname =
          reinterpret_cast<
              const AttrVolName<AttrResidentFullCache, Strategy::FULL_CACHE>*>(
              vec2.front().get())
              ->GetName();
      NTFS_TRACE1("NTFS volume name: %ls\n", volname.data());
    }
  }
#endif

  volume_ok_ = true;

  // Step 1: Parse $MFT with DATA only to get the base record's non-resident DATA attr
  mft_record_.SetAttrMaskNoAttrList(Mask::DATA);
  if (!mft_record_.ParseFileRecord(static_cast<DWORD>(Enum::MftIdx::MFT)) ||
      !mft_record_.ParseAttrs())
  {
    return;
  }

  // Save base DATA attr and its runs (will be destroyed by re-parse below)
  const auto& base_data_attrs = mft_record_.getAttr(AttrType::DATA);
  ULONGLONG base_real_size = 0;
  ULONGLONG base_attr_start_vcn = 0;
  if (!base_data_attrs.empty() && base_data_attrs.front()->IsNonResident())
  {
    const auto* base_non_res =
        static_cast<const AttrNonResident<S>*>(base_data_attrs.front().get());
    base_attr_start_vcn = base_non_res->GetStartVcn();
    base_real_size = base_non_res->GetDataSize();
    mft_data_ = base_data_attrs.front().get();

    // Save base runs into mft_runs_
    const auto& base_runs = base_non_res->GetRuns();
    for (const auto& run : base_runs)
    {
      MftRun mft_run;
      mft_run.start_vcn = base_attr_start_vcn + run.start_vcn;
      mft_run.clusters = run.clusters;
      mft_run.lcn = run.lcn;
      mft_runs_.push_back(mft_run);
    }
  }

  // Step 2: Iteratively re-parse with ATTRIBUTE_LIST to get extension records' DATA attrs
  // Each iteration, the expanded run list allows reading more extension records,
  // which in turn provide more runs. Repeat until no new runs are found.
  for (int iteration = 0; iteration < 20; iteration++)
  {
    // Reset attr_list_chain_ so AttrList can retry previously-failed extension records
    mft_record_.ResetAttrListChain();

    mft_record_.SetAttrMaskNoAttrList(Mask::DATA | Mask::ATTRIBUTE_LIST);
    if (!mft_record_.ParseFileRecord(static_cast<DWORD>(Enum::MftIdx::MFT)) ||
        !mft_record_.ParseAttrs())
    {
      // ATTRIBUTE_LIST parsing failed, but we have base runs so continue
    }

    // Add extension records' DATA attrs to mft_runs_
    const ULONGLONG prev_run_count = mft_runs_.size();
    const auto& all_data_attrs2 = mft_record_.getAttr(AttrType::DATA);
    for (const auto& attr : all_data_attrs2)
    {
      if (!attr->IsNonResident())
      {
        continue;
      }

      const auto* non_res =
          static_cast<const AttrNonResident<S>*>(attr.get());
      const ULONGLONG attr_start_vcn = non_res->GetStartVcn();
      const ULONGLONG attr_size = non_res->GetDataSize();

      // Skip if this is the base record's DATA attr (same start_vcn and size)
      if (attr_start_vcn == base_attr_start_vcn && attr_size == base_real_size)
      {
        continue;
      }

      const auto& runs = non_res->GetRuns();
      for (const auto& run : runs)
      {
        MftRun mft_run;
        mft_run.start_vcn = attr_start_vcn + run.start_vcn;
        mft_run.clusters = run.clusters;
        mft_run.lcn = run.lcn;
        mft_runs_.push_back(mft_run);
      }
    }

    // Sort by start_vcn
    std::sort(mft_runs_.begin(), mft_runs_.end(),
              [](const MftRun& a, const MftRun& b)
              { return a.start_vcn < b.start_vcn; });

    // Deduplicate runs (same start_vcn = same logical run, keep one)
    auto last = std::unique(mft_runs_.begin(), mft_runs_.end(),
                            [](const MftRun& a, const MftRun& b)
                            { return a.start_vcn == b.start_vcn; });
    mft_runs_.erase(last, mft_runs_.end());

    ValidateMftRuns();

    if (mft_runs_.size() == prev_run_count)
    {
      NTFS_TRACE1("MFT run list stable after %d iterations\n", iteration + 1);
      break;
    }
  }
}

// Validate the combined MFT run list (check for overlaps and gaps)
template <Strategy S>
void NtfsVolume<S>::ValidateMftRuns()
{
  // Trim overlapping runs: if prev extends past curr's start, shorten prev
  for (size_t i = 1; i < mft_runs_.size(); i++)
  {
    auto& prev = mft_runs_[i - 1];
    const auto& curr = mft_runs_[i];
    const ULONGLONG prev_end = prev.start_vcn + prev.clusters;
    if (prev_end > curr.start_vcn)
    {
      prev.clusters = curr.start_vcn - prev.start_vcn;
    }
  }
}

// Read clusters from the unified MFT run list
// vcn: Starting VCN to read from
// clusters: Number of clusters to read
// buffer: Destination buffer (must be large enough for clusters * cluster_size)
// Returns: Number of bytes read, or empty on failure
template <Strategy S>
std::optional<ULONGLONG>
    NtfsVolume<S>::ReadMftData(ULONGLONG vcn, ULONGLONG clusters,
                               std::span<BYTE> buffer) const
{
  if (mft_runs_.empty())
  {
    return {};
  }

  ULONGLONG remaining = clusters;
  ULONGLONG vcn_current = vcn;
  BYTE* buf = buffer.data();
  ULONGLONG total_read = 0;

  // Find the run containing the starting VCN
  for (const auto& run : mft_runs_)
  {
    // Skip runs that end before our target VCN
    if (vcn_current >= run.start_vcn + run.clusters)
    {
      continue;
    }

    // Check if VCN is within this run
    if (vcn_current >= run.start_vcn)
    {
      ULONGLONG offset_in_run = vcn_current - run.start_vcn;
      ULONGLONG available = run.clusters - offset_in_run;
      ULONGLONG to_read = (remaining < available) ? remaining : available;

      if (run.lcn)
      {
        // Non-sparse: read from disk
        LARGE_INTEGER addr;
        addr.QuadPart = static_cast<LONGLONG>(
            (*run.lcn + offset_in_run) * GetClusterSize());

        DWORD bytesToRead = static_cast<DWORD>(to_read * GetClusterSize());
        auto data = volume_.Read(addr, bytesToRead);
        if (!data)
        {
          NTFS_TRACE1("ReadMftData: Failed to read from LCN %I64d\n",
                      *run.lcn + offset_in_run);
          return {};
        }
        memcpy(buf, data->data(), data->size());
        total_read += data->size();
      }
      else
      {
        // Sparse: zero-fill
        ULONGLONG bytes = to_read * GetClusterSize();
        memset(buf, 0, bytes);
        total_read += bytes;
      }

      buf += to_read * GetClusterSize();
      remaining -= to_read;
      vcn_current += to_read;

      if (remaining == 0)
      {
        return total_read;
      }
    }
  }

  // If we get here, we ran out of runs before reading all requested clusters
  if (remaining == clusters)
  {
    // VCN not found in any run
    return {};
  }

  // Partial read: return what we managed to read
  return total_read;
}

#ifdef _WIN32
// Open a volume ('a' - 'z', 'A' - 'Z'), get volume handle and BPB
template <Strategy S>
bool NtfsVolume<S>::OpenVolume(_TCHAR volume)
{
  // Verify parameter
  if (!_istalpha(volume))
  {
    NTFS_TRACE("Volume name error, should be like 'C', 'D'\n");
    return false;
  }

  std::array<_TCHAR, 7> volumePath;
  _sntprintf_s(volumePath.data(), 7, 6, _T("\\\\.\\%c:"), volume);
  volumePath[6] = _T('\0');

  return OpenVolume(std::wstring_view(volumePath.data()));
}

// Open an arbitrary device/image path, get volume handle and BPB
template <Strategy S>
bool NtfsVolume<S>::OpenVolume(std::wstring_view path)
{
  if (!volume_.Open(path))
  {
    NTFS_TRACE("Cannnot open volume\n");
    return false;
  }

  return ParseBootSector();
}
#endif

// Use an already-open reader (eg. a test double), get BPB
template <Strategy S>
bool NtfsVolume<S>::OpenVolume(std::unique_ptr<IDiskReader> reader)
{
  volume_ = FileReader<S>(std::move(reader));

  return ParseBootSector();
}

// Read the first sector (boot sector) and derive volume geometry from it
template <Strategy S>
bool NtfsVolume<S>::ParseBootSector()
{
  constexpr DWORD default_sector_size = 512;
  LARGE_INTEGER frAddr{.QuadPart = 0};
  std::optional<std::span<const BYTE>> bpb_buffer =
      volume_.Read(frAddr, default_sector_size);
  if (!bpb_buffer)
  {
    NTFS_TRACE("Read boot sector error\n");
    return false;
  }
  auto bpb = reinterpret_cast<const Data::NtfsBpb*>(bpb_buffer->data());

  if (strncmp(reinterpret_cast<const char*>(&bpb->signature[0]), NTFS_SIGNATURE,
              sizeof(bpb->signature)) != 0)
  {
    NTFS_TRACE("Volume file system is not NTFS\n");
    return false;
  }

  // Log important volume parameters

  sector_size_ = bpb->bytes_per_sector;
  NTFS_TRACE1("Sector Size = %u bytes\n", sector_size_);

  // Sector size must be large enough to hold one Update Sequence Number
  // (WORD). Fixup patching (PatchUS) computes (sector_size / 2) - 1; a
  // sector_size of 0 or 1 from a corrupted/malicious boot sector underflows
  // that unsigned arithmetic and walks the patch pointer outside the file
  // record/index block buffer, corrupting adjacent heap memory.
  if (sector_size_ < sizeof(WORD))
  {
    NTFS_TRACE("Sector Size must be at least 2 bytes\n");
    return false;
  }

  cluster_size_ = sector_size_ * bpb->sectors_per_cluster;
  NTFS_TRACE1("Cluster Size = %u bytes\n", cluster_size_);

  if (cluster_size_ == 0)
  {
    NTFS_TRACE("Cluster Size can't be null\n");
    return false;
  }
  cluster_buffer_.resize(cluster_size_);

  char sz = static_cast<char>(bpb->clusters_per_file_record);
  if (sz > 0)
  {
    file_record_size_ = cluster_size_ * sz;
  }
  else
  {
    file_record_size_ = 1U << static_cast<unsigned char>(-sz);
  }
  NTFS_TRACE1("FileRecord Size = %u bytes\n", file_record_size_);

  // clusters_per_file_record is an attacker-controlled signed-byte-style
  // field: a negative encoding close to 0 (eg. 0xFF -> sz = -1) yields a
  // file_record_size_ of just a few bytes. FileRecordHeader's ctor happens
  // to reject anything other than exactly 1024 bytes today
  // (src/data/file-record-header.cpp), which incidentally protects this
  // path, but that is a coincidence of its current buffer layout
  // (FileRecordHeader::Data::raw is a fixed BYTE[1024]), not a structural
  // guarantee - nothing stops that accidental guard from disappearing later.
  // Validate file_record_size_ here too, the same way as index_block_size_
  // below: it must be able to hold FileRecordHeader::Data, and every sector
  // in it must be addressable by PatchUS().
  if (file_record_size_ < sizeof(FileRecordHeader::Data) ||
      file_record_size_ % sector_size_ != 0)
  {
    NTFS_TRACE("FileRecord Size is invalid\n");
    return false;
  }

  sz = static_cast<char>(bpb->clusters_per_index_block);
  if (sz > 0)
  {
    index_block_size_ = cluster_size_ * sz;
  }
  else
  {
    index_block_size_ = 1U << static_cast<unsigned char>(-sz);
  }
  NTFS_TRACE1("IndexBlock Size = %u bytes\n", index_block_size_);

  // Same reasoning as file_record_size_ just above, but here nothing
  // downstream accidentally protects it: AttrIndexAlloc<S>::ParseIndexBlock()
  // (src/attr-index-alloc.cpp) allocates exactly index_block_size_ bytes and
  // immediately reinterprets the start of that buffer as Data::IndexBlock
  // (magic, offset_of_us, ...) without ever checking the allocation is big
  // enough to hold it. clusters_per_index_block = 0xFF ("sz = -1") yields
  // index_block_size_ = 2, so even ibBuf->magic alone (a DWORD) already reads
  // past a 2-byte allocation. Also require a whole number of sectors, like
  // file_record_size_ above, so the Update Sequence Array fixup
  // (AttrIndexAlloc<S>::PatchUS) walks a well-defined number of sectors
  // instead of running off a partial one.
  if (index_block_size_ < sizeof(Data::IndexBlock) ||
      index_block_size_ % sector_size_ != 0)
  {
    NTFS_TRACE("IndexBlock Size is invalid\n");
    return false;
  }

  mft_addr_ = bpb->lcn_mft * cluster_size_;
  NTFS_TRACE1("MFT address = 0x%016I64X\n", mft_addr_);

  return true;
}

// Check if Volume is successfully opened
template <Strategy S>
bool NtfsVolume<S>::IsVolumeOK() const noexcept
{
  return volume_ok_;
}

// Get NTFS volume version
template <Strategy S>
std::pair<BYTE, BYTE> NtfsVolume<S>::GetVersion() const noexcept
{
  return {version_major_, version_minor_};
}

// Get File Record count
template <Strategy S>
ULONGLONG NtfsVolume<S>::GetRecordsCount() const noexcept
{
  return (mft_data_->GetDataSize() / file_record_size_);
}

// Get BPB information
template <Strategy S>
WORD NtfsVolume<S>::GetSectorSize() const noexcept
{
  return sector_size_;
}

template <Strategy S>
DWORD NtfsVolume<S>::GetClusterSize() const noexcept
{
  return cluster_size_;
}

template <Strategy S>
DWORD NtfsVolume<S>::GetFileRecordSize() const noexcept
{
  return file_record_size_;
}

template <Strategy S>
DWORD NtfsVolume<S>::GetIndexBlockSize() const noexcept
{
  return index_block_size_;
}

// Get MFT starting address
template <Strategy S>
ULONGLONG NtfsVolume<S>::GetMFTAddr() const noexcept
{
  return mft_addr_;
}

template <Strategy S>
std::span<BYTE> NtfsVolume<S>::GetClusterBuffer() const noexcept
{
  return {cluster_buffer_.data(), cluster_buffer_.size()};
}

template <Strategy S>
std::optional<std::span<const BYTE>> NtfsVolume<S>::Read(LARGE_INTEGER& addr,
                                                         DWORD length) const
{
  return volume_.Read(addr, length);
}

template <Strategy S>
bool NtfsVolume<S>::ReadInto(LARGE_INTEGER& addr, std::span<BYTE> dest) const
{
  return volume_.ReadInto(addr, dest);
}

// Install Attribute CallBack routines for the whole Volume
template <Strategy S>
bool NtfsVolume<S>::InstallAttrRawCB(AttrType attrType,
                                     AttrRawCallback cb) noexcept
{
  const DWORD atIdx = ATTR_INDEX(attrType);
  if (atIdx >= kAttrNums)
  {
    return false;
  }

  attr_raw_call_back_[atIdx] = cb;
  return true;
}

template <Strategy S>
void NtfsVolume<S>::AttrRawCallBack(DWORD attType, const AttrHeaderCommon& ahc,
                                    bool& bDiscard) const
{
  if (attr_raw_call_back_[attType] != nullptr)
  {
    attr_raw_call_back_[attType](ahc, bDiscard);
  }
}

// Clear all Attribute CallBack routines
template <Strategy S>
void NtfsVolume<S>::ClearAttrRawCB() noexcept
{
  for (AttrRawCallback& call_back : attr_raw_call_back_)
  {
    call_back = nullptr;
  }
}

template class NtfsVolume<Strategy::NO_CACHE>;
template class NtfsVolume<Strategy::FULL_CACHE>;

}  // namespace NtfsBrowser
