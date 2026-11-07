#include <ntfs-browser/win-types.h>

#include <sstream>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

#include <ntfs-browser/attr/mask.h>
#include <ntfs-browser/cache/strategy.h>
#include <ntfs-browser/index-entry.h>
#include <ntfs-browser/io/file-record.h>
#include <ntfs-browser/io/record-standing.h>
#include <ntfs-browser/mft/idx.h>
#include <ntfs-browser/namespace.h>
#include <ntfs-browser/ntfs-volume.h>
#include <ntfs-browser/test/support/test-log-sink.h>
#include <ntfs-browser/test/support/walk-comparison.h>

namespace NTFS_BROWSER_NAMESPACE::Test::Support {

namespace {

// What the log of a disk read says: the batched walk makes other reads than the
// default one, so a test leaves these lines out when it compares logs.
constexpr std::string_view read_line_marker = "Successfully read";

// Appends the lines the library logged since the last call to log, but the
// lines about disk reads.
void TakeLogLines(std::string& log) {
  std::istringstream lines(TakeCapturedLog());
  std::string line;
  while (std::getline(lines, line)) {
    if (line.find(read_line_marker) == std::string::npos) {
      log += line;
      log += '\n';
    }
  }
}

// The standing of a record as a caller of TraverseSubEntries() works it out:
// it parses the record the entry names itself.
template <NTFS_BROWSER_NAMESPACE::Cache::Strategy S>
NTFS_BROWSER_NAMESPACE::Io::RecordStanding
    ClassifyByProbe(NTFS_BROWSER_NAMESPACE::Io::FileRecord<S>& probe,
                    const WalkedEntry& entry) {
  if (!probe.ParseFileRecord(entry.reference)) {
    return NTFS_BROWSER_NAMESPACE::Io::RecordStanding::Unreadable;
  }
  if (probe.IsDeleted()) {
    return NTFS_BROWSER_NAMESPACE::Io::RecordStanding::Deleted;
  }
  // A reference that carries sequence 0 claims no generation.
  const bool reused =
      entry.sequence != 0 && entry.sequence != probe.GetSequenceNumber();
  return reused ? NTFS_BROWSER_NAMESPACE::Io::RecordStanding::Reused
                : NTFS_BROWSER_NAMESPACE::Io::RecordStanding::Live;
}

// Where a callback of the walk puts what it is given.
struct Collected {
  std::vector<WalkedEntry>* entries;
};

WalkedEntry EntryOf(const IndexEntryView& view,
                    NTFS_BROWSER_NAMESPACE::Io::RecordStanding standing) {
  return {.name = std::wstring(view.GetFilename()),
          .reference = view.GetFileReference(),
          .sequence = view.GetSequenceNumber(),
          .directory = view.IsDirectory(),
          .standing = standing};
}

}  // namespace

template <NTFS_BROWSER_NAMESPACE::Cache::Strategy S>
WalkResult WalkVolume(const NtfsVolume<S>& volume, bool batched) {
  NTFS_BROWSER_NAMESPACE::Io::FileRecord<S> dir(volume);
  dir.SetAttrMask(NTFS_BROWSER_NAMESPACE::Attr::Mask::IndexRoot |
                  NTFS_BROWSER_NAMESPACE::Attr::Mask::IndexAllocation);
  NTFS_BROWSER_NAMESPACE::Io::FileRecord<S> probe(volume);

  WalkResult result;
  std::vector<ULONGLONG> pending{
      static_cast<ULONGLONG>(NTFS_BROWSER_NAMESPACE::Mft::Idx::Root)};
  std::unordered_set<ULONGLONG> seen;
  (void)TakeCapturedLog();
  while (!pending.empty()) {
    const ULONGLONG reference = pending.back();
    pending.pop_back();
    if (!seen.insert(reference).second || !dir.ParseFileRecord(reference) ||
        !dir.ParseAttrs()) {
      continue;
    }

    std::vector<WalkedEntry> listed;
    Collected collected{&listed};
    if (batched) {
      dir.TraverseSubEntriesWithStanding(
          [](const IndexEntryView& view,
             NTFS_BROWSER_NAMESPACE::Io::RecordStanding standing,
             void* context) {
            static_cast<Collected*>(context)->entries->push_back(
                EntryOf(view, standing));
          },
          &collected);
    } else {
      dir.TraverseSubEntries(
          [](const IndexEntryView& view, void* context) {
            static_cast<Collected*>(context)->entries->push_back(EntryOf(
                view, NTFS_BROWSER_NAMESPACE::Io::RecordStanding::Live));
          },
          &collected);
    }
    TakeLogLines(result.log);

    // The directories below this one, in the order the entries came.
    std::vector<ULONGLONG> below;
    for (WalkedEntry& entry : listed) {
      if (!batched) {
        entry.standing = ClassifyByProbe(probe, entry);
      }
      if (entry.directory &&
          entry.reference >=
              static_cast<ULONGLONG>(NTFS_BROWSER_NAMESPACE::Mft::Idx::User) &&
          entry.standing !=
              NTFS_BROWSER_NAMESPACE::Io::RecordStanding::Reused) {
        below.push_back(entry.reference);
      }
      result.entries.push_back(std::move(entry));
    }
    pending.insert(pending.end(), below.rbegin(), below.rend());
    // The probe logged lines of its own: they are not the walk's.
    (void)TakeCapturedLog();
  }
  return result;
}

template WalkResult
    WalkVolume<NTFS_BROWSER_NAMESPACE::Cache::Strategy::NoCache>(
        const NtfsVolume<NTFS_BROWSER_NAMESPACE::Cache::Strategy::NoCache>&
            volume,
        bool batched);
template WalkResult
    WalkVolume<NTFS_BROWSER_NAMESPACE::Cache::Strategy::FullCache>(
        const NtfsVolume<NTFS_BROWSER_NAMESPACE::Cache::Strategy::FullCache>&
            volume,
        bool batched);

}  // namespace NTFS_BROWSER_NAMESPACE::Test::Support
