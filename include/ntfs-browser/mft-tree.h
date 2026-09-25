#pragma once

#include <ntfs-browser/win-types.h>

#include <functional>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

#include <ntfs-browser/export.h>
#include <ntfs-browser/strategy.h>

namespace NtfsBrowser
{
template <Strategy S>
class NtfsVolume;

// One $FILE_NAME of an MFT record. A record has one per hard link, plus a
// DOS 8.3 alias when its long name needs one.
struct MftName
{
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
struct MftEntry
{
  ULONGLONG record{0};
  WORD sequence{0};
  bool in_use{false};
  bool directory{false};
  std::vector<MftName> names;
  // Real size of the unnamed $DATA stream. Without one, the $FILE_NAME size,
  // which NTFS only refreshes on a rename.
  ULONGLONG size{0};
  // From $STANDARD_INFORMATION, in local time.
  FILETIME create_time{};
  FILETIME write_time{};
  FILETIME access_time{};
  bool read_only{false};
  bool hidden{false};
  bool system{false};
  bool compressed{false};
  bool encrypted{false};
  bool sparse{false};
};

struct MftScanOptions
{
  // Called every few thousand records, and once at the end. Returning false
  // stops the scan: the tree then holds the records scanned so far.
  std::function<bool(ULONGLONG done, ULONGLONG total)> progress;
};

struct MftScanStats
{
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
  // Records with an attribute that failed to parse, counted either way.
  // Dropped unless the volume's recover_errors is on, which keeps what
  // parsed before the failure.
  ULONGLONG damaged{0};
  // Kept records that no chain of valid parent references links to the root.
  ULONGLONG unreachable{0};
  // False when progress() stopped the scan early.
  bool complete{true};
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
// build it from a NO_CACHE volume, since FULL_CACHE would also keep the whole
// $MFT in memory.
class NTFS_BROWSER_EXPORT MftTree
{
 public:
  // Scans every record of volume's $MFT.
  explicit MftTree(const NtfsVolume<Strategy::NO_CACHE>& volume,
                   const MftScanOptions& options = {});
  explicit MftTree(const NtfsVolume<Strategy::FULL_CACHE>& volume,
                   const MftScanOptions& options = {});

  // Every kept record, in record number order.
  [[nodiscard]] const std::vector<MftEntry>& Entries() const noexcept;
  // nullptr when the scan did not keep that record.
  [[nodiscard]] const MftEntry* Find(ULONGLONG record) const;
  // Records filed under dirRecord through a valid parent reference, once
  // each, even when several of their names sit in it.
  [[nodiscard]] std::span<const ULONGLONG> Children(ULONGLONG dirRecord) const;
  // Whether valid parent references lead from record up to the root.
  [[nodiscard]] bool IsReachable(ULONGLONG record) const;
  // Path of record through its first non-DOS name: "\dir\file" when it
  // reaches the root. Otherwise "dir\file", relative to the record where the
  // chain of valid parent references breaks, whose number lostAncestor
  // receives. Empty when the record is not kept or has no name.
  [[nodiscard]] std::wstring
      GetPath(ULONGLONG record,
              std::optional<ULONGLONG>* lostAncestor = nullptr) const;
  // Same, through names[nameIndex], to reach each hard link of a file.
  [[nodiscard]] std::wstring
      GetPath(ULONGLONG record, size_t nameIndex,
              std::optional<ULONGLONG>* lostAncestor = nullptr) const;
  // What the scan met, record slot by record slot.
  [[nodiscard]] const MftScanStats& Stats() const noexcept;

 private:
  std::vector<MftEntry> entries_;
  std::unordered_map<ULONGLONG, size_t> by_record_;
  std::unordered_map<ULONGLONG, std::vector<ULONGLONG>> children_;
  // Parallel to entries_.
  std::vector<bool> reachable_;
  MftScanStats stats_;

  template <Strategy S>
  void Scan(const NtfsVolume<S>& volume, const MftScanOptions& options);
  void Link();
  [[nodiscard]] bool IsValidParent(const MftEntry& child,
                                   const MftName& name) const;
  [[nodiscard]] std::wstring
      PathThrough(const MftEntry& entry, const MftName& name,
                  std::optional<ULONGLONG>* lostAncestor) const;
};

}  // namespace NtfsBrowser
