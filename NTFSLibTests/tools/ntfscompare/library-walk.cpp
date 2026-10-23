#include "library-walk.h"

#include <ntfs-browser/win-types.h>

#include <span>

#include <ntfs-browser/mft/tree.h>
#include <ntfs-browser/namespace.h>

#include "ntfs-browser/index-entry.h"
#include "time-convert.h"

namespace NtfsCompare {

namespace {

// The fields of one MFT tree entry, with every timestamp in UTC.
Entry EntryFromMftEntry(
    const NTFS_BROWSER_NAMESPACE::Mft::MftTree::Entry& entry) {
  Entry out;
  out.is_directory = entry.directory;
  out.logical_size = entry.size;
  out.physical_size = entry.allocated_size;
  out.read_only = entry.read_only;
  out.hidden = entry.hidden;
  out.system = entry.system;
  out.archive = entry.archive;
  out.compressed = entry.compressed;
  out.encrypted = entry.encrypted;
  out.sparse = entry.sparse;
  out.modification_time_utc = LibraryFiletimeToUtcTicks(entry.write_time);
  out.creation_time_utc = LibraryFiletimeToUtcTicks(entry.create_time);
  out.access_time_utc = LibraryFiletimeToUtcTicks(entry.access_time);
  out.change_time_utc = LibraryFiletimeToUtcTicks(entry.change_time);
  return out;
}

// Adds every name of the child record that names frame's directory as its
// parent. A directory among them is queued on stack.
void AddChildNames(const NTFS_BROWSER_NAMESPACE::Mft::MftTree& tree,
                   ULONGLONG child, const WalkFrame& frame, Listing& result,
                   std::vector<WalkFrame>& stack) {
  const NTFS_BROWSER_NAMESPACE::Mft::MftTree::Entry* entry = tree.Find(child);
  if (entry == nullptr) {
    return;
  }

  for (const NTFS_BROWSER_NAMESPACE::Mft::MftTree::Name& name : entry->names) {
    if (name.dos_only || !name.parent_valid ||
        name.parent_record != frame.record) {
      continue;
    }

    const Entry out = EntryFromMftEntry(*entry);
    const std::wstring path =
        frame.prefix.empty() ? name.name : frame.prefix + L"/" + name.name;
    result.emplace(path, out);

    if (out.is_directory) {
      stack.push_back({.record = child, .prefix = path});
    }
  }
}

}  // namespace

Entry EntryFromIndexEntry(
    const NTFS_BROWSER_NAMESPACE::IndexEntryView& index_entry) {
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
  index_entry.GetFileTime(&write_tm, &create_tm, &access_tm, &change_tm);
  entry.modification_time_utc = LibraryFiletimeToUtcTicks(write_tm);
  entry.creation_time_utc = LibraryFiletimeToUtcTicks(create_tm);
  entry.access_time_utc = LibraryFiletimeToUtcTicks(access_tm);
  entry.change_time_utc = LibraryFiletimeToUtcTicks(change_tm);
  return entry;
}

void AddIndexEntry(const NTFS_BROWSER_NAMESPACE::IndexEntryView& index_entry,
                   const std::wstring& prefix, Listing& result,
                   std::vector<WalkFrame>& stack) {
  // Skip system metafiles and the DOS 8.3 alias: the Win32 name is
  // this tool's path key everywhere.
  if (index_entry.GetFileReference() <
          static_cast<ULONGLONG>(NTFS_BROWSER_NAMESPACE::Mft::Idx::User) ||
      !index_entry.IsWin32Name()) {
    return;
  }
  const std::wstring_view name = index_entry.GetFilename();
  if (name.empty()) {
    return;
  }

  const Entry entry = EntryFromIndexEntry(index_entry);
  const std::wstring path =
      prefix.empty() ? std::wstring(name) : prefix + L"/" + std::wstring(name);
  const ULONGLONG child_record = index_entry.GetFileReference();
  result.emplace(path, entry);

  if (entry.is_directory) {
    stack.push_back({.record = child_record, .prefix = path});
  }
}

Listing WalkMftTree(const NTFS_BROWSER_NAMESPACE::Mft::MftTree& tree,
                    ULONGLONG start_record) {
  Listing result;

  std::vector<WalkFrame> stack;
  stack.push_back({.record = start_record, .prefix = L""});

  while (!stack.empty()) {
    const WalkFrame frame = std::move(stack.back());
    stack.pop_back();

    for (const ULONGLONG child : tree.Children(frame.record)) {
      if (child <
          static_cast<ULONGLONG>(NTFS_BROWSER_NAMESPACE::Mft::Idx::User)) {
        continue;
      }
      AddChildNames(tree, child, frame, result, stack);
    }
  }

  return result;
}

}  // namespace NtfsCompare
