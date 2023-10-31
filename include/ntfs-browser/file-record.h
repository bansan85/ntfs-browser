#pragma once

#include <array>
#include <memory>
#include <optional>
#include <string_view>
#include <functional>
#include <vector>

#include <tchar.h>
#include <windows.h>

#include <ntfs-browser/data/attr-defines.h>
#include <ntfs-browser/data/attr-type.h>
#include <ntfs-browser/mask.h>
#include <ntfs-browser/strategy.h>
#include <ntfs-browser/data/file-record-header.h>

namespace NtfsBrowser
{
template <Strategy S>
class NtfsVolume;
class IndexEntry;

template <Strategy S>
class AttrBase;

// User defined Callback routine to handle Directory traversing
// Will be called by FileRecord::TraverseSubEntries for each sub entry
using SUBENTRY_CALLBACK =
    std::function<void(const IndexEntry& ie, void* context)>;

// User defined Callback routine to handle FileRecord parsed attributes
// Will be called by FileRecord::TraverseAttrs() for each attribute
// attrClass is the according attribute's wrapping class, CAttr_xxx
// Set bStop to true if don't want to continue
// Set bStop to false to continue processing
template <Strategy S>
using ATTRS_CALLBACK =
    std::function<void(const AttrBase<S>& attr, void* context, bool* bStop)>;

template <Strategy S>
class FileRecord
{
 public:
  explicit FileRecord(const NtfsVolume<S>& volume);
  FileRecord(FileRecord&& other) noexcept = default;
  FileRecord(FileRecord const& other) = delete;
  FileRecord& operator=(FileRecord&& other) noexcept = delete;
  FileRecord& operator=(FileRecord const& other) = delete;

  virtual ~FileRecord();
  friend class AttrBase<S>;
  template <class TYPE_RESIDENT, Strategy S>
  friend class AttrList;

 private:
  const NtfsVolume<S>& volume_;
  std::optional<FileRecordHeaderImpl<S>> file_record_{};
  std::optional<ULONGLONG> file_reference_{};
  std::array<AttrRawCallback, kAttrNums> attr_raw_call_back_{};
  Mask attr_mask_{Mask::ALL};
  std::array<std::vector<std::unique_ptr<AttrBase<S>>>, kAttrNums> attr_list_{};

  void ClearAttrs() noexcept;
  void UserCallBack(DWORD attType, const AttrHeaderCommon& ahc,
                    bool& bDiscard) noexcept;
  template <typename RESIDENT>
  [[nodiscard]] std::unique_ptr<AttrBase<S>>
      AllocAttr(const AttrHeaderCommon& ahc, bool& bUnhandled);
  [[nodiscard]] bool ParseAttr(const AttrHeaderCommon& ahc);
  [[nodiscard]] std::optional<FileRecordHeaderImpl<S>>
      ReadFileRecord(ULONGLONG fileRef);
  [[nodiscard]] std::optional<IndexEntry>
      VisitIndexBlock(ULONGLONG vcn, std::wstring_view fileName) const;
  void TraverseSubNode(ULONGLONG vcn, SUBENTRY_CALLBACK seCallBack,
                       void* context) const;

 public:
  [[nodiscard]] const NtfsVolume<S>& GetVolume() const noexcept;
  [[nodiscard]] bool ParseFileRecord(ULONGLONG fileRef);
  [[nodiscard]] bool ParseAttrs();
  [[nodiscard]] std::optional<ULONGLONG> GetFileReference() const noexcept;
  [[nodiscard]] bool InstallAttrRawCB(AttrType attrType,
                                      AttrRawCallback cb) noexcept;
  void ClearAttrRawCB() noexcept;

  void SetAttrMask(Mask mask) noexcept;
  void TraverseAttrs(ATTRS_CALLBACK<S> attrCallBack, void* context) noexcept;
  [[nodiscard]] const std::vector<std::unique_ptr<AttrBase<S>>>&
      getAttr(AttrType attrType) const noexcept;
  [[nodiscard]] std::vector<std::unique_ptr<AttrBase<S>>>&
      getAttr(AttrType attrType) noexcept;

  [[nodiscard]] std::wstring_view GetFileName() const;
  [[nodiscard]] ULONGLONG GetFileSize() const noexcept;
  void GetFileTime(FILETIME* writeTm, FILETIME* createTm,
                   FILETIME* accessTm) const noexcept;

  void TraverseSubEntries(SUBENTRY_CALLBACK seCallBack, void* context) const;
  [[nodiscard]] std::optional<IndexEntry>
      FindSubEntry(std::wstring_view fileName) const;
  [[nodiscard]] const AttrBase<S>* FindStream(std::wstring_view name);

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
