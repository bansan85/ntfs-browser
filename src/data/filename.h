#pragma once

#include <ntfs-browser/win-types.h>

namespace NtfsBrowser::Data {

enum class FilenameFlag : DWORD;
enum class FilenameNamespace : BYTE;

struct Filename {
  ULONGLONG parent_ref;          // File reference to the parent directory
  ULONGLONG create_time;         // File creation time
  ULONGLONG alter_time;          // File altered time
  ULONGLONG mft_time;            // MFT changed time
  ULONGLONG read_time;           // File read time
  ULONGLONG alloc_size;          // Allocated size of the file
  ULONGLONG real_size;           // Real size of the file
  FilenameFlag flags;            // Flags
  DWORD er;                      // Used by EAs and Reparse
  BYTE name_length;              // Filename length in characters
  FilenameNamespace name_space;  // Filename space
  // NOLINTNEXTLINE(cppcoreguidelines-avoid-c-arrays,modernize-avoid-c-arrays)
  WORD name[1];  // Filename
};

}  // namespace NtfsBrowser::Data
