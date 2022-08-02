#pragma once

#include <vector>

#include <windows.h>
#include <tchar.h>

#include <ntfs-browser/data/attr-defines.h>
#include <ntfs-browser/mask.h>

namespace NtfsBrowser
{
class NtfsVolume;
class IndexEntry;

class AttrBase;
struct FileRecordHeader;

// User defined Callback routine to handle Directory traversing
// Will be called by FileRecord::TraverseSubEntries for each sub entry
using SUBENTRY_CALLBACK = void (*)(const IndexEntry* ie);

// User defined Callback routine to handle FileRecord parsed attributes
// Will be called by FileRecord::TraverseAttrs() for each attribute
// attrClass is the according attribute's wrapping class, CAttr_xxx
// Set bStop to TRUE if don't want to continue
// Set bStop to FALSE to continue processing
using ATTRS_CALLBACK = void (*)(const AttrBase* attr, void* context,
                                BOOL* bStop);

class FileRecord
{
 public:
  FileRecord(const NtfsVolume* volume);
  virtual ~FileRecord();
  friend class AttrBase;
  template <class TYPE_RESIDENT>
  friend class AttrList;

 private:
  const NtfsVolume* Volume;
  FileRecordHeader* file_record_;
  ULONGLONG FileReference;
  AttrRawCallback AttrRawCallBack[kAttrNums];
  Mask AttrMask;
  std::vector<AttrBase*> attr_list_[kAttrNums];  // Attributes

  void ClearAttrs();
  BOOL PatchUS(WORD* sector, int sectors, WORD usn, WORD* usarray);
  void UserCallBack(DWORD attType, AttrHeaderCommon* ahc, BOOL* bDiscard);
  AttrBase* AllocAttr(AttrHeaderCommon* ahc, BOOL* bUnhandled);
  BOOL ParseAttr(AttrHeaderCommon* ahc);
  FileRecordHeader* ReadFileRecord(ULONGLONG& fileRef);
  BOOL VisitIndexBlock(const ULONGLONG& vcn, const _TCHAR* fileName,
                       IndexEntry& ieFound) const;
  void TraverseSubNode(const ULONGLONG& vcn,
                       SUBENTRY_CALLBACK seCallBack) const;

 public:
  BOOL ParseFileRecord(ULONGLONG fileRef);
  BOOL ParseAttrs();

  BOOL InstallAttrRawCB(DWORD attrType, AttrRawCallback cb);
  void ClearAttrRawCB();

  void SetAttrMask(Mask mask);
  void TraverseAttrs(ATTRS_CALLBACK attrCallBack, void* context);
  const std::vector<AttrBase*>* getAttr(DWORD attrType) const;
  std::vector<AttrBase*>* getAttr(DWORD attrType);

  int GetFileName(_TCHAR* buf, DWORD bufLen) const;
  ULONGLONG GetFileSize() const;
  void GetFileTime(FILETIME* writeTm, FILETIME* createTm = NULL,
                   FILETIME* accessTm = NULL) const;

  void TraverseSubEntries(SUBENTRY_CALLBACK seCallBack) const;
  const BOOL FindSubEntry(const _TCHAR* fileName, IndexEntry& ieFound) const;
  const AttrBase* FindStream(_TCHAR* name = NULL);

  BOOL IsDeleted() const;
  BOOL IsDirectory() const;
  BOOL IsReadOnly() const;
  BOOL IsHidden() const;
  BOOL IsSystem() const;
  BOOL IsCompressed() const;
  BOOL IsEncrypted() const;
  BOOL IsSparse() const;
};  // FileRecord
}  // namespace NtfsBrowser
