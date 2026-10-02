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
// a time from the root. FULL_CACHE, NO_CACHE and MftTree each start their own
// walk from this same record, so all three compare the very same subtree.
template <NtfsBrowser::Strategy S>
[[nodiscard]] std::optional<ULONGLONG>
    ResolveDirectoryRecord(NtfsBrowser::NtfsVolume<S>& volume,
                           std::wstring_view relativePath)
{
  using namespace NtfsBrowser;

  FileRecord<S> current(volume);
  current.SetAttrMask(Mask::INDEX_ROOT | Mask::INDEX_ALLOCATION);
  if (!current.ParseFileRecord(static_cast<ULONGLONG>(Enum::MftIdx::ROOT)) ||
      !current.ParseAttrs())
  {
    return std::nullopt;
  }

  ULONGLONG record = static_cast<ULONGLONG>(Enum::MftIdx::ROOT);
  size_t pos = 0;
  while (pos < relativePath.size())
  {
    const size_t next = relativePath.find(L'/', pos);
    const std::wstring_view component = relativePath.substr(
        pos,
        next == std::wstring_view::npos ? std::wstring_view::npos : next - pos);
    pos = (next == std::wstring_view::npos) ? relativePath.size() : next + 1;
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
                                       ULONGLONG startRecord)
{
  using namespace NtfsBrowser;

  Listing result;

  struct Frame
  {
    ULONGLONG record;
    std::wstring prefix;
  };
  struct CallbackContext
  {
    Listing* result;
    std::vector<Frame>* stack;
    const std::wstring* prefix;
  };

  std::vector<Frame> stack;
  stack.push_back({.record = startRecord, .prefix = L""});

  while (!stack.empty())
  {
    const Frame frame = std::move(stack.back());
    stack.pop_back();

    FileRecord<S> dir(volume);
    dir.SetAttrMask(Mask::INDEX_ROOT | Mask::INDEX_ALLOCATION);
    if (!dir.ParseFileRecord(frame.record) || !dir.ParseAttrs())
    {
      continue;
    }

    CallbackContext ctx{
        .result = &result, .stack = &stack, .prefix = &frame.prefix};

    dir.TraverseSubEntries(
        [](const IndexEntry& ie, void* context)
        {
          auto const* c = static_cast<CallbackContext*>(context);

          // Skip system metafiles and the DOS 8.3 alias: the Win32 name is
          // this tool's path key everywhere.
          if (ie.GetFileReference() <
                  static_cast<ULONGLONG>(Enum::MftIdx::USER) ||
              !ie.IsWin32Name())
          {
            return;
          }
          const std::wstring_view name = ie.GetFilename();
          if (name.empty())
          {
            return;
          }

          Entry entry;
          entry.is_directory = ie.IsDirectory();
          entry.logical_size = ie.GetFileSize();
          entry.physical_size = ie.GetAllocatedSize();
          entry.read_only = ie.IsReadOnly();
          entry.hidden = ie.IsHidden();
          entry.system = ie.IsSystem();
          entry.archive = ie.IsArchive();
          entry.compressed = ie.IsCompressed();
          entry.encrypted = ie.IsEncrypted();
          entry.sparse = ie.IsSparse();

          FILETIME writeTm{};
          FILETIME createTm{};
          FILETIME accessTm{};
          FILETIME changeTm{};
          ie.GetFileTime(&writeTm, &createTm, &accessTm, &changeTm);
          entry.modification_time_utc = LibraryFiletimeToUtcTicks(writeTm);
          entry.creation_time_utc = LibraryFiletimeToUtcTicks(createTm);
          entry.access_time_utc = LibraryFiletimeToUtcTicks(accessTm);
          entry.change_time_utc = LibraryFiletimeToUtcTicks(changeTm);

          const std::wstring path =
              c->prefix->empty() ? std::wstring(name)
                                 : *c->prefix + L"/" + std::wstring(name);
          const bool isDirectory = entry.is_directory;
          const ULONGLONG childRecord = ie.GetFileReference();
          c->result->emplace(path, std::move(entry));

          if (isDirectory)
          {
            c->stack->push_back({.record = childRecord, .prefix = path});
          }
        },
        &ctx);
  }

  return result;
}

// Method 6: recursively lists startRecord's subtree through
// MftTree::Children(), reading each entry's MftEntry for its fields.
[[nodiscard]] Listing WalkMftTree(const NtfsBrowser::MftTree& tree,
                                  ULONGLONG startRecord);

}  // namespace NtfsCompare
