#include "attr/index-entry-walker.h"

#include <cstddef>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

#include <ntfs-browser/namespace.h>

#include "data/index-entry-flag.h"
#include "data/index-entry.h"
#include "index-entry.h"
#include "log/ntfs-common.h"
#include "win-types.h"

namespace NTFS_BROWSER_NAMESPACE::Attr {

IndexEntryWalker::IndexEntryWalker(
    const Messages& messages, bool recover,
    std::vector<IndexEntryView>& entries,
    std::vector<std::vector<BYTE>>& realigned) noexcept
    : messages_(messages),
      recover_(recover),
      entries_(entries),
      realigned_(realigned) {}

bool IndexEntryWalker::RejectOnDefect(std::string_view defect) {
  Log::Recoverable(recover_, "{}", defect);
  if (recover_) {
    return false;
  }
  entries_.clear();
  return true;
}

std::optional<Data::IndexEntry> IndexEntryWalker::BoundedHeader(
    std::span<const BYTE> cur) {
  if (cur.size() < offsetof(Data::IndexEntry, stream)) {
    rejected_ = RejectOnDefect(messages_.header_exceeds);
    return std::nullopt;
  }
  const Data::IndexEntry head = Data::ReadIndexEntryHeader(cur);
  if (head.size == 0 || head.size > cur.size()) {
    rejected_ = RejectOnDefect(messages_.entry_exceeds);
    return std::nullopt;
  }

  total_ += head.size;
  if (total_ > declared_total_) {
    rejected_ = RejectOnDefect(messages_.total_exceeds);
    return std::nullopt;
  }
  return head;
}

bool IndexEntryWalker::Append(std::span<const BYTE> cur,
                              const Data::IndexEntry& head) {
  const Data::IndexEntry& aligned_index_entry =
      Data::AlignIndexEntry(realigned_, cur, head.size);
  if (const std::optional<std::string_view> defect =
          Data::ValidateIndexEntry(aligned_index_entry)) {
    if (RejectOnDefect(*defect)) {
      return false;
    }
    clean_ = false;
  }

  entries_.emplace_back(aligned_index_entry);
  return true;
}

IndexEntryWalker::Outcome IndexEntryWalker::Walk(std::span<const BYTE> first,
                                                 DWORD declared_total) {
  declared_total_ = declared_total;
  total_ = 0;
  clean_ = true;
  rejected_ = false;

  std::span<const BYTE> cur = first;
  while (true) {
    const std::optional<Data::IndexEntry> head = BoundedHeader(cur);
    if (!head) {
      return {.accepted = !rejected_, .clean_end = std::nullopt};
    }
    if (!Append(cur, *head)) {
      return {.accepted = false, .clean_end = std::nullopt};
    }

    if ((head->flags & Data::IndexEntryFlag::Last) ==
        Data::IndexEntryFlag::Last) {
      Log::Trace("Last Index Entry");
      if (!clean_) {
        return {.accepted = true, .clean_end = std::nullopt};
      }
      return {.accepted = true,
              .clean_end = (first.size() - cur.size()) + head->size};
    }

    cur = cur.subspan(head->size);  // Pick next
  }
}

}  // namespace NTFS_BROWSER_NAMESPACE::Attr
