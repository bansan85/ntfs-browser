#pragma once

#include <array>
#include <memory>
#include <optional>
#include <vector>
#include <string_view>

#include <tchar.h>
#include <windows.h>

#include <ntfs-browser/data/attr-defines.h>
#include <ntfs-browser/mask.h>

// OK

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
                                bool* bStop);

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
  const NtfsVolume& volume_;
  std::unique_ptr<FileRecordHeader> file_record_;
  std::optional<ULONGLONG> file_reference_;
  std::array<AttrRawCallback, kAttrNums> attr_raw_call_back_;
  Mask attr_mask_;
  std::array<std::vector<AttrBase*>, kAttrNums> attr_list_;  // Attributes

  void ClearAttrs();
  [[nodiscard]] bool PatchUS(WORD* sector, int sectors, WORD usn,
                             WORD* usarray);
  void UserCallBack(DWORD attType, const AttrHeaderCommon& ahc, bool& bDiscard);
  [[nodiscard]] AttrBase* AllocAttr(const AttrHeaderCommon& ahc,
                                    bool& bUnhandled);
  [[nodiscard]] bool ParseAttr(const AttrHeaderCommon& ahc);
  [[nodiscard]] std::unique_ptr<FileRecordHeader>
      ReadFileRecord(ULONGLONG fileRef);
  [[nodiscard]] std::optional<IndexEntry>
      VisitIndexBlock(const ULONGLONG& vcn, std::wstring_view fileName) const;
  void TraverseSubNode(const ULONGLONG& vcn,
                       SUBENTRY_CALLBACK seCallBack) const;

 public:
  [[nodiscard]] bool ParseFileRecord(ULONGLONG fileRef);
  [[nodiscard]] bool ParseAttrs();

  [[nodiscard]] bool InstallAttrRawCB(DWORD attrType, AttrRawCallback cb);
  void ClearAttrRawCB();

  void SetAttrMask(Mask mask);
  void TraverseAttrs(ATTRS_CALLBACK attrCallBack, void* context);
  [[nodiscard]] const std::vector<AttrBase*>& getAttr(DWORD attrType) const;
  [[nodiscard]] std::vector<AttrBase*>& getAttr(DWORD attrType);

  [[nodiscard]] std::wstring GetFileName() const;
  [[nodiscard]] ULONGLONG GetFileSize() const;
  void GetFileTime(FILETIME* writeTm, FILETIME* createTm,
                   FILETIME* accessTm) const;

  void TraverseSubEntries(SUBENTRY_CALLBACK seCallBack) const;
  [[nodiscard]] std::optional<IndexEntry>
      FindSubEntry(std::wstring_view fileName) const;
  [[nodiscard]] const AttrBase* FindStream(std::wstring_view name);

  [[nodiscard]] bool IsDeleted() const;
  [[nodiscard]] bool IsDirectory() const;
  [[nodiscard]] bool IsReadOnly() const;
  [[nodiscard]] bool IsHidden() const;
  [[nodiscard]] bool IsSystem() const;
  [[nodiscard]] bool IsCompressed() const;
  [[nodiscard]] bool IsEncrypted() const;
  [[nodiscard]] bool IsSparse() const;
};  // FileRecord
}  // namespace NtfsBrowser
