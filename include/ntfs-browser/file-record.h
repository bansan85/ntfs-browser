#pragma once

#include <ntfs-browser/win-types.h>

#include <array>
#include <functional>
#include <memory>
#include <optional>
#include <string_view>
#include <unordered_set>
#include <vector>

#include <ntfs-browser/data/attr-defines.h>
#include <ntfs-browser/data/attr-type.h>
#include <ntfs-browser/export.h>
#include <ntfs-browser/mask.h>
#include <ntfs-browser/strategy.h>

namespace NtfsBrowser
{
namespace Efs
{
struct WrappedFek;
}  // namespace Efs
template <Strategy S>
class NtfsVolume;
class IndexEntry;

// On-disk file record header layout - an implementation detail FileRecord
// keeps behind a pointer so this public header doesn't need its definition.
template <Strategy S>
struct FileRecordHeaderImpl;

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
class NTFS_BROWSER_EXPORT FileRecord
{
 public:
  explicit FileRecord(const NtfsVolume<S>& volume);
  FileRecord(FileRecord&& other) noexcept = default;
  FileRecord(FileRecord const& other) = delete;
  FileRecord& operator=(FileRecord&& other) noexcept = delete;
  FileRecord& operator=(FileRecord const& other) = delete;

  virtual ~FileRecord();
  friend class AttrBase<S>;
  friend class NtfsVolume<S>;
  template <class TYPE_RESIDENT, Strategy>
  friend class AttrList;

 private:
  const NtfsVolume<S>& volume_;
  std::unique_ptr<FileRecordHeaderImpl<S>> file_record_;
  std::optional<ULONGLONG> file_reference_{};
  std::array<AttrRawCallback, kAttrNums> attr_raw_call_back_{};
  Mask attr_mask_{Mask::ALL};
  std::array<std::vector<std::unique_ptr<AttrBase<S>>>, kAttrNums> attr_list_{};

  // False makes AllocAttr() wrap $ATTRIBUTE_LIST generically, not via AttrList.
  bool resolve_attr_list_{true};

  // True bypasses the deleted-record gate in ParseAttrs(): set by
  // NtfsVolume on its own internal FileRecords ($Volume, $MFT, $MFT
  // extension records), so a freed one doesn't take the whole volume down,
  // whatever include_deleted says.
  bool bypass_deleted_gate_{false};

  // Owned per-instance so this FileRecord's raw bytes (viewed by NO_CACHE
  // attributes as plain pointers/spans, no copy) are never aliased by
  // another FileRecord's read (eg. NtfsVolume::mft_record_ vs. this one).
  std::vector<BYTE> record_buffer_;

  void ClearAttrs() noexcept;
  // Attaches an EFS decryption context to every encrypted $DATA stream.
  // False only when strict and an anomalous stream was found: the caller
  // then rejects the whole record instead of reading it undecrypted.
  [[nodiscard]] bool AttachEfsContext();
  [[nodiscard]] std::vector<Efs::WrappedFek> ReadEfsEntries() const;
  void UserCallBack(DWORD attType, const AttrHeaderCommon& ahc, bool& bDiscard);
  template <typename RESIDENT>
  [[nodiscard]] std::unique_ptr<AttrBase<S>>
      AllocAttr(const AttrHeaderCommon& ahc, bool& bUnhandled,
                std::unordered_set<ULONGLONG>& attrListChain);
  [[nodiscard]] bool ParseAttr(const AttrHeaderCommon& ahc,
                               std::unordered_set<ULONGLONG>& attrListChain);
  // attrListChain carries one $ATTRIBUTE_LIST resolution's already-visited
  // (record, attribute type) pairs into this record's own attribute parse,
  // instead of starting a fresh chain.
  [[nodiscard]] bool ParseAttrs(std::unordered_set<ULONGLONG>& attrListChain);
  [[nodiscard]] std::unique_ptr<FileRecordHeaderImpl<S>>
      ReadFileRecord(ULONGLONG fileRef);
  [[nodiscard]] std::optional<IndexEntry>
      VisitIndexBlock(ULONGLONG vcn, std::wstring_view fileName,
                      std::unordered_set<ULONGLONG>& visitedVcns,
                      size_t depth) const;
  void TraverseSubNode(ULONGLONG vcn, SUBENTRY_CALLBACK seCallBack,
                       void* context,
                       std::unordered_set<ULONGLONG>& visitedVcns,
                       size_t depth) const;
  // TraverseSubEntries()'s recoverOrphanedBlocks pass: visitedVcns is the set
  // the normal B+ tree walk already reached, and is extended here in place.
  void
      ScanOrphanedIndexBlocks(SUBENTRY_CALLBACK seCallBack, void* context,
                              std::unordered_set<ULONGLONG>& visitedVcns) const;

 public:
  [[nodiscard]] const NtfsVolume<S>& GetVolume() const noexcept;
  [[nodiscard]] bool ParseFileRecord(ULONGLONG fileRef);
  [[nodiscard]] bool ParseAttrs();
  [[nodiscard]] std::optional<ULONGLONG> GetFileReference() const noexcept;
  // Times this record was reused; 0 when no record is parsed.
  [[nodiscard]] WORD GetSequenceNumber() const noexcept;
  // Record number of the base record this extension record belongs to. 0 for
  // a base record, or when no record is parsed.
  [[nodiscard]] ULONGLONG GetBaseRecordReference() const noexcept;
  [[nodiscard]] bool InstallAttrRawCB(AttrType attrType,
                                      AttrRawCallback cb) noexcept;
  void ClearAttrRawCB() noexcept;

  void SetAttrMask(Mask mask) noexcept;
  void TraverseAttrs(ATTRS_CALLBACK<S> attrCallBack, void* context);
  [[nodiscard]] const std::vector<std::unique_ptr<AttrBase<S>>>&
      getAttr(AttrType attrType) const noexcept;
  [[nodiscard]] std::vector<std::unique_ptr<AttrBase<S>>>&
      getAttr(AttrType attrType) noexcept;

  [[nodiscard]] std::wstring_view GetFileName() const;
  [[nodiscard]] ULONGLONG GetFileSize() const noexcept;
  void GetFileTime(FILETIME* writeTm, FILETIME* createTm,
                   FILETIME* accessTm) const noexcept;

  // With the volume's recover_errors on, also scans every $INDEX_ALLOCATION
  // block the B+ tree walk itself doesn't reach - recovery for a directory
  // whose $INDEX_ROOT or an internal node is corrupt and no longer points at
  // every child block, or whose $INDEX_ROOT attribute is missing entirely.
  // An entry found this way is only reported if its parent reference still
  // matches this directory, and, with include_deleted off, only if the
  // record it names is still in use under a matching sequence number.
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
