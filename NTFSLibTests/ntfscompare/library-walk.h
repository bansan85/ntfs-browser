#pragma once

#include <ntfs-browser/win-types.h>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <ntfs-browser/file-record.h>
#include <ntfs-browser/index-entry.h>
#include <ntfs-browser/mask.h>
#include <ntfs-browser/mft-idx.h>

#include "entry.h"
#include "time-convert.h"

namespace NtfsBrowser
{
class MftTree;
enum class Strategy : std::uint8_t;
template <Strategy S>
class NtfsVolume;
}  // namespace NtfsBrowser

namespace NtfsCompare
{

// Resolves relativePath (components separated by '/', matching the "/"-joined
// keys every Listing uses; empty means the volume's root) to the MFT record
// number of the directory it names, walking FindSubEntry() one component at
// a time from the root. FullCache, NoCache and MftTree each start their own
// walk from this same record, so all three compare the very same subtree.
template <NtfsBrowser::Strategy S>
[[nodiscard]] std::optional<ULONGLONG>
    ResolveDirectoryRecord(NtfsBrowser::NtfsVolume<S>& volume,
                           std::wstring_view relative_path)
{
  using NtfsBrowser::FileRecord;
  using NtfsBrowser::IndexEntry;
  using NtfsBrowser::Mask;

  FileRecord<S> current(volume);
  current.SetAttrMask(Mask::IndexRoot | Mask::IndexAllocation);
  if (!current.ParseFileRecord(
          static_cast<ULONGLONG>(NtfsBrowser::Enum::MftIdx::Root)) ||
      !current.ParseAttrs())
  {
    return std::nullopt;
  }

  auto record = static_cast<ULONGLONG>(NtfsBrowser::Enum::MftIdx::Root);
  size_t pos = 0;
  while (pos < relative_path.size())
  {
    const size_t next = relative_path.find(L'/', pos);
    const std::wstring_view component = relative_path.substr(
        pos,
        next == std::wstring_view::npos ? std::wstring_view::npos : next - pos);
    pos = (next == std::wstring_view::npos) ? relative_path.size() : next + 1;
    if (component.empty())
    {
      continue;
    }

    const std::optional<IndexEntry> entry = current.FindSubEntry(component);
    if (!entry || !entry->IsDirectory())
    {
      return std::nullopt;
    }
    record = entry->GetFileReference();

    if (!current.ParseFileRecord(record) || !current.ParseAttrs())
    {
      return std::nullopt;
    }
  }

  return record;
}

// Methods 4/5: recursively lists startRecord's subtree through
// FileRecord::TraverseSubEntries(), reading each entry's own $FILE_NAME
// (Filename/IndexEntry) for its fields.
template <NtfsBrowser::Strategy S>
[[nodiscard]] Listing WalkLibraryIndex(NtfsBrowser::NtfsVolume<S>& volume,
                                       ULONGLONG start_record)
{
  using NtfsBrowser::FileRecord;
  using NtfsBrowser::IndexEntryView;
  using NtfsBrowser::Mask;

  Listing result;

  struct Frame
  {
    ULONGLONG record = 0;
    std::wstring prefix;
  };
  struct CallbackContext
  {
    Listing* result;
    std::vector<Frame>* stack;
    const std::wstring* prefix;
  };

  std::vector<Frame> stack;
  stack.push_back({.record = start_record, .prefix = L""});

  while (!stack.empty())
  {
    const Frame frame = std::move(stack.back());
    stack.pop_back();

    FileRecord<S> dir(volume);
    dir.SetAttrMask(Mask::IndexRoot | Mask::IndexAllocation);
    if (!dir.ParseFileRecord(frame.record) || !dir.ParseAttrs())
    {
      continue;
    }

    CallbackContext ctx{
        .result = &result, .stack = &stack, .prefix = &frame.prefix};

    dir.TraverseSubEntries(
        [](const IndexEntryView& index_entry, void* context)
        {
          auto const* callback_context = static_cast<CallbackContext*>(context);

          // Skip system metafiles and the DOS 8.3 alias: the Win32 name is
          // this tool's path key everywhere.
          if (index_entry.GetFileReference() <
                  static_cast<ULONGLONG>(NtfsBrowser::Enum::MftIdx::User) ||
              !index_entry.IsWin32Name())
          {
            return;
          }
          const std::wstring_view name = index_entry.GetFilename();
          if (name.empty())
          {
            return;
          }

          Entry entry;
          entry.is_directory = index_entry.IsDirectory();
          entry.logical_size = index_entry.GetFileSize();
          entry.physical_size = index_entry.GetAllocatedSize();
          entry.read_only = index_entry.IsReadOnly();
          entry.hidden = index_entry.IsHidden();
          entry.system = index_entry.IsSystem();
          entry.archive = index_entry.IsArchive();
          entry.compressed = index_entry.IsCompressed();
          entry.encrypted = index_entry.IsEncrypted();
          entry.sparse = index_entry.IsSparse();

          FILETIME write_tm{};
          FILETIME create_tm{};
          FILETIME access_tm{};
          FILETIME change_tm{};
          index_entry.GetFileTime(&write_tm, &create_tm, &access_tm,
                                  &change_tm);
          entry.modification_time_utc = LibraryFiletimeToUtcTicks(write_tm);
          entry.creation_time_utc = LibraryFiletimeToUtcTicks(create_tm);
          entry.access_time_utc = LibraryFiletimeToUtcTicks(access_tm);
          entry.change_time_utc = LibraryFiletimeToUtcTicks(change_tm);

          const std::wstring path =
              callback_context->prefix->empty()
                  ? std::wstring(name)
                  : *callback_context->prefix + L"/" + std::wstring(name);
          const bool is_directory = entry.is_directory;
          const ULONGLONG child_record = index_entry.GetFileReference();
          callback_context->result->emplace(path, std::move(entry));

          if (is_directory)
          {
            callback_context->stack->push_back(
                {.record = child_record, .prefix = path});
          }
        },
        &ctx);
  }

  return result;
}

// Method 6: recursively lists startRecord's subtree through
// MftTree::Children(), reading each entry's MftEntry for its fields.
[[nodiscard]] Listing WalkMftTree(const NtfsBrowser::MftTree& tree,
                                  ULONGLONG start_record);

}  // namespace NtfsCompare
