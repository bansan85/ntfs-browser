#pragma once

#include <ntfs-browser/win-types.h>

#include <cstddef>

#include <ntfs-browser/namespace.h>

namespace NTFS_BROWSER_NAMESPACE::Data {

// Without this, alignof(ULONGLONG) pads the structs: the on-disk records are
// packed, and they are only ever read through memcpy.
#pragma pack(1)

// The first 8 bytes of every USN record, whatever its version.
struct UsnRecordCommon {
  DWORD record_length;  // Whole record, name included. A multiple of 8.
  WORD major_version;   // 2, 3 or 4.
  WORD minor_version;   // 0.
};

// USN_RECORD_V2: 64-bit file references. The name follows at name_offset.
struct UsnRecordV2 {
  UsnRecordCommon common;
  ULONGLONG file_reference;
  ULONGLONG parent_reference;
  LONGLONG usn;  // Byte offset of this record in the $J stream.
  ULONGLONG timestamp;
  DWORD reason;
  DWORD source_info;
  DWORD security_id;
  DWORD file_attributes;
  WORD file_name_length;  // In bytes, not characters.
  WORD file_name_offset;  // From the start of the record.
};

// A 128-bit file reference of USN_RECORD_V3 and V4. On NTFS the low half is the
// usual 48-bit record number and 16-bit sequence number, and the high half is
// zero. ReFS uses both halves.
struct UsnFileId128 {
  ULONGLONG low;
  ULONGLONG high;
};

// USN_RECORD_V3: 128-bit file references.
struct UsnRecordV3 {
  UsnRecordCommon common;
  UsnFileId128 file_reference;
  UsnFileId128 parent_reference;
  LONGLONG usn;
  ULONGLONG timestamp;
  DWORD reason;
  DWORD source_info;
  DWORD security_id;
  DWORD file_attributes;
  WORD file_name_length;
  WORD file_name_offset;
};

// USN_RECORD_V4: range tracking. It has no name, so the library only counts it.
struct UsnRecordV4 {
  UsnRecordCommon common;
  UsnFileId128 file_reference;
  UsnFileId128 parent_reference;
  LONGLONG usn;
  DWORD reason;
  DWORD source_info;
  DWORD remaining_extents;
  WORD number_of_extents;
  WORD extent_size;
};

#pragma pack()

// The sizes and offsets below are the ones the USN_RECORD_V2, V3 and V4
// structures of Microsoft's documentation give.
// NOLINTBEGIN(cppcoreguidelines-avoid-magic-numbers,readability-magic-numbers)
static_assert(sizeof(UsnRecordCommon) == 8);
static_assert(sizeof(UsnRecordV2) == 60);
static_assert(offsetof(UsnRecordV2, usn) == 24);
static_assert(offsetof(UsnRecordV2, timestamp) == 32);
static_assert(offsetof(UsnRecordV2, reason) == 40);
static_assert(offsetof(UsnRecordV2, file_attributes) == 52);
static_assert(offsetof(UsnRecordV2, file_name_length) == 56);
static_assert(sizeof(UsnRecordV3) == 76);
static_assert(offsetof(UsnRecordV3, usn) == 40);
static_assert(offsetof(UsnRecordV3, timestamp) == 48);
static_assert(offsetof(UsnRecordV3, reason) == 56);
static_assert(offsetof(UsnRecordV3, file_attributes) == 68);
static_assert(offsetof(UsnRecordV3, file_name_length) == 72);
static_assert(sizeof(UsnRecordV4) == 64);
// NOLINTEND(cppcoreguidelines-avoid-magic-numbers,readability-magic-numbers)

// Records of a journal start on a multiple of this many bytes.
inline constexpr size_t usn_record_alignment = 8;

}  // namespace NTFS_BROWSER_NAMESPACE::Data
