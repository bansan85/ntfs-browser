#pragma once

#include <array>
#include <memory>
#include <vector>

#include <tchar.h>
#include <windows.h>

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
using SUBENTRY_CALLBACK = void (*)(const IndexEntry& ie);

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
  explicit FileRecord(const NtfsVolume& volume);
  FileRecord(FileRecord&& other) noexcept = default;
  FileRecord(FileRecord const& other) = delete;
  FileRecord& operator=(FileRecord&& other) noexcept = delete;
  FileRecord& operator=(FileRecord const& other) = delete;

  virtual ~FileRecord();
  friend class AttrBase;
  template <class TYPE_RESIDENT>
  friend class AttrList;

 private:
  const NtfsVolume& Volume;
  std::unique_ptr<FileRecordHeader> file_record_;
  ULONGLONG FileReference;
  std::array<AttrRawCallback, kAttrNums> AttrRawCallBack;
  Mask AttrMask;
  std::array<std::vector<AttrBase*>, kAttrNums> attr_list_;  // Attributes

  void ClearAttrs();
  [[nodiscard]] BOOL PatchUS(WORD* sector, int sectors, WORD usn,
                             WORD* usarray);
  void UserCallBack(DWORD attType, const AttrHeaderCommon& ahc, BOOL& bDiscard);
  [[nodiscard]] AttrBase* AllocAttr(const AttrHeaderCommon& ahc,
                                    BOOL& bUnhandled);
  [[nodiscard]] BOOL ParseAttr(const AttrHeaderCommon& ahc);
  [[nodiscard]] std::unique_ptr<FileRecordHeader>
      ReadFileRecord(ULONGLONG& fileRef);
  const IndexEntry* VisitIndexBlock(const ULONGLONG& vcn,
                                    const _TCHAR* fileName) const;
  void TraverseSubNode(const ULONGLONG& vcn,
                       SUBENTRY_CALLBACK seCallBack) const;

 public:
  [[nodiscard]] BOOL ParseFileRecord(ULONGLONG fileRef);
  [[nodiscard]] BOOL ParseAttrs();

  [[nodiscard]] BOOL InstallAttrRawCB(DWORD attrType, AttrRawCallback cb);
  void ClearAttrRawCB();

  void SetAttrMask(Mask mask);
  void TraverseAttrs(ATTRS_CALLBACK attrCallBack, void* context);
  [[nodiscard]] const std::vector<AttrBase*>* getAttr(DWORD attrType) const;
  [[nodiscard]] std::vector<AttrBase*>* getAttr(DWORD attrType);

  [[nodiscard]] int GetFileName(_TCHAR* buf, DWORD bufLen) const;
  [[nodiscard]] ULONGLONG GetFileSize() const;
  void GetFileTime(FILETIME* writeTm, FILETIME* createTm,
                   FILETIME* accessTm) const;

  void TraverseSubEntries(SUBENTRY_CALLBACK seCallBack) const;
  [[nodiscard]] const IndexEntry* FindSubEntry(const _TCHAR* fileName) const;
  [[nodiscard]] const AttrBase* FindStream(_TCHAR* name);

  [[nodiscard]] BOOL IsDeleted() const;
  [[nodiscard]] BOOL IsDirectory() const;
  [[nodiscard]] BOOL IsReadOnly() const;
  [[nodiscard]] BOOL IsHidden() const;
  [[nodiscard]] BOOL IsSystem() const;
  [[nodiscard]] BOOL IsCompressed() const;
  [[nodiscard]] BOOL IsEncrypted() const;
  [[nodiscard]] BOOL IsSparse() const;
};  // FileRecord
}  // namespace NtfsBrowser
