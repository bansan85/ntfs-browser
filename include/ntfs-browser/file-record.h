#pragma once

#include <ntfs-browser/win-types.h>

#include <functional>
#include <memory>
#include <optional>
#include <string_view>
#include <vector>

#include <ntfs-browser/attr-base.h>
#include <ntfs-browser/data/attr-defines.h>
#include <ntfs-browser/data/attr-type.h>
#include <ntfs-browser/export.h>
#include <ntfs-browser/mask.h>
#include <ntfs-browser/strategy.h>

namespace NtfsBrowser {

template <Cache::Strategy S>
class NtfsVolume;
class IndexEntry;
class IndexEntryView;

namespace Attr {

template <class Resident, Cache::Strategy S>
class AttrList;

}  // namespace Attr

template <Cache::Strategy S>
class NTFS_BROWSER_EXPORT FileRecord {
 public:
  // User defined Callback routine to handle Directory traversing
  // Will be called by FileRecord::TraverseSubEntries for each sub entry. The
  // view is only valid during the call: build an IndexEntry from it to keep it.
  using SubentryCallback =
      std::function<void(const IndexEntryView& index_entry, void* context)>;

  // User defined Callback routine to handle FileRecord parsed attributes
  // Will be called by FileRecord::TraverseAttrs() for each attribute
  // attrClass is the according attribute's wrapping class, CAttr_xxx
  // Set stop to true if don't want to continue
  // Set stop to false to continue processing
  using AttrsCallback =
      std::function<void(const AttrBase<S>& attr, void* context, bool* stop)>;

  explicit FileRecord(const NtfsVolume<S>& volume);
  // Defined out of line, so the move needs Impl complete only in
  // file-record.cpp, not in every other TU that includes this header.
  FileRecord(FileRecord&& other) noexcept;
  FileRecord(const FileRecord& other) = delete;
  FileRecord& operator=(FileRecord&& other) noexcept = delete;
  FileRecord& operator=(const FileRecord& other) = delete;

  virtual ~FileRecord();
  friend class AttrBase<S>;
  friend class NtfsVolume<S>;
  template <class Resident, Cache::Strategy>
  friend class Attr::AttrList;

 private:
  // Every member and private method, kept out of this header.
  class Impl;
  std::unique_ptr<Impl> impl_;

 public:
  [[nodiscard]] const NtfsVolume<S>& GetVolume() const noexcept;
  [[nodiscard]] bool ParseFileRecord(ULONGLONG file_ref);
  [[nodiscard]] bool ParseAttrs();
  [[nodiscard]] std::optional<ULONGLONG> GetFileReference() const noexcept;
  // Times this record was reused; 0 when no record is parsed.
  [[nodiscard]] WORD GetSequenceNumber() const noexcept;
  // Record number of the base record this extension record belongs to. 0 for
  // a base record, or when no record is parsed. Also 0 for an extension of
  // $MFT (record 0): use IsExtensionRecord() to tell the two apart.
  [[nodiscard]] ULONGLONG GetBaseRecordReference() const noexcept;
  // True if the parsed record is an extension record: its base file reference
  // is not 0, sequence number included. False when no record is parsed.
  [[nodiscard]] bool IsExtensionRecord() const noexcept;
  [[nodiscard]] bool InstallAttrRawCB(Attr::Type attr_type,
                                      Attr::RawCallback callback) noexcept;
  void ClearAttrRawCB() noexcept;

  void SetAttrMask(Attr::Mask mask) noexcept;
  void TraverseAttrs(const AttrsCallback& attr_call_back, void* context);
  [[nodiscard]] const std::vector<std::unique_ptr<AttrBase<S>>>&
      GetAttr(Attr::Type attr_type) const noexcept;
  [[nodiscard]] std::vector<std::unique_ptr<AttrBase<S>>>&
      GetAttr(Attr::Type attr_type) noexcept;

  [[nodiscard]] std::wstring_view GetFileName() const;
  [[nodiscard]] ULONGLONG GetFileSize() const noexcept;
  // Bytes actually allocated on disk for the unnamed $DATA stream; 0 when
  // there is none (eg. a directory).
  [[nodiscard]] ULONGLONG GetAllocatedSize() const noexcept;
  // changeTm is the last MFT (metadata) change time, distinct from writeTm's
  // content modification time: it also moves on a rename or attribute change
  // that leaves the file's content untouched.
  void GetFileTime(FILETIME* write_tm, FILETIME* create_tm, FILETIME* access_tm,
                   FILETIME* change_tm = nullptr) const noexcept;

  // With the volume's recover_errors on, also scans every $INDEX_ALLOCATION
  // block the B+ tree walk itself doesn't reach - recovery for a directory
  // whose $INDEX_ROOT or an internal node is corrupt and no longer points at
  // every child block, or whose $INDEX_ROOT attribute is missing entirely.
  // An entry found this way is only reported if its parent reference still
  // matches this directory, sequence number included (0 matches any; a freed
  // directory also matches the sequence it had before it was freed), and,
  // with include_deleted off, only if the
  // record it names is still in use under a matching sequence number.
  void TraverseSubEntries(const SubentryCallback& se_call_back,
                          void* context) const;

  [[nodiscard]] std::optional<IndexEntry>
      FindSubEntry(std::wstring_view file_name) const;
  [[nodiscard]] const AttrBase<S>* FindStream(std::wstring_view name) const;

  [[nodiscard]] bool IsDeleted() const noexcept;
  [[nodiscard]] bool IsDirectory() const noexcept;
  [[nodiscard]] bool IsReadOnly() const noexcept;
  [[nodiscard]] bool IsHidden() const noexcept;
  [[nodiscard]] bool IsSystem() const noexcept;
  [[nodiscard]] bool IsArchive() const noexcept;
  // Marks a device file; never set on NTFS's own on-disk files in practice.
  [[nodiscard]] bool IsDevice() const noexcept;
  // Set only when no other StdInfoPermission bit is set.
  [[nodiscard]] bool IsNormal() const noexcept;
  [[nodiscard]] bool IsTemporary() const noexcept;
  [[nodiscard]] bool IsCompressed() const noexcept;
  [[nodiscard]] bool IsOffline() const noexcept;
  [[nodiscard]] bool IsNotContentIndexed() const noexcept;
  [[nodiscard]] bool IsEncrypted() const noexcept;
  [[nodiscard]] bool IsSparse() const noexcept;
  [[nodiscard]] bool IsReparsePoint() const noexcept;
};  // FileRecord

}  // namespace NtfsBrowser
