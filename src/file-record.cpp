#include <ntfs-browser/file-record.h>
#include <ntfs-browser/ntfs-volume.h>
#include <ntfs-browser/data/attr-type.h>
#include <ntfs-browser/attr-std-info.h>
#include <ntfs-browser/attr-list.h>
#include <ntfs-browser/attr-non-resident.h>
#include <ntfs-browser/attr-resident.h>
#include <ntfs-browser/attr-file-name.h>
#include <ntfs-browser/attr-vol-name.h>
#include <ntfs-browser/attr-vol-info.h>
#include <ntfs-browser/attr-data.h>
#include <ntfs-browser/attr-bitmap.h>
#include <ntfs-browser/attr-index-root.h>
#include <ntfs-browser/attr-index-alloc.h>
#include <ntfs-browser/data/file-record-header.h>
#include <ntfs-browser/data/mft-idx.h>

namespace NtfsBrowser
{

FileRecord::FileRecord(const NtfsVolume* volume)
{
  _ASSERT(volume);
  Volume = volume;
  file_record_ = NULL;
  FileReference = (ULONGLONG)-1;

  ClearAttrRawCB();

  // Default to parse all attributes
  AttrMask = MASK_ALL;
}

FileRecord::~FileRecord()
{
  ClearAttrs();

  if (file_record_) delete file_record_;
}

// Free all CAttr_xxx
void FileRecord::ClearAttrs()
{
  for (int i = 0; i < kAttrNums; i++)
  {
    attr_list_[i].clear();
  }
}

// Verify US and update sectors
BOOL FileRecord::PatchUS(WORD* sector, int sectors, WORD usn, WORD* usarray)
{
  int i;

  for (i = 0; i < sectors; i++)
  {
    sector += ((Volume->SectorSize >> 1) - 1);
    if (*sector != usn) return FALSE;  // USN error
    *sector = usarray[i];              // Write back correct data
    sector++;
  }
  return TRUE;
}

// Call user defined Callback routines for an attribute
void FileRecord::UserCallBack(DWORD attType, AttrHeaderCommon* ahc,
                              BOOL* bDiscard)
{
  *bDiscard = FALSE;

  if (AttrRawCallBack[attType])
    AttrRawCallBack[attType](ahc, bDiscard);
  else if (Volume->AttrRawCallBack[attType])
    Volume->AttrRawCallBack[attType](ahc, bDiscard);
}

AttrBase* FileRecord::AllocAttr(AttrHeaderCommon* ahc, BOOL* bUnhandled)
{
  switch (ahc->Type)
  {
    case static_cast<DWORD>(AttrType::STANDARD_INFORMATION):
      return new AttrStdInfo(ahc, this);

    case static_cast<DWORD>(AttrType::ATTRIBUTE_LIST):
      if (ahc->NonResident)
        return new AttrList<AttrNonResident>(ahc, this);
      else
        return new AttrList<AttrResident>(ahc, this);

    case static_cast<DWORD>(AttrType::FILE_NAME):
      return new AttrFileName(ahc, this);

    case static_cast<DWORD>(AttrType::VOLUME_NAME):
      return new AttrVolName(ahc, this);

    case static_cast<DWORD>(AttrType::VOLUME_INFORMATION):
      return new AttrVolInfo(ahc, this);

    case static_cast<DWORD>(AttrType::DATA):
      if (ahc->NonResident)
        return new AttrData<AttrNonResident>(ahc, this);
      else
        return new AttrData<AttrResident>(ahc, this);

    case static_cast<DWORD>(AttrType::INDEX_ROOT):
      return new AttrIndexRoot(ahc, this);

    case static_cast<DWORD>(AttrType::INDEX_ALLOCATION):
      return new AttrIndexAlloc(ahc, this);

    case static_cast<DWORD>(AttrType::BITMAP):
      if (ahc->NonResident)
        return new AttrBitmap<AttrNonResident>(ahc, this);
      else
        // Resident Bitmap may exist in a directory's FileRecord
        // or in $MFT for a very small volume in theory
        return new AttrBitmap<AttrResident>(ahc, this);

    // Unhandled Attributes
    default:
      *bUnhandled = TRUE;
      if (ahc->NonResident)
        return new AttrNonResident(ahc, this);
      else
        return new AttrResident(ahc, this);
  }
}

// Parse a single Attribute
// Return False on error
BOOL FileRecord::ParseAttr(AttrHeaderCommon* ahc)
{
  DWORD attrIndex = ATTR_INDEX(ahc->Type);
  if (attrIndex < kAttrNums)
  {
    BOOL bDiscard = FALSE;
    UserCallBack(attrIndex, ahc, &bDiscard);

    if (!bDiscard)
    {
      BOOL bUnhandled = FALSE;
      AttrBase* attr = AllocAttr(ahc, &bUnhandled);
      if (attr)
      {
        if (bUnhandled)
        {
          NTFS_TRACE1("Unhandled attribute: 0x%04X\n", ahc->Type);
        }
        attr_list_[attrIndex].push_back(attr);
        return TRUE;
      }
      else
      {
        NTFS_TRACE1("Attribute Parse error: 0x%04X\n", ahc->Type);
        return FALSE;
      }
    }
    else
    {
      NTFS_TRACE1("User Callback has processed this Attribute: 0x%04X\n",
                  ahc->Type);
      return TRUE;
    }
  }
  else
  {
    NTFS_TRACE1("Invalid Attribute Type: 0x%04X\n", ahc->Type);
    return FALSE;
  }
}

// Read File Record
FileRecordHeader* FileRecord::ReadFileRecord(ULONGLONG& fileRef)
{
  FileRecordHeader* fr = NULL;
  DWORD len;

  if (fileRef < static_cast<ULONGLONG>(MftIdx::USER) || Volume->MFTData == NULL)
  {
    // Take as continuous disk allocation
    LARGE_INTEGER frAddr;
    frAddr.QuadPart = Volume->MFTAddr + (Volume->FileRecordSize) * fileRef;
    frAddr.LowPart = SetFilePointer(Volume->hVolume, frAddr.LowPart,
                                    &frAddr.HighPart, FILE_BEGIN);

    if (frAddr.LowPart == DWORD(-1) && GetLastError() != NO_ERROR)
      return FALSE;
    else
    {
      fr = (FileRecordHeader*)new BYTE[Volume->FileRecordSize];

      if (ReadFile(Volume->hVolume, fr, Volume->FileRecordSize, &len, NULL) &&
          len == Volume->FileRecordSize)
        return fr;
      else
      {
        delete fr;
        return NULL;
      }
    }
  }
  else
  {
    // May be fragmented $MFT
    ULONGLONG frAddr;
    frAddr = (Volume->FileRecordSize) * fileRef;

    fr = (FileRecordHeader*)new BYTE[Volume->FileRecordSize];

    if (Volume->MFTData->ReadData(frAddr, fr, Volume->FileRecordSize, &len) &&
        len == Volume->FileRecordSize)
      return fr;
    else
    {
      delete fr;
      return NULL;
    }
  }
}

// Read File Record, verify and patch the US (update sequence)
BOOL FileRecord::ParseFileRecord(ULONGLONG fileRef)
{
  // Clear previous data
  ClearAttrs();
  if (file_record_)
  {
    delete file_record_;
    file_record_ = NULL;
  }

  FileRecordHeader* fr = ReadFileRecord(fileRef);
  if (fr == NULL)
  {
    NTFS_TRACE1("Cannot read file record %I64u\n", fileRef);

    FileReference = (ULONGLONG)-1;
  }
  else
  {
    FileReference = fileRef;

    if (fr->Magic == kFileRecordMagic)
    {
      // Patch US
      WORD* usnaddr = (WORD*)((BYTE*)fr + fr->OffsetOfUS);
      WORD usn = *usnaddr;
      WORD* usarray = usnaddr + 1;
      if (PatchUS((WORD*)fr, Volume->FileRecordSize / Volume->SectorSize, usn,
                  usarray))
      {
        NTFS_TRACE1("File Record %I64u Found\n", fileRef);
        file_record_ = fr;

        return TRUE;
      }
      else
      {
        NTFS_TRACE("Update Sequence Number error\n");
      }
    }
    else
    {
      NTFS_TRACE("Invalid file record\n");
    }

    delete fr;
  }

  return FALSE;
}

// Visit IndexBlocks recursivly to find a specific FileName
BOOL FileRecord::VisitIndexBlock(const ULONGLONG& vcn, const _TCHAR* fileName,
                                 IndexEntry& ieFound) const
{
  const std::vector<AttrBase*>* vec =
      getAttr(static_cast<DWORD>(AttrType::INDEX_ALLOCATION));
  if (vec == NULL || vec->empty()) return FALSE;

  IndexBlock ib;
  if (((AttrIndexAlloc*)vec->front())->ParseIndexBlock(vcn, ib))
  {
    for (IndexEntry* ie : ib)
    {
      if (ie->HasName())
      {
        // Compare name
        int i = ie->Compare(fileName);
        if (i == 0)
        {
          ieFound = *ie;
          return TRUE;
        }
        else if (i < 0)  // fileName is smaller than IndexEntry
        {
          // Visit SubNode
          if (ie->IsSubNodePtr())
          {
            // Search in SubNode (IndexBlock), recursive call
            if (VisitIndexBlock(ie->GetSubNodeVCN(), fileName, ieFound))
              return TRUE;
          }
          else
            return FALSE;  // not found
        }
        // Just step forward if fileName is bigger than IndexEntry
      }
      else if (ie->IsSubNodePtr())
      {
        // Search in SubNode (IndexBlock), recursive call
        if (VisitIndexBlock(ie->GetSubNodeVCN(), fileName, ieFound))
          return TRUE;
      }
    }
  }

  return FALSE;
}

// Traverse SubNode recursivly in ascending order
// Call user defined callback routine once found an subentry
void FileRecord::TraverseSubNode(const ULONGLONG& vcn,
                                 SUBENTRY_CALLBACK seCallBack) const
{
  const std::vector<AttrBase*>* vec =
      getAttr(static_cast<DWORD>(AttrType::INDEX_ALLOCATION));
  if (vec == NULL || vec->empty()) return;

  IndexBlock ib;
  if (((AttrIndexAlloc*)vec->front())->ParseIndexBlock(vcn, ib))

  {
    for (IndexEntry* ie : ib)
    {
      if (ie->IsSubNodePtr())
        TraverseSubNode(ie->GetSubNodeVCN(), seCallBack);  // recursive call

      if (ie->HasName()) seCallBack(ie);
    }
  }
}

// Parse all the attributes in a File Record
// And insert them into a link list
BOOL FileRecord::ParseAttrs()
{
  _ASSERT(file_record_);

  // Clear previous data
  ClearAttrs();

  // Visit all attributes

  DWORD dataPtr = 0;  // guard if data exceeds FileRecordSize bounds
  AttrHeaderCommon* ahc =
      (AttrHeaderCommon*)((BYTE*)file_record_ + file_record_->OffsetOfAttr);
  dataPtr += file_record_->OffsetOfAttr;

  while (ahc->Type != (DWORD)-1 &&
         (dataPtr + ahc->TotalSize) <= Volume->FileRecordSize)
  {
    if (ATTR_MASK(ahc->Type) & AttrMask)  // Skip unwanted attributes
    {
      if (!ParseAttr(ahc))  // Parse error
        return FALSE;

      if (IsEncrypted() || IsCompressed())
      {
        NTFS_TRACE("Compressed and Encrypted file not supported yet !\n");
        return FALSE;
      }
    }

    dataPtr += ahc->TotalSize;
    ahc = (AttrHeaderCommon*)((BYTE*)ahc + ahc->TotalSize);  // next attribute
  }

  return TRUE;
}

// Install Attribute raw data CallBack routines for a single File Record
BOOL FileRecord::InstallAttrRawCB(DWORD attrType, AttrRawCallback cb)
{
  DWORD atIdx = ATTR_INDEX(attrType);
  if (atIdx < kAttrNums)
  {
    AttrRawCallBack[atIdx] = cb;
    return TRUE;
  }
  else
    return FALSE;
}

// Clear all Attribute CallBack routines
void FileRecord::ClearAttrRawCB()
{
  for (int i = 0; i < kAttrNums; i++) AttrRawCallBack[i] = NULL;
}

// Choose attributes to handle, unwanted attributes will be discarded silently
void FileRecord::SetAttrMask(DWORD mask)
{
  // Standard Information and Attribute List is needed always
  AttrMask = mask | MASK_STANDARD_INFORMATION | MASK_ATTRIBUTE_LIST;
}

// Traverse all Attribute and return CAttr_xxx classes to User Callback routine
void FileRecord::TraverseAttrs(ATTRS_CALLBACK attrCallBack, void* context)
{
  _ASSERT(attrCallBack);

  for (int i = 0; i < kAttrNums; i++)
  {
    if (AttrMask & (((DWORD)1) << i))  // skip masked attributes
    {
      for (const AttrBase* ab : attr_list_[i])
      {
        BOOL bStop;
        bStop = FALSE;
        attrCallBack(ab, context, &bStop);
        if (bStop) return;
      }
    }
  }
}

// Find Attributes
const std::vector<AttrBase*>* FileRecord::getAttr(DWORD attrType) const
{
  DWORD attrIdx = ATTR_INDEX(attrType);

  if (attrIdx < kAttrNums)
  {
    return &attr_list_[attrIdx];
  }
  else
    return NULL;
}

std::vector<AttrBase*>* FileRecord::getAttr(DWORD attrType)
{
  DWORD attrIdx = ATTR_INDEX(attrType);

  if (attrIdx < kAttrNums)
  {
    return &attr_list_[attrIdx];
  }
  else
    return NULL;
}

// Get File Name (First Win32 name)
int FileRecord::GetFileName(_TCHAR* buf, DWORD bufLen) const
{
  // A file may have several filenames
  // Return the first Win32 filename
  for (const AttrBase* fn_ :
       attr_list_[ATTR_INDEX(static_cast<DWORD>(AttrType::FILE_NAME))])
  {
    const AttrFileName* fn = static_cast<const AttrFileName*>(fn_);
    if (fn->IsWin32Name())
    {
      int len = fn->GetFileName(buf, bufLen);
      if (len != 0) return len;  // success or fail
    }
  }

  return 0;
}

// Get File Size
ULONGLONG FileRecord::GetFileSize() const
{
  const std::vector<AttrBase*>& vec =
      attr_list_[ATTR_INDEX(static_cast<DWORD>(AttrType::FILE_NAME))];
  return vec.empty() ? 0 : ((AttrFileName&)vec.front()).GetFileSize();
}

// Get File Times
void FileRecord::GetFileTime(FILETIME* writeTm, FILETIME* createTm,
                             FILETIME* accessTm) const
{
  const std::vector<AttrBase*>& vec = attr_list_[ATTR_INDEX(
      static_cast<DWORD>(AttrType::STANDARD_INFORMATION))];
  // Standard Information attribute hold the most updated file time
  if (!vec.empty())
    ((AttrStdInfo*)vec.front())->GetFileTime(writeTm, createTm, accessTm);
  else
  {
    writeTm->dwHighDateTime = 0;
    writeTm->dwLowDateTime = 0;
    if (createTm)
    {
      createTm->dwHighDateTime = 0;
      createTm->dwLowDateTime = 0;
    }
    if (accessTm)
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

  const std::vector<AttrBase*>* vec =
      getAttr(static_cast<DWORD>(AttrType::INDEX_ROOT));
  if (vec == NULL || vec->empty()) return;

  AttrIndexRoot* ir = (AttrIndexRoot*)vec->front();

  if (!ir->IsFileName()) return;

  for (IndexEntry* ie : *ir)
  {
    // Visit subnode first
    if (ie->IsSubNodePtr()) TraverseSubNode(ie->GetSubNodeVCN(), seCallBack);

    if (ie->HasName()) seCallBack(ie);
  }
}

// Find a specific FileName from InexRoot described B+ tree
const BOOL FileRecord::FindSubEntry(const _TCHAR* fileName,
                                    IndexEntry& ieFound) const
{
  // Start searching from IndexRoot (B+ tree root node)
  const std::vector<AttrBase*>* vec =
      getAttr(static_cast<DWORD>(AttrType::INDEX_ROOT));
  if (vec == NULL || vec->empty()) return FALSE;

  AttrIndexRoot* ir = (AttrIndexRoot*)vec->front();

  if (!ir->IsFileName()) return FALSE;

  for (IndexEntry* ie : *ir)
  {
    if (ie->HasName())
    {
      // Compare name
      int i = ie->Compare(fileName);
      if (i == 0)
      {
        ieFound = *ie;
        return TRUE;
      }
      else if (i < 0)  // fileName is smaller than IndexEntry
      {
        // Visit SubNode
        if (ie->IsSubNodePtr())
        {
          // Search in SubNode (IndexBlock)
          if (VisitIndexBlock(ie->GetSubNodeVCN(), fileName, ieFound))
            return TRUE;
        }
        else
          return FALSE;  // not found
      }
      // Just step forward if fileName is bigger than IndexEntry
    }
    else if (ie->IsSubNodePtr())
    {
      // Search in SubNode (IndexBlock)
      if (VisitIndexBlock(ie->GetSubNodeVCN(), fileName, ieFound)) return TRUE;
    }
  }

  return FALSE;
}

// Find Data attribute class of
const AttrBase* FileRecord::FindStream(_TCHAR* name)
{
  const std::vector<AttrBase*>* vec =
      getAttr(static_cast<DWORD>(AttrType::DATA));
  if (vec == NULL) return NULL;
  for (const AttrBase* data : *vec)
  {
    if (data->IsUnNamed() && name == NULL)  // Unnamed stream
      return data;
    if ((!data->IsUnNamed()) && name)  // Named stream
    {
      _TCHAR an[MAX_PATH];
      if (data->GetAttrName(an, MAX_PATH))
      {
        if (_tcscmp(an, name) == 0) break;
      }
    }
  }

  return NULL;
}

// Check if it's deleted or in use
BOOL FileRecord::IsDeleted() const
{
  return !(file_record_->Flags & static_cast<DWORD>(FileRecordFlag::INUSE));
}

// Check if it's a directory
BOOL FileRecord::IsDirectory() const
{
  return file_record_->Flags & static_cast<DWORD>(FileRecordFlag::DIR);
}

BOOL FileRecord::IsReadOnly() const
{
  // Standard Information attribute holds the most updated file time
  const std::vector<AttrBase*>& vec = attr_list_[ATTR_INDEX(
      static_cast<DWORD>(AttrType::STANDARD_INFORMATION))];
  return vec.empty() ? FALSE : ((AttrStdInfo*)vec.front())->IsReadOnly();
}

BOOL FileRecord::IsHidden() const
{
  const std::vector<AttrBase*>& vec = attr_list_[ATTR_INDEX(
      static_cast<DWORD>(AttrType::STANDARD_INFORMATION))];
  return vec.empty() ? FALSE : ((AttrStdInfo*)vec.front())->IsHidden();
}

BOOL FileRecord::IsSystem() const
{
  const std::vector<AttrBase*>& vec = attr_list_[ATTR_INDEX(
      static_cast<DWORD>(AttrType::STANDARD_INFORMATION))];
  return vec.empty() ? FALSE : ((AttrStdInfo*)vec.front())->IsSystem();
}

BOOL FileRecord::IsCompressed() const
{
  const std::vector<AttrBase*>& vec = attr_list_[ATTR_INDEX(
      static_cast<DWORD>(AttrType::STANDARD_INFORMATION))];
  return vec.empty() ? FALSE : ((AttrStdInfo*)vec.front())->IsCompressed();
}

BOOL FileRecord::IsEncrypted() const
{
  const std::vector<AttrBase*>& vec = attr_list_[ATTR_INDEX(
      static_cast<DWORD>(AttrType::STANDARD_INFORMATION))];
  return vec.empty() ? FALSE : ((AttrStdInfo*)vec.front())->IsEncrypted();
}

BOOL FileRecord::IsSparse() const
{
  const std::vector<AttrBase*>& vec = attr_list_[ATTR_INDEX(
      static_cast<DWORD>(AttrType::STANDARD_INFORMATION))];
  return vec.empty() ? FALSE : ((AttrStdInfo*)vec.front())->IsSparse();
}

}  // namespace NtfsBrowser