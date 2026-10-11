#include "library-walk.h"

#include <ntfs-browser/win-types.h>

#include <span>

#include <ntfs-browser/mft/idx.h>
#include <ntfs-browser/mft/tree.h>

namespace NtfsCompare {

Listing WalkMftTree(const NtfsBrowser::Mft::MftTree& tree,
                    ULONGLONG start_record) {
  using NtfsBrowser::Mft::MftTree;

  Listing result;

  struct Frame {
    ULONGLONG record;
    std::wstring prefix;
  };

  std::vector<Frame> stack;
  stack.push_back({.record = start_record, .prefix = L""});

  while (!stack.empty()) {
    const Frame frame = std::move(stack.back());
    stack.pop_back();

    for (const ULONGLONG child : tree.Children(frame.record)) {
      if (child < static_cast<ULONGLONG>(NtfsBrowser::Mft::Idx::User)) {
        continue;
      }
      const NtfsBrowser::Mft::MftTree::Entry* entry = tree.Find(child);
      if (entry == nullptr) {
        continue;
      }

      for (const NtfsBrowser::Mft::MftTree::Name& name : entry->names) {
        if (name.dos_only || !name.parent_valid ||
            name.parent_record != frame.record) {
          continue;
        }

        Entry out;
        out.is_directory = entry->directory;
        out.logical_size = entry->size;
        out.physical_size = entry->allocated_size;
        out.read_only = entry->read_only;
        out.hidden = entry->hidden;
        out.system = entry->system;
        out.archive = entry->archive;
        out.compressed = entry->compressed;
        out.encrypted = entry->encrypted;
        out.sparse = entry->sparse;
        out.modification_time_utc =
            LibraryFiletimeToUtcTicks(entry->write_time);
        out.creation_time_utc = LibraryFiletimeToUtcTicks(entry->create_time);
        out.access_time_utc = LibraryFiletimeToUtcTicks(entry->access_time);
        out.change_time_utc = LibraryFiletimeToUtcTicks(entry->change_time);

        const std::wstring path =
            frame.prefix.empty() ? name.name : frame.prefix + L"/" + name.name;
        const bool is_directory = out.is_directory;
        result.emplace(path, out);

        if (is_directory) {
          stack.push_back({.record = child, .prefix = path});
        }
      }
    }
  }

  return result;
}

}  // namespace NtfsCompare
