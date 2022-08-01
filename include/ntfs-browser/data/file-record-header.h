#pragma once

#include <cstdint>

#include <windows.h>

namespace NtfsBrowser
{
/******************************
		File Record
	---------------------
	| File Record Header|
	---------------------
	|    Attribute 1    |
	---------------------
	|    Attribute 2    |
	---------------------
	|      ......       |
	---------------------
	|     0xFFFFFFFF    |
	---------------------
*******************************/

// File Record Header

constexpr uint32_t kFileRecordMagic('ELIF');
enum class FileRecordFlag
{
  INUSE = 0x01,
  DIR = 0x02
};

struct FileRecordHeader
{
  DWORD Magic;          // "FILE"
  WORD OffsetOfUS;      // Offset of Update Sequence
  WORD SizeOfUS;        // Size in words of Update Sequence Number & Array
  ULONGLONG LSN;        // $LogFile Sequence Number
  WORD SeqNo;           // Sequence number
  WORD Hardlinks;       // Hard link count
  WORD OffsetOfAttr;    // Offset of the first Attribute
  WORD Flags;           // Flags
  DWORD RealSize;       // Real size of the FILE record
  DWORD AllocSize;      // Allocated size of the FILE record
  ULONGLONG RefToBase;  // File reference to the base FILE record
  WORD NextAttrId;      // Next Attribute Id
  WORD Align;           // Align to 4 byte boundary
  DWORD RecordNo;       // Number of this MFT Record
};
}  // namespace NtfsBrowser