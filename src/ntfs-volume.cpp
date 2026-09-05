#include <cstring>
#include <limits>

#include <ntfs-browser/attr-base.h>
#include <ntfs-browser/data/file-record-header.h>
#include <ntfs-browser/mask.h>
#include <ntfs-browser/mft-idx.h>
#include <ntfs-browser/ntfs-volume.h>

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

  mft_record_.SetAttrMask(Mask::DATA);
  if (!mft_record_.ParseFileRecord(static_cast<DWORD>(Enum::MftIdx::MFT)) ||
      !mft_record_.ParseAttrs())
  {
    return;
  }

  const std::vector<std::unique_ptr<AttrBase<S>>>& vec3 =
      mft_record_.getAttr(AttrType::DATA);
  if (vec3.empty())
  {
    return;
  }

  mft_data_ = vec3.front().get();

  // Only report the volume as usable once mft_data_ is actually assigned.
  // This used to be set true right after $Volume was parsed, before $MFT's
  // own file record was even read - if ParseFileRecord()/ParseAttrs() failed
  // above, or $MFT's DATA attribute vector came back empty, this function
  // would already have returned with volume_ok_ true and mft_data_ still
  // null. Every caller following the documented "construct, then check
  // IsVolumeOK()" contract (eg. NTFSLibTests/ntfsundel/ntfsundelDlg.cpp)
  // would then have GetRecordsCount() dereference that null mft_data_. See
  // F14 in docs/bug-reports/2026-09-03-full-repo.md.
  volume_ok_ = true;
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

  // clusters_per_file_record is fully attacker-controlled: for sz <= 0,
  // file_record_size_ is about to be computed as
  // 1U << static_cast<unsigned char>(-sz). -sz can be as large as 128 (sz ==
  // -128, from a byte of 0x80), and shifting a 32-bit 1U by >= 32 is
  // undefined behaviour - this must be rejected before that shift ever runs,
  // not after (unlike the size checks below, inherited from F6, which look
  // at the *result* and only catch it being implausibly small). Bounding sz
  // also closes a second gap those checks miss entirely: sz == -31 (byte
  // 0xE1) shifts a well-defined amount (31 < 32) to a well-defined but
  // absurd 0x80000000 (2 GiB) that is still >= sizeof(FileRecordHeader::Data)
  // and a multiple of every common sector size, so it sails through both
  // checks below unrejected and only blows up as a ~2 GiB allocation once
  // FileRecord<S>::ReadFileRecord() resizes its buffer to it. Cap the
  // magnitude at the same bound on both sides: sz > 8 would already make
  // file_record_size_ bigger than any real NTFS file record, and sz < -12
  // would shift past 4096 bytes, just as implausible. See F5 in
  // docs/bug-reports/2026-09-03-full-repo.md.
  if (sz < -12 || sz > 8)
  {
    NTFS_TRACE("clusters_per_file_record magnitude out of range\n");
    return false;
  }

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

  // Same reasoning and same bound as clusters_per_file_record just above -
  // reject an sz magnitude that would either shift 1U by >= 32 (undefined
  // behaviour) or produce a well-defined but absurd index_block_size_ (eg.
  // sz == -31, 1U << 31 == 0x80000000) that the size checks below cannot
  // catch, since they only look at the result. See F5 in
  // docs/bug-reports/2026-09-03-full-repo.md.
  if (sz < -12 || sz > 8)
  {
    NTFS_TRACE("clusters_per_index_block magnitude out of range\n");
    return false;
  }

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

  // lcn_mft is attacker-controlled and otherwise unbounded, and both it and
  // cluster_size_ are just as attacker-controlled as the sizes validated
  // above - multiplying them can itself silently overflow/wrap the
  // ULONGLONG mft_addr_ is stored in. Detect that first, rather than let a
  // wrapped, spuriously "small" result slip past the bounds check below.
  const bool mft_addr_overflows =
      cluster_size_ != 0 &&
      bpb->lcn_mft > (std::numeric_limits<ULONGLONG>::max)() / cluster_size_;
  mft_addr_ = mft_addr_overflows ? (std::numeric_limits<ULONGLONG>::max)()
                                 : bpb->lcn_mft * cluster_size_;
  NTFS_TRACE1("MFT address = 0x%016I64X\n", mft_addr_);

  // mft_addr_ is later added to a per-record byte offset and narrowed down
  // to a LONGLONG for LARGE_INTEGER::QuadPart (FileRecord::ReadFileRecord,
  // src/file-record.cpp). gsl::narrow<LONGLONG>() throws
  // gsl::narrowing_error when the value doesn't fit - and that type derives
  // from std::exception, not std::runtime_error, so left unvalidated it
  // slips past every catch in the parser and escapes
  // NtfsVolume::Init() -> the NtfsVolume constructor itself, the first time
  // $MFT is read (see F8 in docs/bug-reports/2026-09-03-full-repo.md).
  // Reject the volume outright, the same way the sizes above are: mft_addr_
  // must fit a LONGLONG with ample headroom left for that later per-record
  // offset. (bpb->total_sectors would be an equally natural bound - $MFT
  // cannot legitimately start beyond the volume it lives on - but it is not
  // otherwise validated or even required to be populated by every caller
  // constructing an NtfsVolume from an already-open IDiskReader, e.g. tests,
  // so it is not relied on here.)
  constexpr ULONGLONG kMaxPlausibleMftAddr =
      (std::numeric_limits<LONGLONG>::max)() / 2;

  if (mft_addr_overflows || mft_addr_ > kMaxPlausibleMftAddr)
  {
    NTFS_TRACE("MFT address is invalid\n");
    return false;
  }

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
  // mft_data_ is only ever assigned once Init() successfully parses $MFT's
  // own DATA attribute (see the F14 fix in Init() above), and IsVolumeOK()
  // now stays false until that happens - but this function is noexcept and
  // must not crash even if a caller reads it before/without checking
  // IsVolumeOK(), so guard it directly rather than relying solely on that
  // contract being honored. See F14 in
  // docs/bug-reports/2026-09-03-full-repo.md.
  if (mft_data_ == nullptr)
  {
    return 0;
  }

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
