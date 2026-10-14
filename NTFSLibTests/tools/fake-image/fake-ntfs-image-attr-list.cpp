#include <array>
#include <cstring>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

#include <ntfs-browser/mft/idx.h>

#include "data/attribute-list.h"
#include "data/filename.h"
#include "data/header-non-resident.h"
#include "data/header-resident.h"
#include "data/index-entry-flag.h"
#include "data/index-root.h"
#include "fake-ntfs-image-internal.h"

namespace NtfsBrowserTests {

namespace {

// Directory whose only attribute is a resident $ATTRIBUTE_LIST relocating
// $INDEX_ROOT to the extension record index_extension_idx, reproducing a
// directory that outgrew its base record (eg. C:\Windows).
FakeRecord MakeAttributeListOnlyDirRecord() {
  FakeRecord record = MakeRecordHeader(
      attr_offset_value, NtfsBrowser::Data::FileRecordFlag::InUse |
                             NtfsBrowser::Data::FileRecordFlag::Dir);

  auto& attr = *reinterpret_cast<NtfsBrowser::Data::HeaderResident*>(
      &record.at(attr_offset_value));
  attr.header.type = Attr::Type::AttributeList;
  attr.header.non_resident = 0;
  attr.header.name_length = 0;
  attr.header.flags = 0;
  attr.header.id = 0;
  attr.attr_size =
      static_cast<DWORD>(NtfsBrowser::Data::attribute_list_entry_header_size);
  attr.attr_offset = static_cast<WORD>(sizeof(attr));
  attr.header.total_size = AlignAttrSize(sizeof(attr) + attr.attr_size);

  auto& al_entry = *reinterpret_cast<NtfsBrowser::Data::AttributeList*>(
      &record.at(attr_offset_value + attr.attr_offset));
  al_entry.attr_type = Attr::Type::IndexRoot;
  al_entry.record_size =
      static_cast<WORD>(NtfsBrowser::Data::attribute_list_entry_header_size);
  al_entry.name_length = 0;
  al_entry.name_offset = 0;
  al_entry.start_vcn = 0;
  al_entry.base_ref.segment_number = index_extension_idx;
  al_entry.base_ref.sequence_number = 0;
  al_entry.attr_id = 0;

  WriteEndOfAttributesMarker(record,
                             attr_offset_value + attr.header.total_size);
  return record;
}

}  // namespace

// Extension record holding the resident $INDEX_ROOT that
// attribute_list_dir_idx's $ATTRIBUTE_LIST points to: a single "Foo" entry
// (file reference 20), plus the terminating nameless entry. baseIdx is the
// record it extends; 0 leaves it a base record.
FakeRecord MakeIndexRootExtensionRecord(ULONGLONG base_idx) {
  FakeRecord record = MakeRecordHeader(
      attr_offset_value, NtfsBrowser::Data::FileRecordFlag::InUse);
  SetRecordLink(record, 0, base_idx);

  auto& attr = *reinterpret_cast<NtfsBrowser::Data::HeaderResident*>(
      &record.at(attr_offset_value));
  attr.header.type = Attr::Type::IndexRoot;
  attr.header.non_resident = 0;
  attr.header.name_length = 0;
  attr.header.flags = 0;
  attr.header.id = 0;
  attr.attr_offset = static_cast<WORD>(sizeof(attr));

  const std::span<BYTE> body =
      std::span<BYTE>(record).subspan(attr_offset_value + attr.attr_offset);
  auto& root = *reinterpret_cast<NtfsBrowser::Data::IndexRoot*>(body.data());
  root.attr_type = Attr::Type::FileName;
  root.coll_rule = 0;
  root.ib_size = fake_file_record_size;
  root.clusters_per_ib = 1;
  root.entry_offset = gsl::narrow<DWORD>(
      (&gsl::at(body, sizeof(NtfsBrowser::Data::IndexRoot))) -
      reinterpret_cast<BYTE*>(&root.entry_offset));

  // Entry 1: "Foo", a regular (non-directory) file, reference 20.
  auto& first_entry = *reinterpret_cast<NtfsBrowser::Data::IndexEntry*>(
      &gsl::at(body, sizeof(NtfsBrowser::Data::IndexRoot)));
  first_entry.mft_index = first_entry_record;
  first_entry.mft_sn = 1;

  auto& filename =
      *reinterpret_cast<NtfsBrowser::Data::Filename*>(&first_entry.stream);
  filename.parent_ref = attribute_list_dir_idx;
  filename.flags = NtfsBrowser::Data::FilenameFlag::None;
  filename.name_length = 3;
  filename.name_space = NtfsBrowser::Data::FilenameNamespace::Win32;
  constexpr std::wstring_view foo_name = L"Foo";
  for (BYTE i = 0; i < filename.name_length; i++) {
    // i is below name_length, the length of the name.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    filename.name[i] = gsl::narrow<WORD>(foo_name[i]);
  }

  first_entry.stream_size =
      gsl::narrow<WORD>(reinterpret_cast<BYTE*>(&filename.name[3]) -
                        reinterpret_cast<BYTE*>(&filename));
  first_entry.size = gsl::narrow<WORD>(AlignAttrSize(
      &first_entry.stream - reinterpret_cast<BYTE*>(&first_entry) +
      first_entry.stream_size));

  // Entry 2: the terminating entry - no name, no sub-node.
  auto& second_entry =
      *reinterpret_cast<NtfsBrowser::Data::IndexEntry*>(&gsl::at(
          body, gsl::narrow<gsl::index>(sizeof(NtfsBrowser::Data::IndexRoot) +
                                        first_entry.size)));
  second_entry.flags = NtfsBrowser::Data::IndexEntryFlag::Last;
  second_entry.stream_size = 0;
  second_entry.size = gsl::narrow<WORD>(AlignAttrSize(
      &second_entry.stream - reinterpret_cast<BYTE*>(&second_entry)));

  root.total_entry_size =
      static_cast<DWORD>(first_entry.size) + second_entry.size;
  root.alloc_entry_size = root.total_entry_size;
  root.flags = 0;

  attr.attr_size = static_cast<DWORD>(sizeof(NtfsBrowser::Data::IndexRoot)) +
                   first_entry.size + second_entry.size;
  attr.header.total_size = AlignAttrSize(sizeof(attr) + attr.attr_size);

  WriteEndOfAttributesMarker(record,
                             attr_offset_value + attr.header.total_size);
  return record;
}

namespace {

// Directory whose $ATTRIBUTE_LIST has two entries naming the same
// extension record, once for $INDEX_ROOT and once for $INDEX_ALLOCATION.
FakeRecord MakeAttributeListTwoTypesDirRecord() {
  FakeRecord record = MakeRecordHeader(
      attr_offset_value, NtfsBrowser::Data::FileRecordFlag::InUse |
                             NtfsBrowser::Data::FileRecordFlag::Dir);

  constexpr WORD entry_size_value = static_cast<WORD>(
      AlignAttrSize(NtfsBrowser::Data::attribute_list_entry_header_size));

  auto& attr = *reinterpret_cast<NtfsBrowser::Data::HeaderResident*>(
      &record.at(attr_offset_value));
  attr.header.type = Attr::Type::AttributeList;
  attr.header.non_resident = 0;
  attr.header.name_length = 0;
  attr.header.flags = 0;
  attr.header.id = 0;
  attr.attr_size = static_cast<DWORD>(entry_size_value) * 2;
  attr.attr_offset = static_cast<WORD>(sizeof(attr));
  attr.header.total_size = AlignAttrSize(sizeof(attr) + attr.attr_size);

  const std::span<BYTE> body =
      std::span<BYTE>(record).subspan(attr_offset_value + attr.attr_offset);

  auto& first_entry =
      *reinterpret_cast<NtfsBrowser::Data::AttributeList*>(body.data());
  first_entry.attr_type = Attr::Type::IndexRoot;
  first_entry.record_size = entry_size_value;
  first_entry.name_length = 0;
  first_entry.name_offset = 0;
  first_entry.start_vcn = 0;
  first_entry.base_ref.segment_number = multi_type_extension_idx;
  first_entry.base_ref.sequence_number = 0;
  first_entry.attr_id = 0;

  auto& second_entry = *reinterpret_cast<NtfsBrowser::Data::AttributeList*>(
      &gsl::at(body, entry_size_value));
  second_entry.attr_type = Attr::Type::IndexAllocation;
  second_entry.record_size = entry_size_value;
  second_entry.name_length = 0;
  second_entry.name_offset = 0;
  second_entry.start_vcn = 0;
  second_entry.base_ref.segment_number = multi_type_extension_idx;
  second_entry.base_ref.sequence_number = 0;
  second_entry.attr_id = 0;

  WriteEndOfAttributesMarker(record,
                             attr_offset_value + attr.header.total_size);
  return record;
}

// Extension record holding both a resident $INDEX_ROOT (the same single
// "Foo" entry as MakeIndexRootExtensionRecord()) and a minimal
// non-resident $INDEX_ALLOCATION right after it. It extends record baseIdx.
FakeRecord MakeIndexRootAndAllocExtensionRecord(ULONGLONG base_idx) {
  FakeRecord record = MakeRecordHeader(
      attr_offset_value, NtfsBrowser::Data::FileRecordFlag::InUse);
  SetRecordLink(record, 0, base_idx);

  auto& root_attr = *reinterpret_cast<NtfsBrowser::Data::HeaderResident*>(
      &record.at(attr_offset_value));
  root_attr.header.type = Attr::Type::IndexRoot;
  root_attr.header.non_resident = 0;
  root_attr.header.name_length = 0;
  root_attr.header.flags = 0;
  root_attr.header.id = 0;
  root_attr.attr_offset = static_cast<WORD>(sizeof(root_attr));

  const std::span<BYTE> body = std::span<BYTE>(record).subspan(
      attr_offset_value + root_attr.attr_offset);
  auto& root = *reinterpret_cast<NtfsBrowser::Data::IndexRoot*>(body.data());
  root.attr_type = Attr::Type::FileName;
  root.coll_rule = 0;
  root.ib_size = fake_file_record_size;
  root.clusters_per_ib = 1;
  root.entry_offset = gsl::narrow<DWORD>(
      (&gsl::at(body, sizeof(NtfsBrowser::Data::IndexRoot))) -
      reinterpret_cast<BYTE*>(&root.entry_offset));

  // Entry 1: "Foo", a regular (non-directory) file, reference 20.
  auto& first_entry = *reinterpret_cast<NtfsBrowser::Data::IndexEntry*>(
      &gsl::at(body, sizeof(NtfsBrowser::Data::IndexRoot)));
  first_entry.mft_index = first_entry_record;
  first_entry.mft_sn = 1;

  auto& filename =
      *reinterpret_cast<NtfsBrowser::Data::Filename*>(&first_entry.stream);
  filename.parent_ref = attr_list_multi_type_dir_idx;
  filename.flags = NtfsBrowser::Data::FilenameFlag::None;
  filename.name_length = 3;
  filename.name_space = NtfsBrowser::Data::FilenameNamespace::Win32;
  constexpr std::wstring_view foo_name = L"Foo";
  for (BYTE i = 0; i < filename.name_length; i++) {
    // i is below name_length, the length of the name.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    filename.name[i] = gsl::narrow<WORD>(foo_name[i]);
  }

  first_entry.stream_size =
      gsl::narrow<WORD>(reinterpret_cast<BYTE*>(&filename.name[3]) -
                        reinterpret_cast<BYTE*>(&filename));
  first_entry.size = gsl::narrow<WORD>(AlignAttrSize(
      &first_entry.stream - reinterpret_cast<BYTE*>(&first_entry) +
      first_entry.stream_size));

  // Entry 2: the terminating entry - no name, no sub-node.
  auto& second_entry =
      *reinterpret_cast<NtfsBrowser::Data::IndexEntry*>(&gsl::at(
          body, gsl::narrow<gsl::index>(sizeof(NtfsBrowser::Data::IndexRoot) +
                                        first_entry.size)));
  second_entry.flags = NtfsBrowser::Data::IndexEntryFlag::Last;
  second_entry.stream_size = 0;
  second_entry.size = gsl::narrow<WORD>(AlignAttrSize(
      &second_entry.stream - reinterpret_cast<BYTE*>(&second_entry)));

  root.total_entry_size =
      static_cast<DWORD>(first_entry.size) + second_entry.size;
  root.alloc_entry_size = root.total_entry_size;
  root.flags = 0;

  root_attr.attr_size =
      static_cast<DWORD>(sizeof(NtfsBrowser::Data::IndexRoot)) +
      first_entry.size + second_entry.size;
  root_attr.header.total_size =
      AlignAttrSize(sizeof(root_attr) + root_attr.attr_size);

  const DWORD alloc_attr_offset =
      attr_offset_value + root_attr.header.total_size;
  auto& alloc_attr = *reinterpret_cast<NtfsBrowser::Data::HeaderNonResident*>(
      &record.at(alloc_attr_offset));
  alloc_attr.header.type = Attr::Type::IndexAllocation;
  alloc_attr.header.non_resident = 1;
  alloc_attr.header.name_length = 0;
  alloc_attr.header.flags = 0;
  alloc_attr.header.id = 0;
  alloc_attr.start_vcn = 0;
  alloc_attr.last_vcn = 0;
  alloc_attr.data_run_offset = static_cast<WORD>(sizeof(alloc_attr));
  alloc_attr.comp_unit_size = 0;
  alloc_attr.real_size = 0;
  alloc_attr.alloc_size = 0;
  alloc_attr.ini_size = 0;
  alloc_attr.header.total_size =
      AlignAttrSize(sizeof(alloc_attr) + run_list_room);

  // Data run: a single 0x00 byte terminates the run list immediately -
  // nothing reads through it in this fixture.
  record.at(alloc_attr_offset + sizeof(alloc_attr)) = 0x00;

  WriteEndOfAttributesMarker(record,
                             alloc_attr_offset + alloc_attr.header.total_size);
  return record;
}

// Extension record: a minimal non-resident $INDEX_ALLOCATION whose
// real_size is the given sentinel. It extends record baseIdx.
FakeRecord MakeIndexAllocationOnlyExtensionRecord(DWORD real_size,
                                                  ULONGLONG base_idx) {
  FakeRecord record = MakeRecordHeader(
      attr_offset_value, NtfsBrowser::Data::FileRecordFlag::InUse);
  SetRecordLink(record, 0, base_idx);

  auto& alloc_attr = *reinterpret_cast<NtfsBrowser::Data::HeaderNonResident*>(
      &record.at(attr_offset_value));
  alloc_attr.header.type = Attr::Type::IndexAllocation;
  alloc_attr.header.non_resident = 1;
  alloc_attr.header.name_length = 0;
  alloc_attr.header.flags = 0;
  alloc_attr.header.id = 0;
  alloc_attr.start_vcn = 0;
  alloc_attr.last_vcn = 0;
  alloc_attr.data_run_offset = static_cast<WORD>(sizeof(alloc_attr));
  alloc_attr.comp_unit_size = 0;
  alloc_attr.real_size = real_size;
  alloc_attr.alloc_size = real_size;
  alloc_attr.ini_size = real_size;
  alloc_attr.header.total_size =
      AlignAttrSize(sizeof(alloc_attr) + run_list_room);

  // Data run: a single 0x00 byte terminates the run list immediately.
  record.at(attr_offset_value + sizeof(alloc_attr)) = 0x00;

  WriteEndOfAttributesMarker(record,
                             attr_offset_value + alloc_attr.header.total_size);
  return record;
}

// Directory whose $ATTRIBUTE_LIST has four entries, each naming a
// different extension record for the same attribute type.
FakeRecord MakeFragmentedAttributeListDirRecord() {
  FakeRecord record = MakeRecordHeader(
      attr_offset_value, NtfsBrowser::Data::FileRecordFlag::InUse |
                             NtfsBrowser::Data::FileRecordFlag::Dir);

  constexpr WORD entry_size_value = static_cast<WORD>(
      AlignAttrSize(NtfsBrowser::Data::attribute_list_entry_header_size));

  auto& attr = *reinterpret_cast<NtfsBrowser::Data::HeaderResident*>(
      &record.at(attr_offset_value));
  attr.header.type = Attr::Type::AttributeList;
  attr.header.non_resident = 0;
  attr.header.name_length = 0;
  attr.header.flags = 0;
  attr.header.id = 0;
  attr.attr_size = static_cast<DWORD>(entry_size_value) * 4;
  attr.attr_offset = static_cast<WORD>(sizeof(attr));
  attr.header.total_size = AlignAttrSize(sizeof(attr) + attr.attr_size);

  const std::array<ULONGLONG, 4> extension_idxs{
      uaf_extension_idx0, uaf_extension_idx1, uaf_extension_idx2,
      uaf_extension_idx3};

  const std::span<BYTE> body =
      std::span<BYTE>(record).subspan(attr_offset_value + attr.attr_offset);
  for (size_t i = 0; i < extension_idxs.size(); i++) {
    auto& entry = *reinterpret_cast<NtfsBrowser::Data::AttributeList*>(
        &gsl::at(body, gsl::narrow<gsl::index>(i * entry_size_value)));
    entry.attr_type = Attr::Type::IndexAllocation;
    entry.record_size = entry_size_value;
    entry.name_length = 0;
    entry.name_offset = 0;
    entry.start_vcn = 0;
    entry.base_ref.segment_number = extension_idxs.at(i);
    entry.base_ref.sequence_number = 0;
    entry.attr_id = 0;
  }

  WriteEndOfAttributesMarker(record,
                             attr_offset_value + attr.header.total_size);
  return record;
}

// Builds a root-directory replacement whose resident $ATTRIBUTE_LIST holds
// one full, self-referencing entry, followed by a truncated partial one.
FakeRecord MakeAttributeListShortReadRecord() {
  FakeRecord record = MakeRecordHeader(
      attr_offset_value, NtfsBrowser::Data::FileRecordFlag::InUse |
                             NtfsBrowser::Data::FileRecordFlag::Dir);

  constexpr DWORD body_size =
      static_cast<DWORD>(NtfsBrowser::Data::attribute_list_entry_header_size) +
      10;
  static_assert(
      body_size % NtfsBrowser::Data::attribute_list_entry_header_size != 0,
      "body size must not be an exact multiple of the entry size, "
      "to reproduce a short final ReadData()");

  auto& attr = *reinterpret_cast<NtfsBrowser::Data::HeaderResident*>(
      &record.at(attr_offset_value));
  attr.header.type = Attr::Type::AttributeList;
  attr.header.non_resident = 0;
  attr.header.name_length = 0;
  attr.header.flags = 0;
  attr.header.id = 0;
  attr.attr_size = body_size;
  attr.attr_offset = static_cast<WORD>(sizeof(attr));
  attr.header.total_size = AlignAttrSize(sizeof(attr) + attr.attr_size);

  auto& first_entry = *reinterpret_cast<NtfsBrowser::Data::AttributeList*>(
      &record.at(attr_offset_value + attr.attr_offset));
  first_entry.attr_type = Attr::Type::Data;
  first_entry.record_size =
      static_cast<WORD>(NtfsBrowser::Data::attribute_list_entry_header_size);
  first_entry.name_length = 0;
  first_entry.name_offset = 0;
  first_entry.start_vcn = 0;
  first_entry.base_ref.segment_number = static_cast<ULONGLONG>(Mft::Idx::Root);
  first_entry.base_ref.sequence_number = 0;
  first_entry.attr_id = 0;

  WriteEndOfAttributesMarker(record,
                             attr_offset_value + attr.header.total_size);
  return record;
}

// Builds a directory record whose resident $ATTRIBUTE_LIST holds a real
// entry (relocating $INDEX_ROOT to index_extension_idx), followed by one
// whose own record_size is nonzero but smaller than the entry header
// itself. VG4(a): the mid-loop record_size bounds check, never a short
// ReadData().
FakeRecord MakeAttributeListRecordSizeTooSmallDirRecord() {
  FakeRecord record = MakeRecordHeader(
      attr_offset_value, NtfsBrowser::Data::FileRecordFlag::InUse |
                             NtfsBrowser::Data::FileRecordFlag::Dir);

  constexpr WORD entry_size_value = static_cast<WORD>(
      AlignAttrSize(NtfsBrowser::Data::attribute_list_entry_header_size));
  // Nonzero, but smaller than entry_size - the exact condition under test.
  constexpr WORD too_small_record_size = 5;

  auto& attr = *reinterpret_cast<NtfsBrowser::Data::HeaderResident*>(
      &record.at(attr_offset_value));
  attr.header.type = Attr::Type::AttributeList;
  attr.header.non_resident = 0;
  attr.header.name_length = 0;
  attr.header.flags = 0;
  attr.header.id = 0;
  attr.attr_size = static_cast<DWORD>(entry_size_value) * 2;
  attr.attr_offset = static_cast<WORD>(sizeof(attr));
  attr.header.total_size = AlignAttrSize(sizeof(attr) + attr.attr_size);

  const std::span<BYTE> body =
      std::span<BYTE>(record).subspan(attr_offset_value + attr.attr_offset);

  auto& first_entry =
      *reinterpret_cast<NtfsBrowser::Data::AttributeList*>(body.data());
  first_entry.attr_type = Attr::Type::IndexRoot;
  first_entry.record_size = entry_size_value;
  first_entry.name_length = 0;
  first_entry.name_offset = 0;
  first_entry.start_vcn = 0;
  first_entry.base_ref.segment_number = index_extension_idx;
  first_entry.base_ref.sequence_number = 0;
  first_entry.attr_id = 0;

  // Relocates nowhere - base_ref names this same directory record, so
  // AttrList skips it - only its record_size matters here.
  auto& second_entry = *reinterpret_cast<NtfsBrowser::Data::AttributeList*>(
      &gsl::at(body, entry_size_value));
  second_entry.attr_type = Attr::Type::Data;
  second_entry.record_size = too_small_record_size;
  second_entry.name_length = 0;
  second_entry.name_offset = 0;
  second_entry.start_vcn = 0;
  second_entry.base_ref.segment_number = attribute_list_dir_idx;
  second_entry.base_ref.sequence_number = 0;
  second_entry.attr_id = 0;

  WriteEndOfAttributesMarker(record,
                             attr_offset_value + attr.header.total_size);
  return record;
}

// Builds a directory record whose resident $ATTRIBUTE_LIST holds a single
// real entry (relocating $INDEX_ROOT to index_extension_idx) whose own
// record_size overshoots the attribute's declared size. VG4(b): the
// post-loop offset-vs-size check, reached through AttrList's normal
// (nullopt) end, not a short ReadData().
FakeRecord MakeAttributeListOffsetMismatchDirRecord() {
  FakeRecord record = MakeRecordHeader(
      attr_offset_value, NtfsBrowser::Data::FileRecordFlag::InUse |
                             NtfsBrowser::Data::FileRecordFlag::Dir);

  constexpr WORD entry_size_value = static_cast<WORD>(
      AlignAttrSize(NtfsBrowser::Data::attribute_list_entry_header_size));
  // Past entry_size, so the single entry's declared span overshoots the
  // attribute's own declared size below.
  constexpr WORD overshoot_record_size =
      static_cast<WORD>(entry_size_value + 4);

  auto& attr = *reinterpret_cast<NtfsBrowser::Data::HeaderResident*>(
      &record.at(attr_offset_value));
  attr.header.type = Attr::Type::AttributeList;
  attr.header.non_resident = 0;
  attr.header.name_length = 0;
  attr.header.flags = 0;
  attr.header.id = 0;
  attr.attr_size = static_cast<DWORD>(entry_size_value);
  attr.attr_offset = static_cast<WORD>(sizeof(attr));
  attr.header.total_size = AlignAttrSize(sizeof(attr) + attr.attr_size);

  auto& first_entry = *reinterpret_cast<NtfsBrowser::Data::AttributeList*>(
      &record.at(attr_offset_value + attr.attr_offset));
  first_entry.attr_type = Attr::Type::IndexRoot;
  first_entry.record_size = overshoot_record_size;
  first_entry.name_length = 0;
  first_entry.name_offset = 0;
  first_entry.start_vcn = 0;
  first_entry.base_ref.segment_number = index_extension_idx;
  first_entry.base_ref.sequence_number = 0;
  first_entry.attr_id = 0;

  WriteEndOfAttributesMarker(record,
                             attr_offset_value + attr.header.total_size);
  return record;
}

// Builds a directory record whose resident $ATTRIBUTE_LIST holds one full
// entry per (type, record) pair, each with a nominal record_size.
FakeRecord MakeAttributeListEntriesDirRecord(
    const std::vector<std::pair<Attr::Type, ULONGLONG>>& entries) {
  FakeRecord record = MakeRecordHeader(
      attr_offset_value, NtfsBrowser::Data::FileRecordFlag::InUse |
                             NtfsBrowser::Data::FileRecordFlag::Dir);

  constexpr WORD entry_size_value = static_cast<WORD>(
      AlignAttrSize(NtfsBrowser::Data::attribute_list_entry_header_size));

  auto& attr = *reinterpret_cast<NtfsBrowser::Data::HeaderResident*>(
      &record.at(attr_offset_value));
  attr.header.type = Attr::Type::AttributeList;
  attr.header.non_resident = 0;
  attr.header.name_length = 0;
  attr.header.flags = 0;
  attr.header.id = 0;
  attr.attr_size = gsl::narrow<DWORD>(entry_size_value * entries.size());
  attr.attr_offset = static_cast<WORD>(sizeof(attr));
  attr.header.total_size = AlignAttrSize(sizeof(attr) + attr.attr_size);

  const size_t body_offset = attr_offset_value + attr.attr_offset;
  for (size_t i = 0; i < entries.size(); i++) {
    auto& entry = *reinterpret_cast<NtfsBrowser::Data::AttributeList*>(
        &record.at(body_offset + (i * entry_size_value)));
    entry.attr_type = entries[i].first;
    entry.record_size = entry_size_value;
    entry.name_length = 0;
    entry.name_offset = 0;
    entry.start_vcn = 0;
    entry.base_ref.segment_number = entries[i].second;
    entry.base_ref.sequence_number = 0;
    entry.attr_id = 0;
  }

  WriteEndOfAttributesMarker(record,
                             attr_offset_value + attr.header.total_size);
  return record;
}

// Builds a directory record whose resident $ATTRIBUTE_LIST, via a single
// full entry, relocates to targetIdx.
FakeRecord MakeAttributeListCycleRecord(ULONGLONG target_idx) {
  FakeRecord record = MakeRecordHeader(
      attr_offset_value, NtfsBrowser::Data::FileRecordFlag::InUse |
                             NtfsBrowser::Data::FileRecordFlag::Dir);

  auto& attr = *reinterpret_cast<NtfsBrowser::Data::HeaderResident*>(
      &record.at(attr_offset_value));
  attr.header.type = Attr::Type::AttributeList;
  attr.header.non_resident = 0;
  attr.header.name_length = 0;
  attr.header.flags = 0;
  attr.header.id = 0;
  attr.attr_size =
      static_cast<DWORD>(NtfsBrowser::Data::attribute_list_entry_header_size);
  attr.attr_offset = static_cast<WORD>(sizeof(attr));
  attr.header.total_size = AlignAttrSize(sizeof(attr) + attr.attr_size);

  auto& first_entry = *reinterpret_cast<NtfsBrowser::Data::AttributeList*>(
      &record.at(attr_offset_value + attr.attr_offset));
  first_entry.attr_type = Attr::Type::AttributeList;
  first_entry.record_size =
      static_cast<WORD>(NtfsBrowser::Data::attribute_list_entry_header_size);
  first_entry.name_length = 0;
  first_entry.name_offset = 0;
  first_entry.start_vcn = 0;
  first_entry.base_ref.segment_number = target_idx;
  first_entry.base_ref.sequence_number = 0;
  first_entry.attr_id = 0;

  WriteEndOfAttributesMarker(record,
                             attr_offset_value + attr.header.total_size);
  return record;
}

// Builds a directory record whose resident $ATTRIBUTE_LIST packs two
// entries at the real on-disk stride (attribute_list_real_entry_size), not
// sizeof(Data::AttributeList).
FakeRecord MakeAttributeListTightlyPackedDirRecord() {
  FakeRecord record = MakeRecordHeader(
      attr_offset_value, NtfsBrowser::Data::FileRecordFlag::InUse |
                             NtfsBrowser::Data::FileRecordFlag::Dir);

  auto& attr = *reinterpret_cast<NtfsBrowser::Data::HeaderResident*>(
      &record.at(attr_offset_value));
  attr.header.type = Attr::Type::AttributeList;
  attr.header.non_resident = 0;
  attr.header.name_length = 0;
  attr.header.flags = 0;
  attr.header.id = 0;
  attr.attr_size = static_cast<DWORD>(attribute_list_real_entry_size) * 2;
  attr.attr_offset = static_cast<WORD>(sizeof(attr));
  attr.header.total_size = AlignAttrSize(sizeof(attr) + attr.attr_size);

  const std::span<BYTE> body =
      std::span<BYTE>(record).subspan(attr_offset_value + attr.attr_offset);

  // The second entry sits at an odd stride on purpose, so it cannot be bound
  // to a struct reference: fill an aligned copy and copy the on-disk bytes.
  const auto write_entry = [](BYTE* dest, Attr::Type type, ULONGLONG record_idx,
                              WORD attr_id) {
    NtfsBrowser::Data::AttributeList entry{};
    entry.attr_type = type;
    entry.record_size = attribute_list_real_entry_size;
    entry.name_length = 0;
    entry.name_offset = 0;
    entry.start_vcn = 0;
    entry.base_ref.segment_number = record_idx;
    entry.base_ref.sequence_number = 0;
    entry.attr_id = attr_id;
    std::memcpy(dest, &entry, attribute_list_real_entry_size);
  };
  write_entry(body.data(), Attr::Type::IndexRoot,
              attr_list_tight_pack_ext_idx_a, 0);
  write_entry(&gsl::at(body, attribute_list_real_entry_size),
              Attr::Type::IndexAllocation, attr_list_tight_pack_ext_idx_b, 1);

  WriteEndOfAttributesMarker(record,
                             attr_offset_value + attr.header.total_size);
  return record;
}

}  // namespace

std::vector<BYTE> BuildFakeNtfsImageWithAttributeListDirectory() {
  std::vector<BYTE> image = BuildFakeNtfsImage();

  const DWORD mft_addr = static_cast<DWORD>(mft_lcn) * cluster_size;
  const auto put_record = [&](ULONGLONG idx, const FakeRecord& record) {
    const size_t offset =
        mft_addr +
        static_cast<size_t>(fake_file_record_size) * gsl::narrow<size_t>(idx);
    std::memcpy(&image.at(offset), record.data(), record.size());
  };

  put_record(attribute_list_dir_idx, MakeAttributeListOnlyDirRecord());
  put_record(index_extension_idx,
             MakeIndexRootExtensionRecord(attribute_list_dir_idx));

  return image;
}

std::vector<BYTE> BuildFakeNtfsImageWithMultiTypeAttributeListDirectory() {
  std::vector<BYTE> image = BuildFakeNtfsImage();

  const DWORD mft_addr = static_cast<DWORD>(mft_lcn) * cluster_size;
  const auto put_record = [&](ULONGLONG idx, const FakeRecord& record) {
    const size_t offset =
        mft_addr +
        static_cast<size_t>(fake_file_record_size) * gsl::narrow<size_t>(idx);
    std::memcpy(&image.at(offset), record.data(), record.size());
  };

  put_record(attr_list_multi_type_dir_idx,
             MakeAttributeListTwoTypesDirRecord());
  put_record(multi_type_extension_idx, MakeIndexRootAndAllocExtensionRecord(
                                           attr_list_multi_type_dir_idx));

  return image;
}

std::vector<BYTE> BuildFakeNtfsImageWithFragmentedAttributeListDirectory() {
  std::vector<BYTE> image = BuildFakeNtfsImage();

  const DWORD mft_addr = static_cast<DWORD>(mft_lcn) * cluster_size;
  const auto put_record = [&](ULONGLONG idx, const FakeRecord& record) {
    const size_t offset =
        mft_addr +
        static_cast<size_t>(fake_file_record_size) * gsl::narrow<size_t>(idx);
    std::memcpy(&image.at(offset), record.data(), record.size());
  };

  put_record(uaf_attr_list_dir_idx, MakeFragmentedAttributeListDirRecord());
  put_record(uaf_extension_idx0,
             MakeIndexAllocationOnlyExtensionRecord(
                 uaf_real_size_sentinels.at(0), uaf_attr_list_dir_idx));
  put_record(uaf_extension_idx1,
             MakeIndexAllocationOnlyExtensionRecord(
                 uaf_real_size_sentinels.at(1), uaf_attr_list_dir_idx));
  put_record(uaf_extension_idx2,
             MakeIndexAllocationOnlyExtensionRecord(
                 uaf_real_size_sentinels.at(2), uaf_attr_list_dir_idx));
  put_record(uaf_extension_idx3,
             MakeIndexAllocationOnlyExtensionRecord(
                 uaf_real_size_sentinels.at(3), uaf_attr_list_dir_idx));

  return image;
}

std::vector<BYTE> BuildFakeNtfsImageWithAttributeListShortRead() {
  std::vector<BYTE> image = BuildFakeNtfsImage();

  // Overwrites the whole root record (#5), not just a single field.
  const DWORD mft_addr = static_cast<DWORD>(mft_lcn) * cluster_size;
  const size_t root_offset =
      mft_addr + static_cast<size_t>(fake_file_record_size) *
                     static_cast<size_t>(Mft::Idx::Root);
  const FakeRecord record = MakeAttributeListShortReadRecord();
  std::memcpy(&image.at(root_offset), record.data(), record.size());

  return image;
}

std::vector<BYTE> BuildFakeNtfsImageWithAttributeListRecordSizeTooSmall() {
  std::vector<BYTE> image = BuildFakeNtfsImage();

  const DWORD mft_addr = static_cast<DWORD>(mft_lcn) * cluster_size;
  const auto put_record = [&](ULONGLONG idx, const FakeRecord& record) {
    const size_t offset =
        mft_addr +
        static_cast<size_t>(fake_file_record_size) * gsl::narrow<size_t>(idx);
    std::memcpy(&image.at(offset), record.data(), record.size());
  };

  put_record(attribute_list_dir_idx,
             MakeAttributeListRecordSizeTooSmallDirRecord());
  put_record(index_extension_idx,
             MakeIndexRootExtensionRecord(attribute_list_dir_idx));

  return image;
}

std::vector<BYTE> BuildFakeNtfsImageWithAttributeListOffsetMismatch() {
  std::vector<BYTE> image = BuildFakeNtfsImage();

  const DWORD mft_addr = static_cast<DWORD>(mft_lcn) * cluster_size;
  const auto put_record = [&](ULONGLONG idx, const FakeRecord& record) {
    const size_t offset =
        mft_addr +
        static_cast<size_t>(fake_file_record_size) * gsl::narrow<size_t>(idx);
    std::memcpy(&image.at(offset), record.data(), record.size());
  };

  put_record(attribute_list_dir_idx,
             MakeAttributeListOffsetMismatchDirRecord());
  put_record(index_extension_idx,
             MakeIndexRootExtensionRecord(attribute_list_dir_idx));

  return image;
}

std::vector<BYTE> BuildFakeNtfsImageWithAttributeListCycle() {
  std::vector<BYTE> image = BuildFakeNtfsImage();

  const DWORD mft_addr = static_cast<DWORD>(mft_lcn) * cluster_size;
  const auto put_record = [&](ULONGLONG idx, const FakeRecord& record) {
    const size_t offset =
        mft_addr +
        static_cast<size_t>(fake_file_record_size) * gsl::narrow<size_t>(idx);
    std::memcpy(&image.at(offset), record.data(), record.size());
  };

  put_record(static_cast<ULONGLONG>(Mft::Idx::Root),
             MakeAttributeListCycleRecord(attr_list_cycle_ext_idx));
  FakeRecord ext =
      MakeAttributeListCycleRecord(static_cast<ULONGLONG>(Mft::Idx::Root));
  SetRecordLink(ext, 0, static_cast<ULONGLONG>(Mft::Idx::Root));
  put_record(attr_list_cycle_ext_idx, ext);

  return image;
}

std::vector<BYTE> BuildFakeNtfsImageWithAttributeListInvalidType() {
  std::vector<BYTE> image = BuildFakeNtfsImage();
  const DWORD mft_addr = static_cast<DWORD>(mft_lcn) * cluster_size;

  // 0x0005 is not an Attr::Type: its low nibble is set.
  PutMftRecord(image, mft_addr, attribute_list_dir_idx,
               MakeAttributeListEntriesDirRecord(
                   {{static_cast<Attr::Type>(0x0005), index_extension_idx}}));
  PutMftRecord(image, mft_addr, index_extension_idx,
               MakeIndexRootExtensionRecord(attribute_list_dir_idx));

  return image;
}

std::vector<BYTE> BuildFakeNtfsImageWithAttributeListDuplicateEntry() {
  std::vector<BYTE> image = BuildFakeNtfsImage();
  const DWORD mft_addr = static_cast<DWORD>(mft_lcn) * cluster_size;

  PutMftRecord(image, mft_addr, attribute_list_dir_idx,
               MakeAttributeListEntriesDirRecord(
                   {{Attr::Type::IndexRoot, index_extension_idx},
                    {Attr::Type::IndexRoot, index_extension_idx}}));
  PutMftRecord(image, mft_addr, index_extension_idx,
               MakeIndexRootExtensionRecord(attribute_list_dir_idx));

  return image;
}

std::vector<BYTE> BuildFakeNtfsImageWithTightlyPackedAttributeListDirectory() {
  std::vector<BYTE> image = BuildFakeNtfsImage();

  const DWORD mft_addr = static_cast<DWORD>(mft_lcn) * cluster_size;
  const auto put_record = [&](ULONGLONG idx, const FakeRecord& record) {
    const size_t offset =
        mft_addr +
        static_cast<size_t>(fake_file_record_size) * gsl::narrow<size_t>(idx);
    std::memcpy(&image.at(offset), record.data(), record.size());
  };

  put_record(attr_list_tight_pack_dir_idx,
             MakeAttributeListTightlyPackedDirRecord());
  put_record(attr_list_tight_pack_ext_idx_a,
             MakeIndexRootExtensionRecord(attr_list_tight_pack_dir_idx));
  put_record(attr_list_tight_pack_ext_idx_b,
             MakeIndexAllocationOnlyExtensionRecord(
                 attr_list_tight_pack_real_size, attr_list_tight_pack_dir_idx));

  return image;
}

}  // namespace NtfsBrowserTests
