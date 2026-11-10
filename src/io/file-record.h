#pragma once

#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <unordered_set>
#include <vector>

#include <ntfs-browser/namespace.h>

#include "alloc/types.h"
#include "attr/base.h"
#include "attr/defines.h"
#include "attr/mask.h"
#include "attr/type.h"
#include "cache/strategy.h"
#include "internal-export.h"
#include "io/record-parse-options.h"
#include "io/record-standing.h"
#include "record/carved-name.h"
#include "win-types.h"

namespace NTFS_BROWSER_NAMESPACE {

template <Cache::Strategy S>
class NtfsVolume;
class IndexEntry;
class IndexEntryView;

namespace Attr {

template <class Resident, Cache::Strategy S>
class AttrList;

}  // namespace Attr

namespace Io {

template <Cache::Strategy S>
class NTFS_BROWSER_EXPORT_TESTS_ONLY FileRecord {
 public:
  // User defined Callback routine to handle Directory traversing
  // Will be called by FileRecord::TraverseSubEntries for each sub entry. The
  // view is only valid during the call: build an IndexEntry from it to keep it.
  using SubentryCallback =
      std::function<void(const IndexEntryView& index_entry, void* context)>;

  // The callback of TraverseSubEntriesWithStanding(): like SubentryCallback,
  // with how the record the entry names stands. The view is only valid during
  // the call.
  using StandingCallback =
      std::function<void(const IndexEntryView& index_entry,
                         RecordStanding standing, void* context)>;

  // User defined Callback routine to handle FileRecord parsed attributes
  // Will be called by FileRecord::TraverseAttrs() for each attribute
  // attrClass is the according attribute's wrapping class, CAttr_xxx
  // Set stop to true if don't want to continue
  // Set stop to false to continue processing
  using AttrsCallback = std::function<void(const Attr::AttrBase<S>& attr,
                                           void* context, bool* stop)>;

  // The types of the sources and the store of RecordParseOptions, under their
  // old names.
  using RecordSource = Io::RecordSource;
  using BaseSource = Io::BaseSource;
  using IndexBlockSource = Io::IndexBlockSource;
  using RunSink = Io::RunSink;
  using RunNote = Io::RunNote;
  using ProbeStore = Io::ProbeStore;

  explicit FileRecord(const NtfsVolume<S>& volume);
  // Defined out of line, so the move needs Impl complete only in
  // file-record.cpp, not in every other TU that includes this header.
  FileRecord(FileRecord&& other) noexcept;
  FileRecord(const FileRecord& other) = delete;
  FileRecord& operator=(FileRecord&& other) noexcept = delete;
  FileRecord& operator=(const FileRecord& other) = delete;

  virtual ~FileRecord();

 private:
  // Every member and private method, kept out of this header.
  class Impl;
  std::unique_ptr<Impl> impl_;

 public:
  [[nodiscard]] const NtfsVolume<S>& GetVolume() const noexcept;
  [[nodiscard]] bool ParseFileRecord(ULONGLONG file_ref);
  // ParseFileRecord() with the record's bytes supplied by the caller. They
  // are copied, so the caller may reuse them right away. preread MUST be
  // exactly one file record long.
  [[nodiscard]] bool ParseFileRecord(ULONGLONG file_ref,
                                     std::span<const BYTE> preread);
  [[nodiscard]] bool ParseAttrs();
  // Drops the attributes ParseAttrs() parsed, as the next ParseFileRecord()
  // does first. The parsed header stays.
  void ClearAttrs() noexcept;
  // Whether the update sequence check of the last ParseFileRecord() failed on
  // at least one sector. Strict: the record was rejected. Recovering: it was
  // rejected if no sector verifies, else it was kept (see IsSalvaged()) with
  // the sectors that do not verify left as they are. A record that does not
  // carry the FILE magic is not a failed check.
  [[nodiscard]] bool HasFailedFixup() const noexcept;
  // Whether the parsed record was kept although strict mode would have
  // rejected something in it: a failed update sequence check on some of its
  // sectors, read from its $MFTMirr copy for that reason, or an attribute that
  // the last ParseAttrs() skipped, cut or kept damaged. Always false when
  // recover_errors is off. What was kept is untrusted.
  [[nodiscard]] bool IsSalvaged() const noexcept;
  // How many extension records the last ParseAttrs() skipped because another
  // file reused their slot since this freed record was freed. Zero for a live
  // record, which keeps such an extension as a defect.
  [[nodiscard]] ULONGLONG GetReusedExtensionCount() const noexcept;
  // Whether the last ParseAttrs() dropped the non-resident $ATTRIBUTE_LIST of
  // this freed record, or of one of its extension records, because its bytes
  // are not a list: another file took its clusters since the record was freed.
  // Always false for a live record, and for a resident list, which no other
  // file can write over.
  [[nodiscard]] bool HasOverwrittenAttrList() const noexcept;
  // Called by the $ATTRIBUTE_LIST of this record when it finds itself
  // overwritten, see HasOverwrittenAttrList(). Logs at Debug, once per parse.
  void NoteOverwrittenAttrList();
  [[nodiscard]] std::optional<ULONGLONG> GetFileReference() const noexcept;
  // Times this record was reused; 0 when no record is parsed.
  [[nodiscard]] WORD GetSequenceNumber() const noexcept;
  // The $LogFile sequence number in the header: NTFS writes it with every
  // logged change of the record, its freeing included, so of two freed records
  // the higher one was freed later. 0 when no record is parsed.
  [[nodiscard]] ULONGLONG GetLogSequenceNumber() const noexcept;
  // Record number of the base record this extension record belongs to. 0 for
  // a base record, or when no record is parsed. Also 0 for an extension of
  // $MFT (record 0): use IsExtensionRecord() to tell the two apart.
  [[nodiscard]] ULONGLONG GetBaseRecordReference() const noexcept;
  // True if the parsed record is an extension record: its base file reference
  // is not 0, sequence number included. False when no record is parsed.
  [[nodiscard]] bool IsExtensionRecord() const noexcept;
  // Whether the parsed record holds an attribute of type attr_type, going by
  // the attribute headers alone: nothing is parsed or logged, and a walk that
  // meets a broken header answers false. False when no record is parsed.
  [[nodiscard]] bool HasAttrHeader(Attr::Type attr_type) const noexcept;
  // The clusters ParseAttrs() reads the first max_bytes of the parsed record's
  // $ATTRIBUTE_LIST from, in the order it reads them. Going by the attribute
  // header alone, as HasAttrHeader() does: nothing is read. Empty for a
  // resident or compressed list, or none.
  [[nodiscard]] std::vector<Alloc::Extent>
      AttributeListExtents(ULONGLONG max_bytes) const;
  // Sets how this record parses, all at once: the attributes it keeps, the
  // sources it reuses instead of reading the volume, and the reads it refuses.
  // It replaces every earlier option, those of the default record included. To
  // change one option, start from GetParseOptions(). The functions and flags
  // in options MUST outlive the parses that use them. It acts on the next
  // parse or walk: what is already parsed stays.
  void SetParseOptions(RecordParseOptions options);
  // The options SetParseOptions() was given, or the defaults when there are
  // none. A mask that was not exact comes back with the attributes that
  // parsing adds, so passing it back changes nothing.
  [[nodiscard]] const RecordParseOptions& GetParseOptions() const noexcept;
  // The layouts that RecordParseOptions::claim_mask asked for, from the last
  // ParseAttrs().
  [[nodiscard]] const std::vector<Alloc::StreamLayout>&
      GetClaimedLayouts() const noexcept;

  // What an $ATTRIBUTE_LIST entry names: the base reference of the extension
  // record that holds the attributes, and their type.
  struct AttrListEntryRef {
    ULONGLONG record_number;
    WORD sequence_number;
    Attr::Type attr_type;
  };

  // Moves the attributes of one $ATTRIBUTE_LIST entry, from the extension
  // record that holds them, into this record. A contained or unwanted
  // attribute is skipped. entry is the entry's base reference and type.
  // attr_list_chain holds the (record, attribute type) pairs already resolved
  // along the current $ATTRIBUTE_LIST chain. Throws when the extension record
  // cannot be parsed, or, unless recover is set, when it belongs to another
  // file.
  void ImportAttrListEntry(const AttrListEntryRef& entry,
                           std::unordered_set<ULONGLONG>& attr_list_chain,
                           bool recover);
  // Whether the options set bypass_deleted_gate. A directory record that
  // bypasses the gate also shows the entries that name freed records, since
  // volume metadata is read whatever include_deleted says.
  [[nodiscard]] bool BypassesDeletedGate() const noexcept;
  // Called where a parse of this record is about to read the volume. True
  // when the read is refused: the flag that read_refusal names is then set.
  [[nodiscard]] bool RefuseRead() const noexcept;
  void TraverseAttrs(const AttrsCallback& attr_call_back, void* context);
  [[nodiscard]] const std::vector<std::unique_ptr<Attr::AttrBase<S>>>&
      GetAttr(Attr::Type attr_type) const noexcept;
  [[nodiscard]] std::vector<std::unique_ptr<Attr::AttrBase<S>>>&
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

  // Names recognised in the slack of the record: the bytes after its attribute
  // end marker, where a name of an earlier version of the record, or of an
  // index that shrank, can linger. Each is a candidate, reported apart from the
  // live names: it changes none of the getters above, and no live name of this
  // record is repeated in it. Empty unless the volume was opened with both
  // recover_errors and include_deleted, and ParseAttrs() reached the end
  // marker. Reads nothing from the volume. The times are converted like those
  // of GetFileTime().
  [[nodiscard]] std::vector<Record::CarvedName> CarveSlackNames() const;

  // Names of deleted files recognised in the unused space of this directory's
  // $INDEX_ALLOCATION blocks, past each block's end entry, where NTFS leaves
  // the entries it moved up to close a gap. Each is a candidate, reported apart
  // from the live entries that TraverseSubEntries() gives: it changes none of
  // them. A candidate is kept only if its parent reference names this
  // directory (sequence number included, as for the orphan scan of
  // TraverseSubEntries()), so a name whose parent bytes were overwritten is
  // dropped. A name equal to a live entry of this directory, or already found
  // in another block, is not repeated. A block that fails its integrity checks
  // is skipped and the others are still carved. Empty unless the volume was
  // opened with both recover_errors and include_deleted, and for a record with
  // no $INDEX_ALLOCATION or whose index is not a file-name index. Reads the
  // blocks like the orphan scan (at most 65536 of them) and nothing in strict
  // mode. The times are converted like those of GetFileTime(). Call it after
  // ParseAttrs().
  [[nodiscard]] std::vector<Record::CarvedName> CarveIndexSlackNames() const;

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

  // TraverseSubEntries() with the probes of the named records batched, and
  // their outcome handed to the callback. Gives the same entries in the same
  // order, hides the same ones, and logs the same lines at the same places,
  // except for the Trace lines about disk reads, which follow the batched
  // reads.
  //
  // Every entry the walk shows comes with the standing of the record it names,
  // so the caller need not read that record again. The standing is the probe's
  // own: Live, Deleted (freed), Reused (in use under another sequence number)
  // or Unreadable. A walk that hides deleted items shows Live entries, and
  // Unreadable ones when the volume recovers.
  //
  // The probes of one index node (the root, a block, an orphaned block) run
  // together, sorted by their place in $MFT: neighbouring records are read in
  // one go, and the reads are announced to the reader first. A record is probed
  // once per walk, whatever number of entries name it (a DOS alias and its
  // Win32 name, say), as long as the walk's memo holds it: the memo is bounded,
  // and a directory past that bound probes some records again. The cost is the
  // memo (a few dozen bytes per record, 2^18 records at most) and the bytes of
  // the records of one round, 128 records at most. No entry is copied.
  // Announced reads that nothing used are withdrawn after each round, so a
  // caller's own announcements do not survive the call. Not thread safe, like
  // the rest of the library.
  void TraverseSubEntriesWithStanding(const StandingCallback& se_call_back,
                                      void* context) const;

  [[nodiscard]] std::optional<IndexEntry>
      FindSubEntry(std::wstring_view file_name) const;
  [[nodiscard]] const Attr::AttrBase<S>*
      FindStream(std::wstring_view name) const;

  // Whether the bytes of the data stream called stream_name (empty: the
  // unnamed one) are probably still the file's own: Intact, Reallocated, Bad,
  // or Unknown. The order is Reallocated, then Bad (a cluster is marked bad in
  // $BadClus), then Unknown, then Intact. Intact, without reading $Bitmap or
  // $BadClus, for a record that is not deleted and for resident data. Empty
  // when the stream does not exist. Never changes what ReadData() returns: a
  // read on a bad cluster is attempted like any other. A strong hint, not a
  // proof: a cluster that is free now may have held other data in between. A
  // snapshot of the volume's $Bitmap and $BadClus, and not thread safe.
  [[nodiscard]] std::optional<Alloc::DataState>
      GetDataState(std::wstring_view stream_name = {}) const;

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

}  // namespace Io

}  // namespace NTFS_BROWSER_NAMESPACE
