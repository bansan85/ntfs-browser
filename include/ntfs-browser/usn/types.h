#pragma once

#include <ntfs-browser/win-types.h>

#include <cstdint>
#include <limits>
#include <string>
#include <vector>

#include <ntfs-browser/namespace.h>

namespace NTFS_BROWSER_NAMESPACE::Usn {

// Why a journal record was written. The values are the USN_REASON_* bits of
// Windows. A record carries the OR of everything that happened to the file
// since it was opened.
// NOLINTNEXTLINE(performance-enum-size)
enum class Reason : DWORD {
  DataOverwrite = 0x00000001,
  DataExtend = 0x00000002,
  DataTruncation = 0x00000004,
  NamedDataOverwrite = 0x00000010,
  NamedDataExtend = 0x00000020,
  NamedDataTruncation = 0x00000040,
  FileCreate = 0x00000100,
  FileDelete = 0x00000200,
  EaChange = 0x00000400,
  SecurityChange = 0x00000800,
  RenameOldName = 0x00001000,
  RenameNewName = 0x00002000,
  IndexableChange = 0x00004000,
  BasicInfoChange = 0x00008000,
  HardLinkChange = 0x00010000,
  CompressionChange = 0x00020000,
  EncryptionChange = 0x00040000,
  ObjectIdChange = 0x00080000,
  ReparsePointChange = 0x00100000,
  StreamChange = 0x00200000,
  TransactedChange = 0x00400000,
  IntegrityChange = 0x00800000,
  DesiredStorageClassChange = 0x01000000,
  Close = 0x80000000
};

// NOLINTNEXTLINE
DEFINE_ENUM_FLAG_OPERATORS(NTFS_BROWSER_NAMESPACE::Usn::Reason)

// Most journal records a volume keeps by default. A 100 MiB journal holds at
// most 104 857 600 / 64 = 1 638 400 records, 64 bytes being the smallest V2
// record (a 60-byte header and a one-character name, rounded up to 8). This
// count therefore never truncates a journal of that size. It takes about
// 330 MiB at that bound, estimated at 200 bytes a record.
inline constexpr std::uint64_t default_max_records = 1638400;

// Removes the limit on the records a volume keeps.
inline constexpr std::uint64_t unlimited_records =
    (std::numeric_limits<std::uint64_t>::max)();

// One change event, as one journal record states it. The name is the name the
// file had at the time of the event, and no path.
struct Record {
  // Record number: the low 48 bits of the file reference.
  ULONGLONG file_record{0};
  // Sequence number: the high 16 bits of the file reference.
  WORD file_sequence{0};
  // High half of a 128-bit (V3) reference. Zero on NTFS. When it is not, the
  // reference cannot name an $MFT record.
  ULONGLONG file_reference_high{0};
  ULONGLONG parent_record{0};
  WORD parent_sequence{0};
  ULONGLONG parent_reference_high{0};
  // Journal sequence number: the record's own offset in the $J stream.
  LONGLONG usn{0};
  // Time of the event, converted like the $STANDARD_INFORMATION times of the
  // library: local time on Windows, UTC elsewhere.
  FILETIME time{};
  // The Usn::Reason bits of this event, as the raw value.
  DWORD reason{0};
  // File attributes at the time of the event.
  DWORD file_attributes{0};
  std::wstring name;
  // 2 or 3.
  WORD major_version{0};
};

// What reading $UsnJrnl:$J gave.
struct Journal {
  // In journal order, so ascending usn. At most VolumeOptions::max_usn_records.
  std::vector<Record> records;
  // Stretches of bytes that were neither a record nor padding, and that the
  // reader skipped to resume at the next plausible record.
  ULONGLONG damaged_records{0};
  // Well-formed records that carry no name (V4), counted and not kept.
  ULONGLONG skipped_records{0};
  // True when older records were dropped to stay within the limit.
  bool truncated{false};
  // Bytes read from the real ranges of the stream. Sparse holes cost none.
  ULONGLONG scanned_bytes{0};
};

}  // namespace NTFS_BROWSER_NAMESPACE::Usn
