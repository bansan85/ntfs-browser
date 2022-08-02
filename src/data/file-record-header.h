#pragma once

#include <cstdint>

#include <windows.h>

#include "../flag/file-record.h"

namespace NtfsBrowser
{
constexpr uint32_t kFileRecordMagic('ELIF');

struct FileRecordHeader
{
  DWORD Magic;             // "FILE"
  WORD OffsetOfUS;         // Offset of Update Sequence
  WORD SizeOfUS;           // Size in words of Update Sequence Number & Array
  ULONGLONG LSN;           // $LogFile Sequence Number
  WORD SeqNo;              // Sequence number
  WORD Hardlinks;          // Hard link count
  WORD OffsetOfAttr;       // Offset of the first Attribute
  Flag::FileRecord Flags;  // Flags
  DWORD RealSize;          // Real size of the FILE record
  DWORD AllocSize;         // Allocated size of the FILE record
  ULONGLONG RefToBase;     // File reference to the base FILE record
  WORD NextAttrId;         // Next Attribute Id
  WORD Align;              // Align to 4 byte boundary
  DWORD RecordNo;          // Number of this MFT Record
};
}  // namespace NtfsBrowser