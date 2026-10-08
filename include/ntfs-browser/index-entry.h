#pragma once

#include <optional>
#include <vector>

#include <ntfs-browser/export.h>
#include <ntfs-browser/filename.h>

namespace NtfsBrowser {
namespace Data {

struct IndexEntry;

}  // namespace Data

// A read-only window on one index entry. It does not own the bytes it reads:
// they belong to the IndexBlock or AttrIndexRoot it came from, and the view
// MUST NOT outlive it. A traversal callback receives one. Convert it to an
// IndexEntry to keep it.
class NTFS_BROWSER_EXPORT IndexEntryView : public Filename {
 public:
  explicit IndexEntryView(const Data::IndexEntry& index_entry);
  IndexEntryView(IndexEntryView&& other) noexcept = default;
  IndexEntryView(const IndexEntryView& other) = default;
  IndexEntryView& operator=(IndexEntryView&& other) noexcept = delete;
  IndexEntryView& operator=(const IndexEntryView& other) = delete;
  ~IndexEntryView() override = default;

  friend class IndexEntry;

 private:
  const Data::IndexEntry* index_entry_;

  // Points the view, and the file name it decoded, at another copy of the
  // same bytes.
  void Rebind(const Data::IndexEntry& index_entry);

 public:
  [[nodiscard]] ULONGLONG GetFileReference() const noexcept;
  // Times the record this entry names has been reused, read from the
  // entry's own mft_sn field - not the live record's current sequence,
  // which may already differ.
  [[nodiscard]] WORD GetSequenceNumber() const noexcept;
  [[nodiscard]] bool IsSubNodePtr() const noexcept;
  [[nodiscard]] ULONGLONG GetSubNodeVCN() const noexcept;
};  // IndexEntryView

// An index entry that owns its bytes: the entry alone, not the block it was
// read from. It stays valid after that block, or the FileRecord, is gone.
class NTFS_BROWSER_EXPORT IndexEntry : public IndexEntryView {
 public:
  explicit IndexEntry(const IndexEntryView& view);
  IndexEntry(IndexEntry&& other) noexcept = default;
  IndexEntry(const IndexEntry& other);
  IndexEntry& operator=(IndexEntry&& other) noexcept = delete;
  IndexEntry& operator=(const IndexEntry& other) = delete;
  ~IndexEntry() override = default;

 private:
  std::vector<BYTE> bytes_;
};  // IndexEntry

}  // namespace NtfsBrowser
