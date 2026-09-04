#include <cassert>
#include <stdexcept>

#include <gsl/narrow>

#include <ntfs-browser/attr-base.h>
#include <ntfs-browser/data/attr-type.h>
#include <ntfs-browser/file-record.h>
#include <ntfs-browser/mask.h>
#include <ntfs-browser/mft-idx.h>
#include <ntfs-browser/ntfs-volume.h>
#include <ntfs-browser/data/file-record-header.h>
#include <ntfs-browser/flag/file-record.h>

#include "attr-bitmap.h"
#include "attr-data.h"
#include "attr-file-name.h"
#include "attr-index-alloc.h"
#include "attr-index-root.h"
#include "attr-list.h"
#include "attr-non-resident.h"
#include "attr-resident.h"
#include "attr-std-info.h"
#include "attr-vol-info.h"
#include "attr-vol-name.h"
#include "attr/header-non-resident.h"
#include "attr/header-resident.h"
#include "data/run-entry.h"
#include "index-block.h"

namespace NtfsBrowser
{

template <Strategy S>
FileRecord<S>::FileRecord(const NtfsVolume<S>& volume) : volume_(volume)
{
  ClearAttrRawCB();

  // Default to parse all attributes
}

template <Strategy S>
FileRecord<S>::~FileRecord()
{
}

template <Strategy S>
const NtfsVolume<S>& FileRecord<S>::GetVolume() const noexcept
{
  return volume_;
}

template <Strategy S>
void FileRecord<S>::ClearAttrs() noexcept
{
  for (std::vector<std::unique_ptr<AttrBase<S>>>& arr : attr_list_)
  {
    arr.clear();
  }
}

// Call user defined Callback routines for an attribute
template <Strategy S>
void FileRecord<S>::UserCallBack(DWORD attType, const AttrHeaderCommon& ahc,
                                 bool& bDiscard) noexcept
{
  bDiscard = false;

  if (attr_raw_call_back_[attType] != nullptr)
  {
    attr_raw_call_back_[attType](ahc, bDiscard);
  }
  else
  {
    volume_.AttrRawCallBack(attType, ahc, bDiscard);
  }
}

template <Strategy S>
template <typename RESIDENT>
std::unique_ptr<AttrBase<S>>
    FileRecord<S>::AllocAttr(const AttrHeaderCommon& ahc, bool& bUnhandled,
                             std::unordered_set<ULONGLONG>& attrListChain)
{
  switch (ahc.type)
  {
    // STANDARD_INFORMATION, FILE_NAME, VOLUME_NAME, VOLUME_INFORMATION and
    // INDEX_ROOT are always resident per the NTFS on-disk format; there is
    // no meaningful "non-resident" shape for any of them to fall back to
    // (GetData() on a non-resident attribute returns the raw
    // Attr::HeaderNonResident bytes, not attribute content fetched through
    // clusters). A record claiming otherwise is corrupt - reject it here,
    // before the RESIDENT ctor reinterprets bytes that (once total_size is
    // validated against ahc.non_resident's own minimum in ParseAttrs) are
    // guaranteed in-bounds but would otherwise describe the wrong on-disk
    // layout entirely.
    case AttrType::STANDARD_INFORMATION:
      if (ahc.non_resident != 0)
      {
        throw std::runtime_error(
            "Standard Information attribute must be resident.\n");
      }
      return std::make_unique<AttrStdInfo<RESIDENT, S>>(ahc, *this);

    case AttrType::ATTRIBUTE_LIST:
      if (ahc.non_resident != 0)
      {
        return std::make_unique<AttrList<AttrNonResident<S>, S>>(ahc, *this,
                                                                 attrListChain);
      }
      return std::make_unique<AttrList<RESIDENT, S>>(ahc, *this, attrListChain);

    case AttrType::FILE_NAME:
      if (ahc.non_resident != 0)
      {
        throw std::runtime_error("File Name attribute must be resident.\n");
      }
      return std::make_unique<AttrFileName<RESIDENT, S>>(ahc, *this);

    case AttrType::VOLUME_NAME:
      if (ahc.non_resident != 0)
      {
        throw std::runtime_error("Volume Name attribute must be resident.\n");
      }
      return std::make_unique<AttrVolName<RESIDENT, S>>(ahc, *this);

    case AttrType::VOLUME_INFORMATION:
      if (ahc.non_resident != 0)
      {
        throw std::runtime_error(
            "Volume Information attribute must be resident.\n");
      }
      return std::make_unique<AttrVolInfo<RESIDENT, S>>(ahc, *this);

    case AttrType::DATA:
      if (ahc.non_resident != 0)
      {
        return std::make_unique<AttrData<AttrNonResident<S>, S>>(ahc, *this);
      }
      return std::make_unique<AttrData<RESIDENT, S>>(ahc, *this);

    case AttrType::INDEX_ROOT:
      if (ahc.non_resident != 0)
      {
        throw std::runtime_error("Index Root attribute must be resident.\n");
      }
      return std::make_unique<AttrIndexRoot<RESIDENT, S>>(ahc, *this);

    // INDEX_ALLOCATION is always non-resident per the NTFS on-disk format;
    // AttrIndexAlloc unconditionally derives from AttrNonResident<S> (no
    // resident variant exists). A record claiming otherwise is corrupt -
    // reject it, rather than reinterpreting its bytes as
    // Attr::HeaderNonResident (64 bytes) when only a resident attribute's
    // minimum (24 bytes, per ParseAttrs' ahc.non_resident-based check) was
    // actually guaranteed.
    case AttrType::INDEX_ALLOCATION:
      if (ahc.non_resident == 0)
      {
        throw std::runtime_error(
            "Index Allocation attribute must be non-resident.\n");
      }
      return std::make_unique<AttrIndexAlloc<S>>(ahc, *this);

    case AttrType::BITMAP:
      if (ahc.non_resident != 0)
      {
        return std::make_unique<AttrBitmap<AttrNonResident<S>, S>>(ahc, *this);
      }
      // Resident Bitmap may exist in a directory's FileRecord
      // or in $MFT for a very small volume in theory
      return std::make_unique<AttrBitmap<RESIDENT, S>>(ahc, *this);

    // Unhandled Attributes
    default:
      bUnhandled = true;
      if (ahc.non_resident != 0)
      {
        return std::make_unique<AttrNonResident<S>>(ahc, *this);
      }
      return std::make_unique<RESIDENT>(ahc, *this);
  }
}

// Parse a single Attribute
// Return False on error
template <Strategy S>
bool FileRecord<S>::ParseAttr(const AttrHeaderCommon& ahc,
                              std::unordered_set<ULONGLONG>& attrListChain)
{
  const DWORD attrIndex = ATTR_INDEX(ahc.type);
  if (attrIndex >= kAttrNums)
  {
    NTFS_TRACE1("Invalid Attribute Type: 0x%04X\n", ahc.type);
    return false;
  }

  bool bDiscard = false;
  UserCallBack(attrIndex, ahc, bDiscard);

  if (bDiscard)
  {
    NTFS_TRACE1("User Callback has processed this Attribute: 0x%04X\n",
                ahc.type);
    return true;
  }

  bool bUnhandled = false;

  std::unique_ptr<AttrBase<S>> attr;
  try
  {
    if constexpr (S == Strategy::NO_CACHE)
      attr = AllocAttr<AttrResidentNoCache>(ahc, bUnhandled, attrListChain);
    else
      attr = AllocAttr<AttrResidentFullCache>(ahc, bUnhandled, attrListChain);
  }
  catch ([[maybe_unused]] const std::exception& e)
  {
    // Widened from std::runtime_error to std::exception (F8): AllocAttr()
    // (eg. its non-resident attribute subclasses' data-run parsing) can
    // reach gsl::narrow(), whose gsl::narrowing_error derives from
    // std::exception, not std::runtime_error - narrower catches let it
    // escape uncaught. See F8 in docs/bug-reports/2026-09-03-full-repo.md.
    NTFS_TRACE1("Attribute Parse error: 0x%04X\n", ahc.type);
    NTFS_TRACE(e.what());
    return false;
  }

  if (bUnhandled)
  {
    NTFS_TRACE1("Unhandled attribute: 0x%04X\n", ahc.type);
  }
  attr_list_[attrIndex].push_back(std::move(attr));
  return true;
}

// Read File Record
template <Strategy S>
std::optional<FileRecordHeaderImpl<S>>
    FileRecord<S>::ReadFileRecord(ULONGLONG fileRef)
{
  if (record_buffer_.size() != volume_.GetFileRecordSize())
  {
    record_buffer_.resize(volume_.GetFileRecordSize());
  }

  if (fileRef < static_cast<ULONGLONG>(Enum::MftIdx::USER) ||
      volume_.mft_data_ == nullptr)
  {
    // Take as continuous disk allocation
    LARGE_INTEGER frAddr;
    try
    {
      frAddr.QuadPart = gsl::narrow<LONGLONG>(
          volume_.GetMFTAddr() + (volume_.GetFileRecordSize()) * fileRef);
    }
    catch ([[maybe_unused]] const std::exception& e)
    {
      // F8 safety net: NtfsVolume::ParseBootSector() now rejects an
      // implausible mft_addr_ upfront, but fileRef itself is
      // attacker-controlled too (directory index entries,
      // $ATTRIBUTE_LIST base_ref, ...) and unbounded here, so the sum can
      // still overflow a LONGLONG. gsl::narrow<LONGLONG>() throws
      // gsl::narrowing_error, which derives from std::exception rather than
      // std::runtime_error - catch it right at the throw site and report it
      // through this function's normal std::optional error channel instead
      // of letting it escape FileRecord::ParseFileRecord() ->
      // NtfsVolume::Init() -> the NtfsVolume constructor. See F8 in
      // docs/bug-reports/2026-09-03-full-repo.md.
      NTFS_TRACE(e.what());
      return {};
    }

    if (!volume_.ReadInto(frAddr, record_buffer_))
    {
      return {};
    }

    try
    {
      return FileRecordHeader::Factory<S>(record_buffer_,
                                          volume_.GetSectorSize());
    }
    catch ([[maybe_unused]] const std::exception& e)
    {
      // Widened from std::runtime_error to std::exception (F8) - see the
      // comment on ParseAttr()'s catch clause above.
      NTFS_TRACE(e.what());
      return {};
    }
  }

  // May be fragmented $MFT
  const ULONGLONG frAddr = (volume_.GetFileRecordSize()) * fileRef;

  if (std::optional<ULONGLONG> len =
          volume_.mft_data_->ReadData(frAddr, record_buffer_);
      !len || *len != volume_.GetFileRecordSize())
  {
    return {};
  }

  try
  {
    return FileRecordHeader::Factory<S>(record_buffer_,
                                        volume_.GetSectorSize());
  }
  catch ([[maybe_unused]] const std::exception& e)
  {
    // This fragmented-$MFT path (fileRef >= Enum::MftIdx::USER) reaches the
    // very same FileRecordHeader::Factory<S>() call as the direct-allocation
    // path above, which does guard it - noticed while widening that catch
    // for F8, and fixed here too for the same reason: nothing should let an
    // exception escape FileRecord::ParseFileRecord() ->
    // NtfsVolume::Init() -> the NtfsVolume constructor.
    NTFS_TRACE(e.what());
    return {};
  }
}

// Read File Record, verify and patch the US (update sequence)
template <Strategy S>
bool FileRecord<S>::ParseFileRecord(ULONGLONG fileRef)
{
  // Clear previous data
  ClearAttrs();
  if (file_record_)
  {
    file_record_.reset();
  }

  std::optional<FileRecordHeaderImpl<S>> fr = ReadFileRecord(fileRef);
  if (!fr)
  {
    NTFS_TRACE1("Cannot read file record %I64u\n", fileRef);

    file_reference_ = {};

    return false;
  }

  file_reference_ = fileRef;

  if (fr->GetData()->magic != kFileRecordMagic)
  {
    NTFS_TRACE("Invalid file record\n");
    return false;
  }

  if (!fr->PatchUS())
  {
    NTFS_TRACE("Update Sequence Number error\n");
    return false;
  }

  NTFS_TRACE1("File Record %I64u Found\n", fileRef);
  file_record_ = std::move(fr);

  return true;
}

// Visit IndexBlocks recursivly to find a specific Filename
template <Strategy S>
std::optional<IndexEntry> FileRecord<S>::VisitIndexBlock(
    ULONGLONG vcn, std::wstring_view fileName,
    std::unordered_set<ULONGLONG>& visitedVcns) const
{
  // A subnode VCN already on this walk means the on-disk B+ tree is
  // malformed (self-loop or cycle) - stop instead of recursing forever.
  if (!visitedVcns.insert(vcn).second)
  {
    return {};
  }

  const std::vector<std::unique_ptr<AttrBase<S>>>& vec =
      getAttr(AttrType::INDEX_ALLOCATION);
  if (vec.empty())
  {
    return {};
  }

  IndexBlock ib;
  if (!static_cast<AttrIndexAlloc<S>*>(vec.front().get())
           ->ParseIndexBlock(vcn, ib))
  {
    return {};
  }

  for (const IndexEntry& ie : ib)
  {
    if (ie.HasName())
    {
      // Compare name
      const int i = ie.Compare(fileName);
      if (i == 0)
      {
        // Must be a copy. Either, will be invalid when ib is destroyed.
        return ie;
      }
      if (i < 0)  // fileName is smaller than IndexEntry
      {
        // Visit SubNode
        if (!ie.IsSubNodePtr())
        {
          return {};  // not found
        }
        // Search in SubNode (IndexBlock), recursive call
        std::optional<IndexEntry> retval =
            VisitIndexBlock(ie.GetSubNodeVCN(), fileName, visitedVcns);
        if (retval)
        {
          return retval;
        }
      }
      // Just step forward if fileName is bigger than IndexEntry
    }
    else if (ie.IsSubNodePtr())
    {
      // Search in SubNode (IndexBlock), recursive call
      std::optional<IndexEntry> retval =
          VisitIndexBlock(ie.GetSubNodeVCN(), fileName, visitedVcns);
      if (retval)
      {
        return retval;
      }
    }
  }

  return {};
}

// Traverse SubNode recursivly in ascending order
// Call user defined callback routine once found an subentry
template <Strategy S>
void FileRecord<S>::TraverseSubNode(
    ULONGLONG vcn, SUBENTRY_CALLBACK seCallBack, void* context,
    std::unordered_set<ULONGLONG>& visitedVcns) const
{
  // A subnode VCN already on this walk means the on-disk B+ tree is
  // malformed (self-loop or cycle) - stop instead of recursing forever.
  if (!visitedVcns.insert(vcn).second)
  {
    return;
  }

  const std::vector<std::unique_ptr<AttrBase<S>>>& vec =
      getAttr(AttrType::INDEX_ALLOCATION);
  if (vec.empty())
  {
    return;
  }

  IndexBlock ib;
  if (!static_cast<AttrIndexAlloc<S>*>(vec.front().get())
           ->ParseIndexBlock(vcn, ib))
  {
    return;
  }

  for (const IndexEntry& ie : ib)
  {
    if (ie.IsSubNodePtr())
    {
      // recursive call
      TraverseSubNode(ie.GetSubNodeVCN(), seCallBack, context, visitedVcns);
    }

    if (ie.HasName())
    {
      seCallBack(ie, context);
    }
  }
}

// Parse all the attributes in a File Record
// And insert them into a link list
template <Strategy S>
bool FileRecord<S>::ParseAttrs()
{
  // A fresh, empty chain: this is a new, top-level resolution unrelated to
  // whatever $ATTRIBUTE_LIST chain (if any) a previous ParseFileRecord()
  // call on this same FileRecord resolved. See the private overload below.
  std::unordered_set<ULONGLONG> attrListChain;
  return ParseAttrs(attrListChain);
}

template <Strategy S>
bool FileRecord<S>::ParseAttrs(std::unordered_set<ULONGLONG>& attrListChain)
{
  assert(file_record_);

  // Clear previous data
  ClearAttrs();

  // Visit all attributes

  DWORD dataPtr = 0;  // guard if data exceeds file_record_size_ bounds
  const AttrHeaderCommon* ahc = file_record_->HeaderCommon();

  if (ahc == nullptr)
  {
    return false;
  }

  dataPtr += file_record_->GetData()->offset_of_attr;

  while ((static_cast<ULONGLONG>(dataPtr) + sizeof(AttrHeaderCommon) <=
          volume_.GetFileRecordSize()) &&
         ahc->type != AttrType::ALL &&
         (static_cast<ULONGLONG>(dataPtr) + ahc->total_size <=
          volume_.GetFileRecordSize()))
  {
    // Every NTFS attribute is physically at least a resident or
    // non-resident header (Attr::HeaderResident / Attr::HeaderNonResident,
    // per ahc->non_resident) - reject a total_size too small to hold one
    // before AllocAttr()/ParseAttr() reinterpret_cast the raw bytes and
    // read fields (attr_size/attr_offset, or
    // start_vcn/last_vcn/data_run_offset/real_size) that would otherwise
    // come from past this attribute's own declared extent, possibly past
    // the record buffer itself. This subsumes the previous
    // "total_size == 0" guard (0 is always smaller than either minimum),
    // and - unlike that guard - runs before ParseAttr() rather than after.
    const DWORD minTotalSize =
        ahc->non_resident != 0
            ? static_cast<DWORD>(sizeof(Attr::HeaderNonResident))
            : static_cast<DWORD>(sizeof(Attr::HeaderResident));
    if (ahc->total_size < minTotalSize)
    {
      NTFS_TRACE("Attribute total_size too small for its header.\n");
      return false;
    }

    if (IsValidAttrType(ahc->type) &&
        static_cast<bool>(ATTR_MASK(ahc->type) &
                          attr_mask_))  // Skip unwanted/unrecognized attributes
    {
      if (!ParseAttr(*ahc, attrListChain))  // Parse error
      {
        return false;
      }

      if (IsEncrypted() || IsCompressed())
      {
        NTFS_TRACE("Compressed and Encrypted file not supported yet !\n");
        return false;
      }
    }

    dataPtr += ahc->total_size;
    ahc = reinterpret_cast<const AttrHeaderCommon*>(
        reinterpret_cast<const BYTE*>(ahc) +
        ahc->total_size);  // next attribute
  }

  return true;
}

template <Strategy S>
std::optional<ULONGLONG> FileRecord<S>::GetFileReference() const noexcept
{
  return file_reference_;
}

// Install Attribute raw data CallBack routines for a single File Record
template <Strategy S>
bool FileRecord<S>::InstallAttrRawCB(AttrType attrType,
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

// Clear all Attribute CallBack routines
template <Strategy S>
void FileRecord<S>::ClearAttrRawCB() noexcept
{
  for (AttrRawCallback& cb : attr_raw_call_back_)
  {
    cb = nullptr;
  }
}

// Choose attributes to handle, unwanted attributes will be discarded silently
template <Strategy S>
void FileRecord<S>::SetAttrMask(Mask mask) noexcept
{
  // Standard Information and Attribute List is needed always
  attr_mask_ = mask | Mask::STANDARD_INFORMATION | Mask::ATTRIBUTE_LIST;
}

// Traverse all Attribute and return CAttr_xxx classes to User Callback routine
template <Strategy S>
void FileRecord<S>::TraverseAttrs(ATTRS_CALLBACK<S> attrCallBack,
                                  void* context) noexcept
{
  assert(attrCallBack);

  for (size_t i = 0; i < kAttrNums; i++)
  {
    // skip masked attributes
    if (static_cast<bool>(attr_mask_ & (static_cast<Mask>(1U << i))))
    {
      for (const std::unique_ptr<AttrBase<S>>& ab : attr_list_[i])
      {
        bool bStop = false;
        attrCallBack(*ab.get(), context, &bStop);
        if (bStop)
        {
          return;
        }
      }
    }
  }
}

// Find Attributes
template <Strategy S>
const std::vector<std::unique_ptr<AttrBase<S>>>&
    FileRecord<S>::getAttr(AttrType attrType) const noexcept
{
  static std::vector<std::unique_ptr<AttrBase<S>>> dummy{};
  const DWORD attrIdx = ATTR_INDEX(attrType);

  if (attrIdx >= kAttrNums)
  {
    return dummy;
  }

  return attr_list_[attrIdx];
}

template <Strategy S>
std::vector<std::unique_ptr<AttrBase<S>>>&
    FileRecord<S>::getAttr(AttrType attrType) noexcept
{
  static std::vector<std::unique_ptr<AttrBase<S>>> dummy{};
  const DWORD attrIdx = ATTR_INDEX(attrType);

  if (attrIdx >= kAttrNums)
  {
    return dummy;
  }

  return attr_list_[attrIdx];
}

// Get File Name (First Win32 name)
template <Strategy S>
std::wstring_view FileRecord<S>::GetFileName() const
{
  // A file may have several filenames
  // Return the first Win32 filename
  for (const std::unique_ptr<AttrBase<S>>& fn_ :
       attr_list_[ATTR_INDEX(AttrType::FILE_NAME)])
  {
    const Filename* fn;
    switch (S)
    {
      case Strategy::NO_CACHE:
      {
        fn = reinterpret_cast<
            const AttrFileName<AttrResidentNoCache, Strategy::NO_CACHE>*>(
            fn_.get());
        break;
      }
      case Strategy::FULL_CACHE:
      {
        fn = reinterpret_cast<
            const AttrFileName<AttrResidentFullCache, Strategy::FULL_CACHE>*>(
            fn_.get());
        break;
      }
      default:
        assert(false);
        return {};
    }

    if (fn->IsWin32Name() && !fn->GetFilename().empty())
    {
      return fn->GetFilename();
    }
  }

  return {};
}

// Get File Size
template <Strategy S>
ULONGLONG FileRecord<S>::GetFileSize() const noexcept
{
  const std::vector<std::unique_ptr<AttrBase<S>>>& vec =
      attr_list_[ATTR_INDEX(AttrType::FILE_NAME)];
  if (vec.empty())
  {
    return 0;
  }
  if (S == Strategy::NO_CACHE)
  {
    return reinterpret_cast<
               const AttrFileName<AttrResidentNoCache, Strategy::NO_CACHE>*>(
               vec.front().get())
        ->GetFileSize();
  }
  else if (S == Strategy::FULL_CACHE)
  {
    return reinterpret_cast<const AttrFileName<AttrResidentFullCache,
                                               Strategy::FULL_CACHE>*>(
               vec.front().get())
        ->GetFileSize();
  }
  return 0;
}

// Get File Times
template <Strategy S>
void FileRecord<S>::GetFileTime(FILETIME* writeTm, FILETIME* createTm,
                                FILETIME* accessTm) const noexcept
{
  const std::vector<std::unique_ptr<AttrBase<S>>>& vec =
      attr_list_[ATTR_INDEX(AttrType::STANDARD_INFORMATION)];
  // Standard Information attribute hold the most updated file time
  if (!vec.empty())
  {
    if (S == Strategy::NO_CACHE)
    {
      return reinterpret_cast<
                 const AttrStdInfo<AttrResidentNoCache, Strategy::NO_CACHE>*>(
                 vec.front().get())
          ->GetFileTime(writeTm, createTm, accessTm);
    }
    else if (S == Strategy::FULL_CACHE)
    {
      return reinterpret_cast<const AttrStdInfo<AttrResidentFullCache,
                                                Strategy::FULL_CACHE>*>(
                 vec.front().get())
          ->GetFileTime(writeTm, createTm, accessTm);
    }
    return;
  }

  if (writeTm != nullptr)
  {
    writeTm->dwHighDateTime = 0;
    writeTm->dwLowDateTime = 0;
  }
  if (createTm != nullptr)
  {
    createTm->dwHighDateTime = 0;
    createTm->dwLowDateTime = 0;
  }
  if (accessTm != nullptr)
  {
    accessTm->dwHighDateTime = 0;
    accessTm->dwLowDateTime = 0;
  }
}

// Traverse all sub directories and files contained
// Call user defined callback routine once found an entry
template <Strategy S>
void FileRecord<S>::TraverseSubEntries(SUBENTRY_CALLBACK seCallBack,
                                       void* context) const
{
  assert(seCallBack);

  // Start traversing from IndexRoot (B+ tree root node)

  const std::vector<std::unique_ptr<AttrBase<S>>>& vec =
      getAttr(AttrType::INDEX_ROOT);
  if (vec.empty())
  {
    return;
  }

  const std::vector<IndexEntry>* all_ie;

  switch (S)
  {
    case Strategy::NO_CACHE:
    {
      const auto* ir = reinterpret_cast<
          const AttrIndexRoot<AttrResidentNoCache, Strategy::NO_CACHE>*>(
          vec.front().get());

      if (!ir->IsFileName())
      {
        return;
      }
      all_ie = ir;
      break;
    }
    case Strategy::FULL_CACHE:
    {
      const auto* ir = reinterpret_cast<
          const AttrIndexRoot<AttrResidentFullCache, Strategy::FULL_CACHE>*>(
          vec.front().get());

      if (!ir->IsFileName())
      {
        return;
      }
      all_ie = ir;
      break;
    }
    default:
    {
      assert(false);
      return;
    }
  }

  std::unordered_set<ULONGLONG> visitedVcns;

  for (const IndexEntry& ie : *all_ie)
  {
    // Visit subnode first
    if (ie.IsSubNodePtr())
    {
      TraverseSubNode(ie.GetSubNodeVCN(), seCallBack, context, visitedVcns);
    }

    if (ie.HasName())
    {
      seCallBack(ie, context);
    }
  }
}

// Find a specific Filename from InexRoot described B+ tree
template <Strategy S>
std::optional<IndexEntry>
    FileRecord<S>::FindSubEntry(std::wstring_view fileName) const
{
  // Start searching from IndexRoot (B+ tree root node)
  const std::vector<std::unique_ptr<AttrBase<S>>>& vec =
      getAttr(AttrType::INDEX_ROOT);
  if (vec.empty())
  {
    return {};
  }

  const std::vector<IndexEntry>* all_ie = nullptr;

  if (S == Strategy::NO_CACHE)
  {
    const auto* ir = reinterpret_cast<
        const AttrIndexRoot<AttrResidentNoCache, Strategy::NO_CACHE>*>(
        vec.front().get());

    if (!ir->IsFileName())
    {
      return {};
    }
    all_ie = ir;
  }
  else if (S == Strategy::FULL_CACHE)
  {
    const auto* ir = reinterpret_cast<
        const AttrIndexRoot<AttrResidentFullCache, Strategy::FULL_CACHE>*>(
        vec.front().get());

    if (!ir->IsFileName())
    {
      return {};
    }
    all_ie = ir;
  }
  else
  {
    return {};
  }

  std::unordered_set<ULONGLONG> visitedVcns;

  for (const IndexEntry& ie : *all_ie)
  {
    if (ie.HasName())
    {
      // Compare name
      const int i = ie.Compare(fileName);
      if (i == 0)
      {
        // Must be a copy.
        return ie;
      }
      if (i < 0)  // fileName is smaller than IndexEntry
      {
        // Visit SubNode
        if (ie.IsSubNodePtr())
        {
          // Search in SubNode (IndexBlock)
          std::optional<IndexEntry> retval =
              VisitIndexBlock(ie.GetSubNodeVCN(), fileName, visitedVcns);
          if (retval)
          {
            return retval;
          }
        }
        // not found
        else
        {
          return {};
        }
      }
      // Just step forward if fileName is bigger than IndexEntry
    }
    else if (ie.IsSubNodePtr())
    {
      // Search in SubNode (IndexBlock)
      std::optional<IndexEntry> retval =
          VisitIndexBlock(ie.GetSubNodeVCN(), fileName, visitedVcns);
      if (retval)
      {
        return retval;
      }
    }
  }

  return {};
}

// Find Data attribute class of
template <Strategy S>
const AttrBase<S>* FileRecord<S>::FindStream(std::wstring_view name)
{
  const std::vector<std::unique_ptr<AttrBase<S>>>& vec =
      getAttr(AttrType::DATA);
  for (const std::unique_ptr<AttrBase<S>>& data : vec)
  {
    // Unnamed stream
    if (data->IsUnNamed() && name.empty())
    {
      return data.get();
    }
    // Named stream
    if ((!data->IsUnNamed()) && data->GetAttrName() == name)
    {
      break;
    }
  }

  return nullptr;
}

// Check if it's deleted or in use
template <Strategy S>
bool FileRecord<S>::IsDeleted() const noexcept
{
  return !static_cast<bool>(file_record_->GetData()->flags &
                            Flag::FileRecord::INUSE);
}

// Check if it's a directory
template <Strategy S>
bool FileRecord<S>::IsDirectory() const noexcept
{
  return static_cast<bool>(file_record_->GetData()->flags &
                           Flag::FileRecord::DIR);
}

template <Strategy S>
bool FileRecord<S>::IsReadOnly() const noexcept
{
  // Standard Information attribute holds the most updated file time
  const std::vector<std::unique_ptr<AttrBase<S>>>& vec =
      attr_list_[ATTR_INDEX(AttrType::STANDARD_INFORMATION)];
  if (vec.empty())
  {
    return false;
  }

  if (S == Strategy::NO_CACHE)
  {
    return reinterpret_cast<
               const AttrStdInfo<AttrResidentNoCache, Strategy::NO_CACHE>*>(
               vec.front().get())
        ->IsReadOnly();
  }
  else if (S == Strategy::FULL_CACHE)
  {
    return reinterpret_cast<
               const AttrStdInfo<AttrResidentFullCache, Strategy::FULL_CACHE>*>(
               vec.front().get())
        ->IsReadOnly();
  }
  return false;
}

template <Strategy S>
bool FileRecord<S>::IsHidden() const noexcept
{
  const std::vector<std::unique_ptr<AttrBase<S>>>& vec =
      attr_list_[ATTR_INDEX(AttrType::STANDARD_INFORMATION)];
  if (vec.empty())
  {
    return false;
  }

  if (S == Strategy::NO_CACHE)
  {
    return reinterpret_cast<
               const AttrStdInfo<AttrResidentNoCache, Strategy::NO_CACHE>*>(
               vec.front().get())
        ->IsHidden();
  }
  else if (S == Strategy::FULL_CACHE)
  {
    return reinterpret_cast<
               const AttrStdInfo<AttrResidentFullCache, Strategy::FULL_CACHE>*>(
               vec.front().get())
        ->IsHidden();
  }
  return false;
}

template <Strategy S>
bool FileRecord<S>::IsSystem() const noexcept
{
  const std::vector<std::unique_ptr<AttrBase<S>>>& vec =
      attr_list_[ATTR_INDEX(AttrType::STANDARD_INFORMATION)];
  if (vec.empty())
  {
    return false;
  }

  if (S == Strategy::NO_CACHE)
  {
    return reinterpret_cast<
               const AttrStdInfo<AttrResidentNoCache, Strategy::NO_CACHE>*>(
               vec.front().get())
        ->IsSystem();
  }
  else if (S == Strategy::FULL_CACHE)
  {
    return reinterpret_cast<
               const AttrStdInfo<AttrResidentFullCache, Strategy::FULL_CACHE>*>(
               vec.front().get())
        ->IsSystem();
  }
  return false;
}

template <Strategy S>
bool FileRecord<S>::IsCompressed() const noexcept
{
  const std::vector<std::unique_ptr<AttrBase<S>>>& vec =
      attr_list_[ATTR_INDEX(AttrType::STANDARD_INFORMATION)];
  if (vec.empty())
  {
    return false;
  }

  if (S == Strategy::NO_CACHE)
  {
    return reinterpret_cast<
               const AttrStdInfo<AttrResidentNoCache, Strategy::NO_CACHE>*>(
               vec.front().get())
        ->IsCompressed();
  }
  else if (S == Strategy::FULL_CACHE)
  {
    return reinterpret_cast<
               const AttrStdInfo<AttrResidentFullCache, Strategy::FULL_CACHE>*>(
               vec.front().get())
        ->IsCompressed();
  }
  return false;
}

template <Strategy S>
bool FileRecord<S>::IsEncrypted() const noexcept
{
  const std::vector<std::unique_ptr<AttrBase<S>>>& vec =
      attr_list_[ATTR_INDEX(AttrType::STANDARD_INFORMATION)];
  if (vec.empty())
  {
    return false;
  }

  if (S == Strategy::NO_CACHE)
  {
    return reinterpret_cast<
               const AttrStdInfo<AttrResidentNoCache, Strategy::NO_CACHE>*>(
               vec.front().get())
        ->IsEncrypted();
  }
  else if (S == Strategy::FULL_CACHE)
  {
    return reinterpret_cast<
               const AttrStdInfo<AttrResidentFullCache, Strategy::FULL_CACHE>*>(
               vec.front().get())
        ->IsEncrypted();
  }
  return false;
}

template <Strategy S>
bool FileRecord<S>::IsSparse() const noexcept
{
  const std::vector<std::unique_ptr<AttrBase<S>>>& vec =
      attr_list_[ATTR_INDEX(AttrType::STANDARD_INFORMATION)];
  if (vec.empty())
  {
    return false;
  }

  if (S == Strategy::NO_CACHE)
  {
    return reinterpret_cast<
               const AttrStdInfo<AttrResidentNoCache, Strategy::NO_CACHE>*>(
               vec.front().get())
        ->IsSparse();
  }
  else if (S == Strategy::FULL_CACHE)
  {
    return reinterpret_cast<
               const AttrStdInfo<AttrResidentFullCache, Strategy::FULL_CACHE>*>(
               vec.front().get())
        ->IsSparse();
  }
  return false;
}

template class FileRecord<Strategy::NO_CACHE>;
template class FileRecord<Strategy::FULL_CACHE>;

/*
template <Strategy S>
template <typename RESIDENT>
std::unique_ptr<AttrBase<Strategy::FULL_CACHE>>
    FileRecord<S>::AllocAttr<AttrResidentFullCache>(
        const AttrHeaderCommon& ahc, bool& bUnhandled);
template <Strategy S>
template <typename RESIDENT>
std::unique_ptr<AttrBase<Strategy::NO_CACHE>>
    FileRecord<S>::AllocAttr<AttrResidentNoCache>(
        const AttrHeaderCommon& ahc, bool& bUnhandled);
        */
}  // namespace NtfsBrowser
