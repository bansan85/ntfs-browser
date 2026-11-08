#include "root-index-edit.h"

#include <ntfs-browser/win-types.h>

#include <cstddef>
#include <vector>

#include <ntfs-browser/namespace.h>

#include "data/filename-namespace.h"
#include "data/filename.h"
#include "data/index-entry-flag.h"
#include "data/index-entry.h"
#include "data/index-root.h"
#include "record-edit.h"

namespace NTFS_BROWSER_NAMESPACE::Tool::FuzzGen {

namespace {

// The root directory record.
constexpr ULONGLONG root_record = 5;

// Sizes in the hand-made index: the fixed part of an entry, the fixed part of
// a $FILE_NAME before its name, the VCN a sub-node entry ends with, and the
// two headers that open an $INDEX_ROOT value.
constexpr WORD entry_header_bytes = 16;
constexpr WORD file_name_header_bytes = 66;
constexpr WORD sub_node_vcn_bytes = 8;
constexpr WORD root_header_bytes = 32;

// A leaf entry for a file named "AB", with the number of its record, and the
// directory that holds it.
constexpr ULONGLONG ab_record = 0x32;
constexpr ULONGLONG ab_sequence = 1;
constexpr ULONGLONG ab_parent = 5;
constexpr WORD ab_name_length = 2;

// The Win32 name space, and the code units of the name "AB".
constexpr BYTE win32_name_space = 1;
constexpr WORD unit_a = 0x41;
constexpr WORD unit_b = 0x42;

}  // namespace

std::size_t RootIndexEdit::Put(std::vector<BYTE>& image, const Spec& spec) {
  const WORD ab_bytes =
      spec.with_ab_entry
          ? static_cast<WORD>(entry_header_bytes + file_name_header_bytes +
                              (2 * ab_name_length))
          : WORD{0};
  const WORD end_bytes = static_cast<WORD>(
      entry_header_bytes + (spec.sub_node_vcn ? sub_node_vcn_bytes : 0));
  const DWORD entries_bytes = static_cast<DWORD>(ab_bytes + end_bytes);
  if (!spec.keep_attributes) {
    RecordEdit::ClearAttributes(image, root_record);
  }
  const std::size_t attr = RecordEdit::AppendResident(
      image, root_record, RecordEdit::type_index_root, 0,
      std::vector<BYTE>(root_header_bytes + entries_bytes, 0));
  const std::size_t value = RecordEdit::ValueOffset(image, attr);
  RecordEdit::Edit<Data::IndexRoot>(image, value, [&](auto& header) {
    header.attr_type = static_cast<Attr::Type>(RecordEdit::type_file_name);
    header.ib_size = spec.block_size;
    header.clusters_per_ib = spec.clusters_per_block;
    header.entry_offset = entry_header_bytes;
    header.total_entry_size = entries_bytes;
    header.alloc_entry_size = entries_bytes;
  });
  const std::size_t first = value + root_header_bytes;
  if (spec.with_ab_entry) {
    RecordEdit::Edit<Data::IndexEntry>(image, first, [&](auto& entry) {
      entry.mft_index = ab_record;
      entry.mft_sn = ab_sequence;
      entry.size = ab_bytes;
      entry.stream_size =
          static_cast<WORD>(file_name_header_bytes + (2 * ab_name_length));
    });
    RecordEdit::Edit<Data::Filename>(
        image, first + entry_header_bytes, [](auto& name) {
          name.parent_ref = ab_parent;
          name.name_length = static_cast<BYTE>(ab_name_length);
          name.name_space =
              static_cast<Data::FilenameNamespace>(win32_name_space);
        });
    RecordEdit::StoreWord(
        image, first + entry_header_bytes + file_name_header_bytes, unit_a);
    RecordEdit::StoreWord(
        image, first + entry_header_bytes + file_name_header_bytes + 2, unit_b);
  }
  RecordEdit::Edit<Data::IndexEntry>(image, first + ab_bytes, [&](auto& entry) {
    entry.size = end_bytes;
    entry.flags = Data::IndexEntryFlag::Last;
    if (spec.sub_node_vcn) {
      entry.flags |= Data::IndexEntryFlag::SubNode;
    }
  });
  if (spec.sub_node_vcn) {
    RecordEdit::StoreQword(image, first + ab_bytes + entry_header_bytes,
                           *spec.sub_node_vcn);
  }
  return first;
}

void RootIndexEdit::PutAllocation(std::vector<BYTE>& image,
                                  const std::vector<BYTE>& runs,
                                  ULONGLONG last_vcn, ULONGLONG size) {
  RecordEdit::AppendNonResident(image, root_record,
                                RecordEdit::type_index_allocation, runs,
                                last_vcn, size, size);
}

}  // namespace NTFS_BROWSER_NAMESPACE::Tool::FuzzGen
