#include <gsl/narrow>

#include <ntfs-browser/attr-base.h>
#include <ntfs-browser/data/attr-type.h>
#include <ntfs-browser/file-record.h>
#include <ntfs-browser/mask.h>
#include <ntfs-browser/mft-idx.h>
#include <ntfs-browser/ntfs-volume.h>

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
#include "data/file-record-header.h"
#include "data/run-entry.h"
#include "flag/file-record.h"
#include "index-block.h"

namespace NtfsBrowser
{

FileRecord::FileRecord(const NtfsVolume& volume) : volume_(volume)
{
  ClearAttrRawCB();

  // Default to parse all attributes
}

FileRecord::~FileRecord() { ClearAttrs(); }

// Free all CAttr_xxx
void FileRecord::ClearAttrs() noexcept
{
  for (std::vector<std::unique_ptr<AttrBase>>& arr : attr_list_)
  {
    arr.clear();
  }
}

// Call user defined Callback routines for an attribute
void FileRecord::UserCallBack(DWORD attType, const AttrHeaderCommon& ahc,
                              bool& bDiscard) noexcept
{
  bDiscard = false;

  if (attr_raw_call_back_[attType] != nullptr)
  {
    attr_raw_call_back_[attType](ahc, bDiscard);
  }
  else if (volume_.attr_raw_call_back_[attType] != nullptr)
  {
    volume_.attr_raw_call_back_[attType](ahc, bDiscard);
  }
}

extern template class NtfsBrowser::AttrBitmap<AttrNonResident>;
extern template class NtfsBrowser::AttrBitmap<AttrResident>;
extern template class NtfsBrowser::AttrList<AttrNonResident>;
extern template class NtfsBrowser::AttrList<AttrResident>;

std::unique_ptr<AttrBase> FileRecord::AllocAttr(const AttrHeaderCommon& ahc,
                                                bool& bUnhandled)
{
  switch (ahc.type)
  {
    case static_cast<DWORD>(AttrType::STANDARD_INFORMATION):
      return std::make_unique<AttrStdInfo>(ahc, *this);

    case static_cast<DWORD>(AttrType::ATTRIBUTE_LIST):
      if (ahc.non_resident != 0)
      {
        return std::make_unique<AttrList<AttrNonResident>>(ahc, *this);
      }
      return std::make_unique<AttrList<AttrResident>>(ahc, *this);

    case static_cast<DWORD>(AttrType::FILE_NAME):
      return std::make_unique<AttrFileName>(ahc, *this);

    case static_cast<DWORD>(AttrType::VOLUME_NAME):
      return std::make_unique<AttrVolName>(ahc, *this);

    case static_cast<DWORD>(AttrType::VOLUME_INFORMATION):
      return std::make_unique<AttrVolInfo>(ahc, *this);

    case static_cast<DWORD>(AttrType::DATA):
      if (ahc.non_resident != 0)
      {
        return std::make_unique<AttrData<AttrNonResident>>(ahc, *this);
      }
      return std::make_unique<AttrData<AttrResident>>(ahc, *this);

    case static_cast<DWORD>(AttrType::INDEX_ROOT):
      return std::make_unique<AttrIndexRoot>(ahc, *this);

    case static_cast<DWORD>(AttrType::INDEX_ALLOCATION):
      return std::make_unique<AttrIndexAlloc>(ahc, *this);

    case static_cast<DWORD>(AttrType::BITMAP):
      if (ahc.non_resident != 0)
      {
        return std::make_unique<AttrBitmap<AttrNonResident>>(ahc, *this);
      }
      // Resident Bitmap may exist in a directory's FileRecord
      // or in $MFT for a very small volume in theory
      return std::make_unique<AttrBitmap<AttrResident>>(ahc, *this);

    // Unhandled Attributes
    default:
      bUnhandled = TRUE;
      if (ahc.non_resident != 0)
      {
        return std::make_unique<AttrNonResident>(ahc, *this);
      }
      return std::make_unique<AttrResident>(ahc, *this);
  }
}

// Parse a single Attribute
// Return False on error
bool FileRecord::ParseAttr(const AttrHeaderCommon& ahc)
{
  const DWORD attrIndex = ATTR_INDEX(ahc.type);
  if (attrIndex < kAttrNums)
  {
    bool bDiscard = false;
    UserCallBack(attrIndex, ahc, bDiscard);

    if (!bDiscard)
    {
      bool bUnhandled = false;
      std::unique_ptr<AttrBase> attr = AllocAttr(ahc, bUnhandled);
      if (attr)
      {
        if (bUnhandled)
        {
          NTFS_TRACE1("Unhandled attribute: 0x%04X\n", ahc.type);
        }
        attr_list_[attrIndex].push_back(std::move(attr));
        return TRUE;
      }
      NTFS_TRACE1("Attribute Parse error: 0x%04X\n", ahc.type);
      return false;
    }
    NTFS_TRACE1("User Callback has processed this Attribute: 0x%04X\n",
                ahc.type);
    return TRUE;
  }
  NTFS_TRACE1("Invalid Attribute Type: 0x%04X\n", ahc.type);
  return false;
}

// Read File Record
std::unique_ptr<FileRecordHeader> FileRecord::ReadFileRecord(ULONGLONG fileRef)
{
  std::vector<BYTE> buffer;
  buffer.reserve(volume_.file_record_size_);

  if (fileRef < static_cast<ULONGLONG>(Enum::MftIdx::USER) ||
      volume_.mft_data_ == nullptr)
  {
    // Take as continuous disk allocation
    LARGE_INTEGER frAddr;
    frAddr.QuadPart = gsl::narrow<LONGLONG>(
        volume_.mft_addr_ + (volume_.file_record_size_) * fileRef);
    frAddr.LowPart = SetFilePointer(volume_.hvolume_.get(),
                                    static_cast<LONG>(frAddr.LowPart),
                                    &frAddr.HighPart, FILE_BEGIN);

    if (frAddr.LowPart == static_cast<DWORD>(-1) && GetLastError() != NO_ERROR)
    {
      return {};
    }
    if (DWORD len = 0;
        ReadFile(volume_.hvolume_.get(), buffer.data(),
                 volume_.file_record_size_, &len, nullptr) != 0 &&
        len == volume_.file_record_size_)
    {
      std::unique_ptr<FileRecordHeader> fr = std::make_unique<FileRecordHeader>(
          buffer.data(), volume_.file_record_size_, volume_.sector_size_);
      return fr;
    }
    return {};
  }
  // May be fragmented $MFT
  const ULONGLONG frAddr = (volume_.file_record_size_) * fileRef;

  if (ULONGLONG len = 0;
      volume_.mft_data_->ReadData(frAddr, buffer.data(),
                                  volume_.file_record_size_, len) &&
      len == volume_.file_record_size_)
  {
    std::unique_ptr<FileRecordHeader> fr = std::make_unique<FileRecordHeader>(
        buffer.data(), volume_.file_record_size_, volume_.sector_size_);
    return fr;
  }
  return {};
}

// Read File Record, verify and patch the US (update sequence)
bool FileRecord::ParseFileRecord(ULONGLONG fileRef)
{
  // Clear previous data
  ClearAttrs();
  if (file_record_)
  {
    file_record_.reset(nullptr);
  }

  std::unique_ptr<FileRecordHeader> fr = ReadFileRecord(fileRef);
  if (!fr)
  {
    NTFS_TRACE1("Cannot read file record %I64u\n", fileRef);

    file_reference_ = {};
  }
  else
  {
    file_reference_ = fileRef;

    if (fr->magic == kFileRecordMagic)
    {
      if (fr->PatchUS())
      {
        NTFS_TRACE1("File Record %I64u Found\n", fileRef);
        file_record_ = std::move(fr);

        return TRUE;
      }
      NTFS_TRACE("Update Sequence Number error\n");
    }
    else
    {
      NTFS_TRACE("Invalid file record\n");
    }
  }

  return false;
}

// Visit IndexBlocks recursivly to find a specific Filename
std::optional<IndexEntry>
    FileRecord::VisitIndexBlock(const ULONGLONG& vcn,
                                std::wstring_view fileName) const
{
  const std::vector<std::unique_ptr<AttrBase>>& vec =
      getAttr(static_cast<DWORD>(AttrType::INDEX_ALLOCATION));
  if (vec.empty())
  {
    return {};
  }

  IndexBlock ib;
  if (!static_cast<AttrIndexAlloc*>(vec.front().get())
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
            VisitIndexBlock(ie.GetSubNodeVCN(), fileName);
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
          VisitIndexBlock(ie.GetSubNodeVCN(), fileName);
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
void FileRecord::TraverseSubNode(const ULONGLONG& vcn,
                                 SUBENTRY_CALLBACK seCallBack) const
{
  const std::vector<std::unique_ptr<AttrBase>>& vec =
      getAttr(static_cast<DWORD>(AttrType::INDEX_ALLOCATION));
  if (vec.empty())
  {
    return;
  }

  IndexBlock ib;
  if (static_cast<AttrIndexAlloc*>(vec.front().get())->ParseIndexBlock(vcn, ib))

  {
    for (const IndexEntry& ie : ib)
    {
      if (ie.IsSubNodePtr())
      {
        // recursive call
        TraverseSubNode(ie.GetSubNodeVCN(), seCallBack);
      }

      if (ie.HasName())
      {
        seCallBack(ie);
      }
    }
  }
}

// Parse all the attributes in a File Record
// And insert them into a link list
bool FileRecord::ParseAttrs()
{
  _ASSERT(file_record_);

  // Clear previous data
  ClearAttrs();

  // Visit all attributes

  DWORD dataPtr = 0;  // guard if data exceeds file_record_size_ bounds
  const AttrHeaderCommon* ahc = &file_record_->HeaderCommon();
  dataPtr += file_record_->offset_of_attr;

  while (ahc->type != static_cast<DWORD>(-1) &&
         (dataPtr + ahc->total_size) <= volume_.file_record_size_)
  {
    if (static_cast<bool>(ATTR_MASK(ahc->type) &
                          attr_mask_))  // Skip unwanted attributes
    {
      if (!ParseAttr(*ahc))  // Parse error
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

// Install Attribute raw data CallBack routines for a single File Record
bool FileRecord::InstallAttrRawCB(DWORD attrType, AttrRawCallback cb) noexcept
{
  const DWORD atIdx = ATTR_INDEX(attrType);
  if (atIdx < kAttrNums)
  {
    attr_raw_call_back_[atIdx] = cb;
    return true;
  }
  return false;
}

// Clear all Attribute CallBack routines
void FileRecord::ClearAttrRawCB() noexcept
{
  for (int i = 0; i < kAttrNums; i++)
  {
    attr_raw_call_back_[i] = nullptr;
  }
}

// Choose attributes to handle, unwanted attributes will be discarded silently
void FileRecord::SetAttrMask(Mask mask) noexcept
{
  // Standard Information and Attribute List is needed always
  attr_mask_ = mask | Mask::STANDARD_INFORMATION | Mask::ATTRIBUTE_LIST;
}

// Traverse all Attribute and return CAttr_xxx classes to User Callback routine
void FileRecord::TraverseAttrs(ATTRS_CALLBACK attrCallBack,
                               void* context) noexcept
{
  _ASSERT(attrCallBack);

  for (size_t i = 0; i < kAttrNums; i++)
  {
    // skip masked attributes
    if (static_cast<bool>(attr_mask_ & (static_cast<Mask>(1U << i))))
    {
      for (const std::unique_ptr<AttrBase>& ab : attr_list_[i])
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
const std::vector<std::unique_ptr<AttrBase>>&
    FileRecord::getAttr(DWORD attrType) const noexcept
{
  static std::vector<std::unique_ptr<AttrBase>> dummy{};
  const DWORD attrIdx = ATTR_INDEX(attrType);

  if (attrIdx < kAttrNums)
  {
    return attr_list_[attrIdx];
  }
  return dummy;
}

std::vector<std::unique_ptr<AttrBase>>&
    FileRecord::getAttr(DWORD attrType) noexcept
{
  static std::vector<std::unique_ptr<AttrBase>> dummy{};
  const DWORD attrIdx = ATTR_INDEX(attrType);

  if (attrIdx < kAttrNums)
  {
    return attr_list_[attrIdx];
  }
  return dummy;
}

// Get File Name (First Win32 name)
std::wstring FileRecord::GetFileName() const
{
  // A file may have several filenames
  // Return the first Win32 filename
  for (const std::unique_ptr<AttrBase>& fn_ :
       attr_list_[ATTR_INDEX(static_cast<DWORD>(AttrType::FILE_NAME))])
  {
    const auto* fn = static_cast<const AttrFileName*>(fn_.get());
    if (fn->IsWin32Name() && !fn->GetFilename().empty())
    {
      return fn->GetFilename();
    }
  }

  return {};
}

// Get File Size
ULONGLONG FileRecord::GetFileSize() const noexcept
{
  const std::vector<std::unique_ptr<AttrBase>>& vec =
      attr_list_[ATTR_INDEX(static_cast<DWORD>(AttrType::FILE_NAME))];
  return vec.empty() ? 0
                     : reinterpret_cast<const AttrFileName*>(vec.front().get())
                           ->GetFileSize();
}

// Get File Times
void FileRecord::GetFileTime(FILETIME* writeTm, FILETIME* createTm,
                             FILETIME* accessTm) const noexcept
{
  const std::vector<std::unique_ptr<AttrBase>>& vec = attr_list_[ATTR_INDEX(
      static_cast<DWORD>(AttrType::STANDARD_INFORMATION))];
  // Standard Information attribute hold the most updated file time
  if (!vec.empty())
  {
    reinterpret_cast<const AttrStdInfo*>(vec.front().get())
        ->GetFileTime(writeTm, createTm, accessTm);
  }
  else
  {
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
}

// Traverse all sub directories and files contained
// Call user defined callback routine once found an entry
void FileRecord::TraverseSubEntries(SUBENTRY_CALLBACK seCallBack) const
{
  _ASSERT(seCallBack);

  // Start traversing from IndexRoot (B+ tree root node)

  const std::vector<std::unique_ptr<AttrBase>>& vec =
      getAttr(static_cast<DWORD>(AttrType::INDEX_ROOT));
  if (vec.empty())
  {
    return;
  }

  const auto* ir = reinterpret_cast<const AttrIndexRoot*>(vec.front().get());

  if (!ir->IsFileName())
  {
    return;
  }

  for (const IndexEntry& ie : *ir)
  {
    // Visit subnode first
    if (ie.IsSubNodePtr())
    {
      TraverseSubNode(ie.GetSubNodeVCN(), seCallBack);
    }

    if (ie.HasName())
    {
      seCallBack(ie);
    }
  }
}

// Find a specific Filename from InexRoot described B+ tree
std::optional<IndexEntry>
    FileRecord::FindSubEntry(std::wstring_view fileName) const
{
  // Start searching from IndexRoot (B+ tree root node)
  const std::vector<std::unique_ptr<AttrBase>>& vec =
      getAttr(static_cast<DWORD>(AttrType::INDEX_ROOT));
  if (vec.empty())
  {
    return {};
  }

  const auto* ir = reinterpret_cast<const AttrIndexRoot*>(vec.front().get());

  if (!ir->IsFileName())
  {
    return {};
  }

  for (const IndexEntry& ie : *ir)
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
              VisitIndexBlock(ie.GetSubNodeVCN(), fileName);
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
          VisitIndexBlock(ie.GetSubNodeVCN(), fileName);
      if (retval)
      {
        return retval;
      }
    }
  }

  return {};
}

// Find Data attribute class of
const AttrBase* FileRecord::FindStream(std::wstring_view name)
{
  const std::vector<std::unique_ptr<AttrBase>>& vec =
      getAttr(static_cast<DWORD>(AttrType::DATA));
  for (const std::unique_ptr<AttrBase>& data : vec)
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
bool FileRecord::IsDeleted() const noexcept
{
  return !static_cast<bool>(file_record_->flags & Flag::FileRecord::INUSE);
}

// Check if it's a directory
bool FileRecord::IsDirectory() const noexcept
{
  return static_cast<bool>(file_record_->flags & Flag::FileRecord::DIR);
}

bool FileRecord::IsReadOnly() const noexcept
{
  // Standard Information attribute holds the most updated file time
  const std::vector<std::unique_ptr<AttrBase>>& vec = attr_list_[ATTR_INDEX(
      static_cast<DWORD>(AttrType::STANDARD_INFORMATION))];
  return vec.empty() ? false
                     : reinterpret_cast<const AttrStdInfo*>(vec.front().get())
                           ->IsReadOnly();
}

bool FileRecord::IsHidden() const noexcept
{
  const std::vector<std::unique_ptr<AttrBase>>& vec = attr_list_[ATTR_INDEX(
      static_cast<DWORD>(AttrType::STANDARD_INFORMATION))];
  return vec.empty() ? false
                     : reinterpret_cast<const AttrStdInfo*>(vec.front().get())
                           ->IsHidden();
}

bool FileRecord::IsSystem() const noexcept
{
  const std::vector<std::unique_ptr<AttrBase>>& vec = attr_list_[ATTR_INDEX(
      static_cast<DWORD>(AttrType::STANDARD_INFORMATION))];
  return vec.empty() ? false
                     : reinterpret_cast<const AttrStdInfo*>(vec.front().get())
                           ->IsSystem();
}

bool FileRecord::IsCompressed() const noexcept
{
  const std::vector<std::unique_ptr<AttrBase>>& vec = attr_list_[ATTR_INDEX(
      static_cast<DWORD>(AttrType::STANDARD_INFORMATION))];
  return vec.empty() ? false
                     : reinterpret_cast<const AttrStdInfo*>(vec.front().get())
                           ->IsCompressed();
}

bool FileRecord::IsEncrypted() const noexcept
{
  const std::vector<std::unique_ptr<AttrBase>>& vec = attr_list_[ATTR_INDEX(
      static_cast<DWORD>(AttrType::STANDARD_INFORMATION))];
  return vec.empty() ? false
                     : reinterpret_cast<const AttrStdInfo*>(vec.front().get())
                           ->IsEncrypted();
}

bool FileRecord::IsSparse() const noexcept
{
  const std::vector<std::unique_ptr<AttrBase>>& vec = attr_list_[ATTR_INDEX(
      static_cast<DWORD>(AttrType::STANDARD_INFORMATION))];
  return vec.empty() ? false
                     : reinterpret_cast<const AttrStdInfo*>(vec.front().get())
                           ->IsSparse();
}

}  // namespace NtfsBrowser