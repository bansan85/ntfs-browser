#pragma once

#include <ntfs-browser/win-types.h>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <ntfs-browser/attr/mask.h>
#include <ntfs-browser/index-entry.h>
#include <ntfs-browser/io/file-record.h>
#include <ntfs-browser/mft/idx.h>
#include <ntfs-browser/mft/tree.h>

#include "entry.h"
#include "time-convert.h"

namespace NtfsBrowser {

namespace Cache {

enum class Strategy : std::uint8_t;

}  // namespace Cache
template <Cache::Strategy S>
class NtfsVolume;

}  // namespace NtfsBrowser

namespace NtfsCompare {

// Resolves relativePath (components separated by '/', matching the "/"-joined
// keys every Listing uses; empty means the volume's root) to the MFT record
// number of the directory it names, walking FindSubEntry() one component at
// a time from the root. FullCache, NoCache and MftTree each start their own
// walk from this same record, so all three compare the very same subtree.
template <NtfsBrowser::Cache::Strategy S>
[[nodiscard]] std::optional<ULONGLONG>
    ResolveDirectoryRecord(NtfsBrowser::NtfsVolume<S>& volume,
                           std::wstring_view relative_path) {
  using NtfsBrowser::IndexEntry;
  using NtfsBrowser::Io::FileRecord;

  NtfsBrowser::Io::FileRecord<S> current(volume);
  current.SetAttrMask(NtfsBrowser::Attr::Mask::IndexRoot |
                      NtfsBrowser::Attr::Mask::IndexAllocation);
  if (!current.ParseFileRecord(
          static_cast<ULONGLONG>(NtfsBrowser::Mft::Idx::Root)) ||
      !current.ParseAttrs()) {
    return std::nullopt;
  }

  auto record = static_cast<ULONGLONG>(NtfsBrowser::Mft::Idx::Root);
  size_t pos = 0;
  while (pos < relative_path.size()) {
    const size_t next = relative_path.find(L'/', pos);
    const std::wstring_view component = relative_path.substr(
        pos,
        next == std::wstring_view::npos ? std::wstring_view::npos : next - pos);
    pos = (next == std::wstring_view::npos) ? relative_path.size() : next + 1;
    if (component.empty()) {
      continue;
    }

    const std::optional<IndexEntry> entry = current.FindSubEntry(component);
    if (!entry || !entry->IsDirectory()) {
      return std::nullopt;
    }
    record = entry->GetFileReference();

    if (!current.ParseFileRecord(record) || !current.ParseAttrs()) {
      return std::nullopt;
    }
  }

  return record;
}

// A directory still to list: its record number, and the key its entries are
// listed under.
struct WalkFrame {
  ULONGLONG record = 0;
  std::wstring prefix;
};

// The fields of one directory index entry, with every timestamp in UTC.
[[nodiscard]] Entry
    EntryFromIndexEntry(const NtfsBrowser::IndexEntryView& index_entry);

// Adds index_entry, found in the directory keyed prefix, to result. A
// directory is queued on stack to be listed in turn. System metafiles and DOS
// alias names are skipped.
void AddIndexEntry(const NtfsBrowser::IndexEntryView& index_entry,
                   const std::wstring& prefix, Listing& result,
                   std::vector<WalkFrame>& stack);

// Methods 4/5: recursively lists startRecord's subtree through
// FileRecord::TraverseSubEntries(), reading each entry's own $FILE_NAME
// (Filename/IndexEntry) for its fields.
template <NtfsBrowser::Cache::Strategy S>
[[nodiscard]] Listing WalkLibraryIndex(NtfsBrowser::NtfsVolume<S>& volume,
                                       ULONGLONG start_record) {
  using NtfsBrowser::IndexEntryView;
  using NtfsBrowser::Io::FileRecord;

  Listing result;

  struct CallbackContext {
    Listing* result;
    std::vector<WalkFrame>* stack;
    const std::wstring* prefix;
  };

  std::vector<WalkFrame> stack;
  stack.push_back({.record = start_record, .prefix = L""});

  while (!stack.empty()) {
    const WalkFrame frame = std::move(stack.back());
    stack.pop_back();

    NtfsBrowser::Io::FileRecord<S> dir(volume);
    dir.SetAttrMask(NtfsBrowser::Attr::Mask::IndexRoot |
                    NtfsBrowser::Attr::Mask::IndexAllocation);
    if (!dir.ParseFileRecord(frame.record) || !dir.ParseAttrs()) {
      continue;
    }

    CallbackContext ctx{
        .result = &result, .stack = &stack, .prefix = &frame.prefix};

    dir.TraverseSubEntries(
        [](const IndexEntryView& index_entry, void* context) {
          const auto* callback_context = static_cast<CallbackContext*>(context);
          AddIndexEntry(index_entry, *callback_context->prefix,
                        *callback_context->result, *callback_context->stack);
        },
        &ctx);
  }

  return result;
}

// Method 6: recursively lists startRecord's subtree through
// MftTree::Children(), reading each entry's MftTree::Entry for its fields.
[[nodiscard]] Listing WalkMftTree(const NtfsBrowser::Mft::MftTree& tree,
                                  ULONGLONG start_record);

}  // namespace NtfsCompare
