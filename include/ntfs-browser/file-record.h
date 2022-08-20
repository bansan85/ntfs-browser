#pragma once

#include <array>
#include <memory>
#include <optional>
#include <string_view>
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
using SUBENTRY_CALLBACK = void (*)(const IndexEntry& ie, void* context);

// User defined Callback routine to handle FileRecord parsed attributes
// Will be called by FileRecord::TraverseAttrs() for each attribute
// attrClass is the according attribute's wrapping class, CAttr_xxx
// Set bStop to true if don't want to continue
// Set bStop to false to continue processing
using ATTRS_CALLBACK = void (*)(const AttrBase& attr, void* context,
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
  std::unique_ptr<FileRecordHeader> file_record_{};
  std::optional<ULONGLONG> file_reference_{};
  std::array<AttrRawCallback, kAttrNums> attr_raw_call_back_{};
  Mask attr_mask_{MASK_ALL};
  std::array<std::vector<std::unique_ptr<AttrBase>>, kAttrNums> attr_list_{};

  void ClearAttrs() noexcept;
  [[nodiscard]] bool PatchUS(WORD* sector, int sectors, WORD usn,
                             WORD* usarray);
  void UserCallBack(DWORD attType, const AttrHeaderCommon& ahc,
                    bool& bDiscard) noexcept;
  [[nodiscard]] std::unique_ptr<AttrBase> AllocAttr(const AttrHeaderCommon& ahc,
                                                    bool& bUnhandled);
  [[nodiscard]] bool ParseAttr(const AttrHeaderCommon& ahc);
  [[nodiscard]] std::unique_ptr<FileRecordHeader>
      ReadFileRecord(ULONGLONG fileRef);
  [[nodiscard]] std::optional<IndexEntry>
      VisitIndexBlock(const ULONGLONG& vcn, std::wstring_view fileName) const;
  void TraverseSubNode(const ULONGLONG& vcn, SUBENTRY_CALLBACK seCallBack,
                       void* context) const;

 public:
  [[nodiscard]] const NtfsVolume& GetVolume() const noexcept;
  [[nodiscard]] bool ParseFileRecord(ULONGLONG fileRef);
  [[nodiscard]] bool ParseAttrs();

  [[nodiscard]] bool InstallAttrRawCB(DWORD attrType,
                                      AttrRawCallback cb) noexcept;
  void ClearAttrRawCB() noexcept;

  void SetAttrMask(Mask mask) noexcept;
  void TraverseAttrs(ATTRS_CALLBACK attrCallBack, void* context) noexcept;
  [[nodiscard]] const std::vector<std::unique_ptr<AttrBase>>&
      getAttr(DWORD attrType) const noexcept;
  [[nodiscard]] std::vector<std::unique_ptr<AttrBase>>&
      getAttr(DWORD attrType) noexcept;

  [[nodiscard]] std::wstring GetFileName() const;
  [[nodiscard]] ULONGLONG GetFileSize() const noexcept;
  void GetFileTime(FILETIME* writeTm, FILETIME* createTm,
                   FILETIME* accessTm) const noexcept;

  void TraverseSubEntries(SUBENTRY_CALLBACK seCallBack, void* context) const;
  [[nodiscard]] std::optional<IndexEntry>
      FindSubEntry(std::wstring_view fileName) const;
  [[nodiscard]] const AttrBase* FindStream(std::wstring_view name);

  [[nodiscard]] bool IsDeleted() const noexcept;
  [[nodiscard]] bool IsDirectory() const noexcept;
  [[nodiscard]] bool IsReadOnly() const noexcept;
  [[nodiscard]] bool IsHidden() const noexcept;
  [[nodiscard]] bool IsSystem() const noexcept;
  [[nodiscard]] bool IsCompressed() const noexcept;
  [[nodiscard]] bool IsEncrypted() const noexcept;
  [[nodiscard]] bool IsSparse() const noexcept;
};  // FileRecord
}  // namespace NtfsBrowser
