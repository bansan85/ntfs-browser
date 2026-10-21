#pragma once

#include <ntfs-browser/win-types.h>

#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include <ntfs-browser/alloc/types.h>
#include <ntfs-browser/cache/strategy.h>
#include <ntfs-browser/export.h>
#include <ntfs-browser/namespace.h>
#include <ntfs-browser/record/carved-name.h>

namespace NTFS_BROWSER_NAMESPACE {

template <Cache::Strategy S>
class NtfsVolume;

namespace Mft {

// Options of one MftTree scan, known as MftTree::ScanOptions. It sits outside
// the class so that the constructors can default it to {}: a nested struct with
// a default member initializer cannot be used that way inside its own class.
struct MftScanOptions {
  // Called every few thousand records, and once at the end. Returning false
  // stops the scan: the tree then holds the records scanned so far.
  std::function<bool(ULONGLONG done, ULONGLONG total)> progress;
  // True gives each kept entry its data_state. It only takes effect when the
  // volume was opened with include_deleted, since only then does the scan
  // list freed records. It reads the cluster bitmap and the runs of each
  // deleted file, in sorted batches. False skips both: the scan then reads
  // exactly what it would without this feature.
  bool annotate_data_state{true};
};

// Every file of a volume, rebuilt from the parent reference in each MFT
// record's own $FILE_NAME, not from directory indexes. It survives the loss
// of a directory's index, or of the directory's record itself.
//
// A name's parent reference is valid when it names a directory the scan
// found and one of these holds: the sequence numbers match; the reference
// carries sequence 0, which NTFS never checks; or the parent was freed and
// its sequence is one more (NTFS bumps it on deletion), which keeps a
// deleted subtree whole. The root directory (record 5) is always a valid
// parent, even when its own record is lost.
//
// The scan reads every MFT record once and copies out each record's names:
// build it from a NoCache volume, since FullCache would also keep the whole
// $MFT in memory.
class NTFS_BROWSER_EXPORT MftTree {
 public:
  // What the change journal ($UsnJrnl:$J) says about one file, from the
  // records of the journal that name it. Only a volume opened with
  // recover_errors reads the journal. A record names a file by its record
  // number and the sequence number it had at the time: one that does not fit
  // the $MFT record's own generation names an earlier file, and is not counted.
  struct UsnInfo {
    // USN of the newest record that names the file.
    LONGLONG last_usn{0};
    // Reason bits (Usn::Reason) of that record, and its time, converted like
    // the entry's own times.
    DWORD last_reason{0};
    FILETIME last_time{};
    // Every reason bit of every record that names the file.
    DWORD all_reasons{0};
    // Time of the newest record with the FileDelete reason. Empty when the
    // journal holds none.
    std::optional<FILETIME> delete_time;
    // Records that name the file, among those the volume kept.
    ULONGLONG record_count{0};
  };

  // One $FILE_NAME of an MFT record. A record has one per hard link, plus a
  // DOS 8.3 alias when its long name needs one.
  struct Name {
    std::wstring name;
    ULONGLONG parent_record{0};
    WORD parent_sequence{0};
    // A pure DOS 8.3 alias. Never used to build a path.
    bool dos_only{false};
    // The parent reference names a directory the scan found, under a matching
    // sequence number. See MftTree for the exact rules.
    bool parent_valid{false};
  };

  // What one MFT base record says about its file, copied out of the record.
  struct Entry {
    ULONGLONG record{0};
    WORD sequence{0};
    bool in_use{false};
    bool directory{false};
    std::vector<Name> names;
    // Real size of the unnamed $DATA stream. Without one, the $FILE_NAME size,
    // which NTFS only refreshes on a rename.
    ULONGLONG size{0};
    // Allocated size of the unnamed $DATA stream; 0 without one.
    ULONGLONG allocated_size{0};
    // From $STANDARD_INFORMATION, in local time.
    FILETIME create_time{};
    FILETIME write_time{};
    // Last MFT (metadata) change time, distinct from write_time's content
    // modification time.
    FILETIME change_time{};
    FILETIME access_time{};
    bool read_only{false};
    bool hidden{false};
    bool system{false};
    bool archive{false};
    bool compressed{false};
    bool encrypted{false};
    bool sparse{false};
    // Whether the unnamed $DATA stream, the one size describes, probably still
    // holds the file's own bytes (a hint, not a proof). Intact for a record in
    // use. Bad when a cluster of a freed file is marked bad and none was
    // reallocated. Empty when the scan was not asked to annotate, see
    // ScanOptions::annotate_data_state, or when the record has no such stream:
    // a directory, for instance.
    std::optional<Alloc::DataState> data_state;
    // What the journal says about the file. Empty when the scan did not read
    // the journal (recover_errors off), or when it does not name the file.
    std::optional<UsnInfo> usn;
    // Names recognised in the slack of the record, after its attribute end
    // marker: candidates, never live names, and none repeats a name of the
    // record. Filled only by a scan whose volume was opened with both
    // recover_errors and include_deleted on. Empty otherwise.
    std::vector<Record::CarvedName> carved_names;
  };

  // A file that the change journal names and that no $MFT record accounts for:
  // the record is gone, overwritten, or reused by another file. It has no data
  // and no attribute beyond what the journal gives, and it is not an Entry:
  // Entries() and Find() keep one entry per record number. Only a scan with
  // both recover_errors and include_deleted on lists them.
  struct JournalOnlyEntry {
    ULONGLONG record{0};
    WORD sequence{0};
    // The name of the newest journal record that names the file. Not a path.
    std::wstring name;
    ULONGLONG parent_record{0};
    WORD parent_sequence{0};
    // The parent reference names a directory the scan found, under the rules
    // of MftTree. Then GetJournalPath() reaches the file through it.
    bool parent_valid{false};
    // File attributes at the time of the newest journal record.
    DWORD file_attributes{0};
    // The directory attribute was set then.
    bool directory{false};
    UsnInfo usn;
  };

  using ScanOptions = MftScanOptions;

  struct ScanStats {
    // Record slots $MFT has room for.
    ULONGLONG slots{0};
    // Base records in use.
    ULONGLONG in_use{0};
    // Base records NTFS freed, kept or not depending on the volume's
    // include_deleted.
    ULONGLONG deleted{0};
    // Extension records. Their attributes are read through their base record.
    ULONGLONG extensions{0};
    // Slots with no FILE magic (never used), a bad fixup, or a read error.
    ULONGLONG unreadable{0};
    // Slots $MFT's $BITMAP marks free, which the scan did not read. Only when
    // the volume's include_deleted is off and $MFT has a usable $BITMAP. Such
    // a slot is counted here and in none of the counters above, whatever it
    // holds: a freed record, or one that never held a file.
    ULONGLONG skipped{0};
    // Records with an attribute that failed to parse, counted either way.
    // Dropped unless the volume's recover_errors is on, which keeps what
    // parsed before the failure.
    ULONGLONG damaged{0};
    // Kept records that no chain of valid parent references links to the root.
    ULONGLONG unreachable{0};
    // False when progress() stopped the scan early.
    bool complete{true};
    // Names carved from record slack, over all entries. Zero unless the volume
    // has both recover_errors and include_deleted on.
    ULONGLONG carved_names{0};
    // Entries given a data_state, by state. All zero when the scan did not
    // annotate.
    ULONGLONG data_intact{0};
    ULONGLONG data_reallocated{0};
    ULONGLONG data_unknown{0};
    // Entries whose data sits on bad clusters. Counted next to the three above.
    ULONGLONG data_bad{0};
    // The journal's side of the scan, all zero or false when it was not read
    // (recover_errors off, no journal, or a scan that progress() stopped).
    // Journal records the volume kept.
    ULONGLONG usn_records{0};
    // Entries given a UsnInfo.
    ULONGLONG usn_annotated{0};
    // Entries of JournalOnly().
    ULONGLONG usn_journal_only{0};
    // Stretches of the journal that were skipped as damaged.
    ULONGLONG usn_damaged{0};
    // The journal held more records than the volume keeps.
    bool usn_truncated{false};
  };

  // Scans every record of volume's $MFT.
  explicit MftTree(const NtfsVolume<Cache::Strategy::NoCache>& volume,
                   const ScanOptions& options = {});
  explicit MftTree(const NtfsVolume<Cache::Strategy::FullCache>& volume,
                   const ScanOptions& options = {});
  MftTree(const MftTree& other);
  MftTree(MftTree&& other) noexcept;
  MftTree& operator=(const MftTree& other);
  MftTree& operator=(MftTree&& other) noexcept;
  ~MftTree();

  // Every kept record, in record number order.
  [[nodiscard]] const std::vector<Entry>& Entries() const noexcept;
  // nullptr when the scan did not keep that record.
  [[nodiscard]] const Entry* Find(ULONGLONG record) const;
  // Records filed under dirRecord through a valid parent reference, once
  // each, even when several of their names sit in it.
  [[nodiscard]] std::span<const ULONGLONG> Children(ULONGLONG dir_record) const;
  // Whether valid parent references lead from record up to the root.
  [[nodiscard]] bool IsReachable(ULONGLONG record) const;
  // Path of record through its first non-DOS name: "\dir\file" when it
  // reaches the root. Otherwise "dir\file", relative to the record where the
  // chain of valid parent references breaks, whose number lostAncestor
  // receives. Empty when the record is not kept or has no name.
  [[nodiscard]] std::wstring
      GetPath(ULONGLONG record,
              std::optional<ULONGLONG>* lost_ancestor = nullptr) const;
  // Same, through names[nameIndex], to reach each hard link of a file.
  [[nodiscard]] std::wstring
      GetPath(ULONGLONG record, size_t name_index,
              std::optional<ULONGLONG>* lost_ancestor = nullptr) const;
  // Files the change journal names that no kept record accounts for, ordered by
  // record number, then sequence number. Empty unless the volume was opened
  // with recover_errors and include_deleted both on.
  [[nodiscard]] std::span<const JournalOnlyEntry> JournalOnly() const noexcept;
  // Path of JournalOnly()[index]: its name, joined to the path of its parent
  // when parent_valid is true ("\dir\file"). Otherwise the bare name, which
  // is relative to the lost parent, whose number lostAncestor receives, as
  // GetPath() does. Empty when index is out of range.
  [[nodiscard]] std::wstring
      GetJournalPath(size_t index,
                     std::optional<ULONGLONG>* lost_ancestor = nullptr) const;
  // What the scan met, record slot by record slot.
  [[nodiscard]] const ScanStats& Stats() const noexcept;

 private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace Mft

}  // namespace NTFS_BROWSER_NAMESPACE
