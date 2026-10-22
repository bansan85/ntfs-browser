#pragma once

#include <ntfs-browser/win-types.h>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>
#include <unordered_set>
#include <vector>

#include <ntfs-browser/io/file-record.h>
#include <ntfs-browser/namespace.h>
#include <ntfs-browser/record/carved-name.h>

namespace NTFS_BROWSER_NAMESPACE {

namespace Cache {

enum class Strategy : std::uint8_t;

}  // namespace Cache

}  // namespace NTFS_BROWSER_NAMESPACE

namespace NTFS_BROWSER_NAMESPACE {
namespace Attr {

template <Cache::Strategy S>
class AttrBase;

}  // namespace Attr

namespace UpCase {

class Table;

}  // namespace UpCase
}  // namespace NTFS_BROWSER_NAMESPACE

namespace NTFS_BROWSER_NAMESPACE {

class IndexEntry;
class IndexEntryView;

namespace Attr {

template <Cache::Strategy S>
class AttrIndexAlloc;

}  // namespace Attr

namespace Io {

// How the recovery scan walks one $INDEX_ALLOCATION attribute.
struct OrphanScanPlan {
  // VCN step from one index block to the next.
  ULONGLONG clusters_per_block;
  // Blocks the attribute declares.
  ULONGLONG declared_blocks;
  // Blocks the scan visits.
  ULONGLONG scan_limit;
  // True when the scan visits fewer blocks than declared.
  bool capped;
};

// One name lookup down a B+ tree: the name, the volume's collation, and the
// index block VCNs the walk has already visited.
struct IndexSearch {
  std::wstring_view file_name;
  const UpCase::Table& upcase;
  std::unordered_set<ULONGLONG> visited_vcns;
};

// Walks the B+ tree of one directory: its $INDEX_ROOT and $INDEX_ALLOCATION
// blocks. Short-lived: built over a FileRecord for one call, never stored.
template <Cache::Strategy S>
class DirectoryIndex {
 public:
  using SubentryCallback = typename FileRecord<S>::SubentryCallback;

  explicit DirectoryIndex(const FileRecord<S>& record) noexcept;

  // Calls se_call_back for every named entry, in collation order, then for
  // the orphaned blocks when recovery is on.
  void Traverse(const SubentryCallback& se_call_back, void* context) const;
  // Finds file_name. upcase is the volume's collation, loaded by the caller.
  [[nodiscard]] std::optional<IndexEntry>
      Find(std::wstring_view file_name, const UpCase::Table& upcase) const;
  // The names the unused space of the $INDEX_ALLOCATION blocks still holds,
  // see FileRecord<S>::CarveIndexSlackNames(). Times are UTC, as stored.
  [[nodiscard]] std::vector<Record::CarvedName> CarveSlackNames() const;

 private:
  // The record whose index this walks. It MUST outlive this object.
  const FileRecord<S>& record_;

  [[nodiscard]] static const std::vector<IndexEntryView>*
      FileNameIndexRootEntries(const Attr::AttrBase<S>& attr);
  [[nodiscard]] std::optional<IndexEntry>
      FindSubEntryInOrder(std::wstring_view file_name,
                          const UpCase::Table& upcase) const;
  [[nodiscard]] std::optional<IndexEntry>
      VisitIndexBlock(ULONGLONG vcn, IndexSearch& search, size_t depth) const;
  [[nodiscard]] std::optional<IndexEntry>
      SearchIndexEntries(const std::vector<IndexEntryView>& entries,
                         IndexSearch& search, size_t child_depth,
                         bool at_root) const;
  void TraverseSubNode(ULONGLONG vcn, const SubentryCallback& se_call_back,
                       void* context,
                       std::unordered_set<ULONGLONG>& visited_vcns,
                       size_t depth) const;
  void ScanOrphanedIndexBlocks(
      const SubentryCallback& se_call_back, void* context,
      std::unordered_set<ULONGLONG>& visited_vcns) const;
  [[nodiscard]] OrphanScanPlan
      PlanOrphanScan(const Attr::AttrIndexAlloc<S>& alloc) const;
  void ReportOrphanedBlock(Attr::AttrIndexAlloc<S>& alloc, ULONGLONG vcn,
                           const SubentryCallback& se_call_back,
                           void* context) const;
};

}  // namespace Io

}  // namespace NTFS_BROWSER_NAMESPACE
