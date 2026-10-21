#pragma once

#include <ntfs-browser/win-types.h>

#include <cstdint>
#include <string>

#include <ntfs-browser/namespace.h>

namespace NTFS_BROWSER_NAMESPACE::Record {

// Where a carved name was found.
enum class CarveSource : std::uint8_t {
  // The bytes after the attribute end marker of an MFT record.
  RecordSlack
};

// A $FILE_NAME that nothing live describes any more, recognised in leftover
// bytes by its shape. It is a candidate and no authority: the bytes may be a
// stale copy, or only look like a name. A carved name never changes a live
// field of the record it was found in.
struct CarvedName {
  // The name, decoded like a live one.
  std::wstring name;
  // The directory the name was filed under. Zero while parent_known is false.
  ULONGLONG parent_record{0};
  WORD parent_sequence{0};
  // False when the bytes that held the parent reference were overwritten.
  bool parent_known{false};
  // The four times of the $FILE_NAME, converted like the times of
  // MftTree::Entry. Zero while times_known is false.
  FILETIME create_time{};
  FILETIME write_time{};
  FILETIME change_time{};
  FILETIME access_time{};
  // False when the bytes that held the times were overwritten.
  bool times_known{false};
  // The namespace byte: 0 POSIX, 1 Win32, 2 DOS, 3 Win32 and DOS.
  BYTE name_space{0};
  // True when the start of the structure was overwritten and only the name,
  // and maybe one of the two groups above, survived. That is, when
  // parent_known or times_known is false. A partial name always has at least
  // 5 characters.
  bool partial{false};
  CarveSource source{CarveSource::RecordSlack};
};

}  // namespace NTFS_BROWSER_NAMESPACE::Record
