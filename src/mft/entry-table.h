#pragma once

#include <ntfs-browser/win-types.h>

#include <cstddef>
#include <optional>
#include <unordered_map>
#include <vector>

#include <ntfs-browser/mft/idx.h>
#include <ntfs-browser/mft/tree.h>
#include <ntfs-browser/namespace.h>

namespace NTFS_BROWSER_NAMESPACE::Mft {

// The root directory's record number, fixed by NTFS.
inline constexpr ULONGLONG root_record = static_cast<ULONGLONG>(Mft::Idx::Root);

// The entries a scan kept, and the way to reach one by its record number. The
// scan fills the entries, then calls IndexAll(). Until then, no lookup finds
// anything.
class EntryTable {
 public:
  // The entries, in the order of their records. The scan appends to them.
  [[nodiscard]] std::vector<Mft::MftTree::Entry>& Entries() noexcept;
  [[nodiscard]] const std::vector<Mft::MftTree::Entry>&
      Entries() const noexcept;

  // Records where each entry sits, for the lookups below.
  void IndexAll();

  // Index in Entries() of record, or nothing when the scan did not keep it.
  [[nodiscard]] std::optional<size_t> IndexOf(ULONGLONG record) const;

  // The entry of record, or null when the scan did not keep it.
  [[nodiscard]] const Mft::MftTree::Entry* Find(ULONGLONG record) const;

  // Whether name's parent reference, read from child's record, names a
  // directory the scan found. See the rules in mft/tree.h.
  [[nodiscard]] bool IsValidParent(const Mft::MftTree::Entry& child,
                                   const Mft::MftTree::Name& name) const;

  // Whether a parent reference names a directory the scan found, under the
  // same rules as IsValidParent(), minus the case of a record filed under
  // itself.
  [[nodiscard]] bool NamesDirectory(ULONGLONG parent_record,
                                    WORD parent_sequence) const;

 private:
  // Records that Entries()[index] holds record.
  void Index(ULONGLONG record, size_t index);

  std::vector<Mft::MftTree::Entry> entries_;
  // Index in entries_ of each kept record, by record number, no_entry for the
  // others. It ends after the last record it holds. A record that would grow
  // it past what the entries justify sits in sparse_entries_ instead.
  std::vector<size_t> entry_of_record_;
  std::unordered_map<ULONGLONG, size_t> sparse_entries_;
};

}  // namespace NTFS_BROWSER_NAMESPACE::Mft
