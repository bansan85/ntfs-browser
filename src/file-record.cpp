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

FileRecord::~FileRecord() {}

const NtfsVolume& FileRecord::GetVolume() const noexcept { return volume_; }

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
  else
  {
    volume_.AttrRawCallBack(attType, ahc, bDiscard);
  }
}

extern template class NtfsBrowser::AttrBitmap<AttrNonResident>;
extern template class NtfsBrowser::AttrBitmap<AttrResidentLight>;
extern template class NtfsBrowser::AttrBitmap<AttrResidentHeavy>;
extern template class NtfsBrowser::AttrList<AttrNonResident>;
extern template class NtfsBrowser::AttrList<AttrResidentLight>;
extern template class NtfsBrowser::AttrList<AttrResidentHeavy>;

template <typename RESIDENT>
std::unique_ptr<AttrBase> FileRecord::AllocAttr(const AttrHeaderCommon& ahc,
                                                bool& bUnhandled)
{
  switch (ahc.type)
  {
    case AttrType::STANDARD_INFORMATION:
      return std::make_unique<AttrStdInfo<RESIDENT>>(ahc, *this);

    case AttrType::ATTRIBUTE_LIST:
      if (ahc.non_resident != 0)
      {
        return std::make_unique<AttrList<AttrNonResident>>(ahc, *this);
      }
      return std::make_unique<AttrList<RESIDENT>>(ahc, *this);

    case AttrType::FILE_NAME:
      return std::make_unique<AttrFileName<RESIDENT>>(ahc, *this);

    case AttrType::VOLUME_NAME:
      return std::make_unique<AttrVolName<RESIDENT>>(ahc, *this);

    case AttrType::VOLUME_INFORMATION:
      return std::make_unique<AttrVolInfo<RESIDENT>>(ahc, *this);

    case AttrType::DATA:
      if (ahc.non_resident != 0)
      {
        return std::make_unique<AttrData<AttrNonResident>>(ahc, *this);
      }
      return std::make_unique<AttrData<RESIDENT>>(ahc, *this);

    case AttrType::INDEX_ROOT:
      return std::make_unique<AttrIndexRoot<RESIDENT>>(ahc, *this);

    case AttrType::INDEX_ALLOCATION:
      return std::make_unique<AttrIndexAlloc>(ahc, *this);

    case AttrType::BITMAP:
      if (ahc.non_resident != 0)
      {
        return std::make_unique<AttrBitmap<AttrNonResident>>(ahc, *this);
      }
      // Resident Bitmap may exist in a directory's FileRecord
      // or in $MFT for a very small volume in theory
      return std::make_unique<AttrBitmap<RESIDENT>>(ahc, *this);

    // Unhandled Attributes
    default:
      bUnhandled = true;
      if (ahc.non_resident != 0)
      {
        return std::make_unique<AttrNonResident>(ahc, *this);
      }
      return std::make_unique<RESIDENT>(ahc, *this);
  }
}

// Parse a single Attribute
// Return False on error
bool FileRecord::ParseAttr(const AttrHeaderCommon& ahc)
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

  std::unique_ptr<AttrBase> attr;
  if (volume_.volume_.GetStrategy() == FileReader::Strategy::NO_CACHE)
    attr = AllocAttr<AttrResidentHeavy>(ahc, bUnhandled);
  else if (volume_.volume_.GetStrategy() == FileReader::Strategy::FULL_CACHE)
    attr = AllocAttr<AttrResidentLight>(ahc, bUnhandled);
  if (attr)
  {
    if (bUnhandled)
    {
      NTFS_TRACE1("Unhandled attribute: 0x%04X\n", ahc.type);
    }
    attr_list_[attrIndex].push_back(std::move(attr));
    return true;
  }
  NTFS_TRACE1("Attribute Parse error: 0x%04X\n", ahc.type);
  return false;
}

// Read File Record
std::unique_ptr<FileRecordHeader> FileRecord::ReadFileRecord(ULONGLONG fileRef)
{
  if (fileRef < static_cast<ULONGLONG>(Enum::MftIdx::USER) ||
      volume_.mft_data_ == nullptr)
  {
    // Take as continuous disk allocation
    LARGE_INTEGER frAddr;
    frAddr.QuadPart = gsl::narrow<LONGLONG>(
        volume_.GetMFTAddr() + (volume_.GetFileRecordSize()) * fileRef);

    std::optional<std::span<const BYTE>> buffer =
        volume_.Read(frAddr, volume_.GetFileRecordSize());
    if (!buffer || buffer->size() != volume_.GetFileRecordSize())
    {
      return {};
    }

    return FileRecordHeader::Factory(*buffer, volume_.GetSectorSize(),
                                     volume_.volume_.GetStrategy());
  }

  // May be fragmented $MFT
  const ULONGLONG frAddr = (volume_.GetFileRecordSize()) * fileRef;

  std::span<BYTE> buffer_file = volume_.GetFileRecordBuffer();
  if (std::optional<ULONGLONG> len =
          volume_.mft_data_->ReadData(frAddr, buffer_file);
      !len || *len != volume_.GetFileRecordSize())
  {
    return {};
  }

  std::unique_ptr<FileRecordHeaderHeavy> fr =
      std::make_unique<FileRecordHeaderHeavy>(buffer_file,
                                              volume_.GetSectorSize());
  return fr;
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
std::optional<IndexEntry>
    FileRecord::VisitIndexBlock(ULONGLONG vcn, std::wstring_view fileName) const
{
  const std::vector<std::unique_ptr<AttrBase>>& vec =
      getAttr(AttrType::INDEX_ALLOCATION);
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
void FileRecord::TraverseSubNode(ULONGLONG vcn, SUBENTRY_CALLBACK seCallBack,
                                 void* context) const
{
  const std::vector<std::unique_ptr<AttrBase>>& vec =
      getAttr(AttrType::INDEX_ALLOCATION);
  if (vec.empty())
  {
    return;
  }

  IndexBlock ib;
  if (!static_cast<AttrIndexAlloc*>(vec.front().get())
           ->ParseIndexBlock(vcn, ib))
  {
    return;
  }

  for (const IndexEntry& ie : ib)
  {
    if (ie.IsSubNodePtr())
    {
      // recursive call
      TraverseSubNode(ie.GetSubNodeVCN(), seCallBack, context);
    }

    if (ie.HasName())
    {
      seCallBack(ie, context);
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
  dataPtr += file_record_->GetData()->offset_of_attr;

  while (ahc->type != AttrType::ALL &&
         (dataPtr + ahc->total_size) <= volume_.GetFileRecordSize())
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

    if (ahc->total_size == 0)
    {
      return false;
    }

    dataPtr += ahc->total_size;
    ahc = reinterpret_cast<const AttrHeaderCommon*>(
        reinterpret_cast<const BYTE*>(ahc) +
        ahc->total_size);  // next attribute
  }

  return true;
}

std::optional<ULONGLONG> FileRecord::GetFileReference() const noexcept
{
  return file_reference_;
}

// Install Attribute raw data CallBack routines for a single File Record
bool FileRecord::InstallAttrRawCB(AttrType attrType,
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
void FileRecord::ClearAttrRawCB() noexcept
{
  for (AttrRawCallback& cb : attr_raw_call_back_)
  {
    cb = nullptr;
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
    FileRecord::getAttr(AttrType attrType) const noexcept
{
  static std::vector<std::unique_ptr<AttrBase>> dummy{};
  const DWORD attrIdx = ATTR_INDEX(attrType);

  if (attrIdx >= kAttrNums)
  {
    return dummy;
  }

  return attr_list_[attrIdx];
}

std::vector<std::unique_ptr<AttrBase>>&
    FileRecord::getAttr(AttrType attrType) noexcept
{
  static std::vector<std::unique_ptr<AttrBase>> dummy{};
  const DWORD attrIdx = ATTR_INDEX(attrType);

  if (attrIdx >= kAttrNums)
  {
    return dummy;
  }

  return attr_list_[attrIdx];
}

// Get File Name (First Win32 name)
std::wstring_view FileRecord::GetFileName() const
{
  // A file may have several filenames
  // Return the first Win32 filename
  for (const std::unique_ptr<AttrBase>& fn_ :
       attr_list_[ATTR_INDEX(AttrType::FILE_NAME)])
  {
    const Filename* fn;
    switch (volume_.volume_.GetStrategy())
    {
      case FileReader::Strategy::NO_CACHE:
      {
        fn = static_cast<const AttrFileName<AttrResidentHeavy>*>(fn_.get());
        break;
      }
      case FileReader::Strategy::FULL_CACHE:
      {
        fn = static_cast<const AttrFileName<AttrResidentLight>*>(fn_.get());
        break;
      }
      default:
        _ASSERT(false);
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
ULONGLONG FileRecord::GetFileSize() const noexcept
{
  const std::vector<std::unique_ptr<AttrBase>>& vec =
      attr_list_[ATTR_INDEX(AttrType::FILE_NAME)];
  if (vec.empty())
  {
    return 0;
  }
  if (volume_.volume_.GetStrategy() == FileReader::Strategy::NO_CACHE)
  {
    return reinterpret_cast<const AttrFileName<AttrResidentHeavy>*>(
               vec.front().get())
        ->GetFileSize();
  }
  else if (volume_.volume_.GetStrategy() == FileReader::Strategy::FULL_CACHE)
  {
    return reinterpret_cast<const AttrFileName<AttrResidentLight>*>(
               vec.front().get())
        ->GetFileSize();
  }
  return 0;
}

// Get File Times
void FileRecord::GetFileTime(FILETIME* writeTm, FILETIME* createTm,
                             FILETIME* accessTm) const noexcept
{
  const std::vector<std::unique_ptr<AttrBase>>& vec =
      attr_list_[ATTR_INDEX(AttrType::STANDARD_INFORMATION)];
  // Standard Information attribute hold the most updated file time
  if (!vec.empty())
  {
    if (volume_.volume_.GetStrategy() == FileReader::Strategy::NO_CACHE)
    {
      return reinterpret_cast<const AttrStdInfo<AttrResidentHeavy>*>(
                 vec.front().get())
          ->GetFileTime(writeTm, createTm, accessTm);
    }
    else if (volume_.volume_.GetStrategy() == FileReader::Strategy::FULL_CACHE)
    {
      return reinterpret_cast<const AttrStdInfo<AttrResidentLight>*>(
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
void FileRecord::TraverseSubEntries(SUBENTRY_CALLBACK seCallBack,
                                    void* context) const
{
  _ASSERT(seCallBack);

  // Start traversing from IndexRoot (B+ tree root node)

  const std::vector<std::unique_ptr<AttrBase>>& vec =
      getAttr(AttrType::INDEX_ROOT);
  if (vec.empty())
  {
    return;
  }

  const std::vector<IndexEntry>* all_ie;

  switch (volume_.volume_.GetStrategy())
  {
    case FileReader::Strategy::NO_CACHE:
    {
      const auto* ir =
          reinterpret_cast<const AttrIndexRoot<AttrResidentHeavy>*>(
              vec.front().get());

      if (!ir->IsFileName())
      {
        return;
      }
      all_ie = ir;
      break;
    }
    case FileReader::Strategy::FULL_CACHE:
    {
      const auto* ir =
          reinterpret_cast<const AttrIndexRoot<AttrResidentLight>*>(
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
      _ASSERT(false);
      return;
    }
  }

  for (const IndexEntry& ie : *all_ie)
  {
    // Visit subnode first
    if (ie.IsSubNodePtr())
    {
      TraverseSubNode(ie.GetSubNodeVCN(), seCallBack, context);
    }

    if (ie.HasName())
    {
      seCallBack(ie, context);
    }
  }
}

// Find a specific Filename from InexRoot described B+ tree
std::optional<IndexEntry>
    FileRecord::FindSubEntry(std::wstring_view fileName) const
{
  // Start searching from IndexRoot (B+ tree root node)
  const std::vector<std::unique_ptr<AttrBase>>& vec =
      getAttr(AttrType::INDEX_ROOT);
  if (vec.empty())
  {
    return {};
  }

  const std::vector<IndexEntry>* all_ie = nullptr;

  if (volume_.volume_.GetStrategy() == FileReader::Strategy::NO_CACHE)
  {
    const auto* ir = reinterpret_cast<const AttrIndexRoot<AttrResidentHeavy>*>(
        vec.front().get());

    if (!ir->IsFileName())
    {
      return {};
    }
    all_ie = ir;
  }
  else if (volume_.volume_.GetStrategy() == FileReader::Strategy::FULL_CACHE)
  {
    const auto* ir = reinterpret_cast<const AttrIndexRoot<AttrResidentLight>*>(
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
  const std::vector<std::unique_ptr<AttrBase>>& vec = getAttr(AttrType::DATA);
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
  return !static_cast<bool>(file_record_->GetData()->flags &
                            Flag::FileRecord::INUSE);
}

// Check if it's a directory
bool FileRecord::IsDirectory() const noexcept
{
  return static_cast<bool>(file_record_->GetData()->flags &
                           Flag::FileRecord::DIR);
}

bool FileRecord::IsReadOnly() const noexcept
{
  // Standard Information attribute holds the most updated file time
  const std::vector<std::unique_ptr<AttrBase>>& vec =
      attr_list_[ATTR_INDEX(AttrType::STANDARD_INFORMATION)];
  if (vec.empty())
  {
    return false;
  }

  if (volume_.volume_.GetStrategy() == FileReader::Strategy::NO_CACHE)
  {
    return reinterpret_cast<const AttrStdInfo<AttrResidentHeavy>*>(
               vec.front().get())
        ->IsReadOnly();
  }
  else if (volume_.volume_.GetStrategy() == FileReader::Strategy::FULL_CACHE)
  {
    return reinterpret_cast<const AttrStdInfo<AttrResidentLight>*>(
               vec.front().get())
        ->IsReadOnly();
  }
  return false;
}

bool FileRecord::IsHidden() const noexcept
{
  const std::vector<std::unique_ptr<AttrBase>>& vec =
      attr_list_[ATTR_INDEX(AttrType::STANDARD_INFORMATION)];
  if (vec.empty())
  {
    return false;
  }

  if (volume_.volume_.GetStrategy() == FileReader::Strategy::NO_CACHE)
  {
    return reinterpret_cast<const AttrStdInfo<AttrResidentHeavy>*>(
               vec.front().get())
        ->IsHidden();
  }
  else if (volume_.volume_.GetStrategy() == FileReader::Strategy::FULL_CACHE)
  {
    return reinterpret_cast<const AttrStdInfo<AttrResidentLight>*>(
               vec.front().get())
        ->IsHidden();
  }
  return false;
}

bool FileRecord::IsSystem() const noexcept
{
  const std::vector<std::unique_ptr<AttrBase>>& vec =
      attr_list_[ATTR_INDEX(AttrType::STANDARD_INFORMATION)];
  if (vec.empty())
  {
    return false;
  }

  if (volume_.volume_.GetStrategy() == FileReader::Strategy::NO_CACHE)
  {
    return reinterpret_cast<const AttrStdInfo<AttrResidentHeavy>*>(
               vec.front().get())
        ->IsSystem();
  }
  else if (volume_.volume_.GetStrategy() == FileReader::Strategy::FULL_CACHE)
  {
    return reinterpret_cast<const AttrStdInfo<AttrResidentLight>*>(
               vec.front().get())
        ->IsSystem();
  }
  return false;
}

bool FileRecord::IsCompressed() const noexcept
{
  const std::vector<std::unique_ptr<AttrBase>>& vec =
      attr_list_[ATTR_INDEX(AttrType::STANDARD_INFORMATION)];
  if (vec.empty())
  {
    return false;
  }

  if (volume_.volume_.GetStrategy() == FileReader::Strategy::NO_CACHE)
  {
    return reinterpret_cast<const AttrStdInfo<AttrResidentHeavy>*>(
               vec.front().get())
        ->IsCompressed();
  }
  else if (volume_.volume_.GetStrategy() == FileReader::Strategy::FULL_CACHE)
  {
    return reinterpret_cast<const AttrStdInfo<AttrResidentLight>*>(
               vec.front().get())
        ->IsCompressed();
  }
  return false;
}

bool FileRecord::IsEncrypted() const noexcept
{
  const std::vector<std::unique_ptr<AttrBase>>& vec =
      attr_list_[ATTR_INDEX(AttrType::STANDARD_INFORMATION)];
  if (vec.empty())
  {
    return false;
  }

  if (volume_.volume_.GetStrategy() == FileReader::Strategy::NO_CACHE)
  {
    return reinterpret_cast<const AttrStdInfo<AttrResidentHeavy>*>(
               vec.front().get())
        ->IsEncrypted();
  }
  else if (volume_.volume_.GetStrategy() == FileReader::Strategy::FULL_CACHE)
  {
    return reinterpret_cast<const AttrStdInfo<AttrResidentLight>*>(
               vec.front().get())
        ->IsEncrypted();
  }
  return false;
}

bool FileRecord::IsSparse() const noexcept
{
  const std::vector<std::unique_ptr<AttrBase>>& vec =
      attr_list_[ATTR_INDEX(AttrType::STANDARD_INFORMATION)];
  if (vec.empty())
  {
    return false;
  }

  if (volume_.volume_.GetStrategy() == FileReader::Strategy::NO_CACHE)
  {
    return reinterpret_cast<const AttrStdInfo<AttrResidentHeavy>*>(
               vec.front().get())
        ->IsSparse();
  }
  else if (volume_.volume_.GetStrategy() == FileReader::Strategy::FULL_CACHE)
  {
    return reinterpret_cast<const AttrStdInfo<AttrResidentLight>*>(
               vec.front().get())
        ->IsSparse();
  }
  return false;
}

template std::unique_ptr<AttrBase>
    FileRecord::AllocAttr<AttrResidentHeavy>(const AttrHeaderCommon& ahc,
                                             bool& bUnhandled);
template std::unique_ptr<AttrBase>
    FileRecord::AllocAttr<AttrResidentLight>(const AttrHeaderCommon& ahc,
                                             bool& bUnhandled);

}  // namespace NtfsBrowser