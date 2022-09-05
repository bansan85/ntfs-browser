#pragma once

#include <cstdint>
#include <memory>
#include <span>
#include <vector>

#include <gsl/pointers>

#include <windows.h>

#include <ntfs-browser/file-reader.h>

#include "../flag/file-record.h"

namespace NtfsBrowser
{
constexpr uint32_t kFileRecordMagic('ELIF');

struct AttrHeaderCommon;

struct FileRecordHeader
{
  union Data
  {
    struct
    {
      DWORD magic;          // "FILE"
      WORD offset_of_us;    // Offset of Update Sequence
      WORD size_of_us;      // Size in words of Update Sequence Number & Array
      ULONGLONG lsn;        // $LogFile Sequence Number
      WORD seq_no;          // Sequence number
      WORD hardlinks;       // Hard link count
      WORD offset_of_attr;  // Offset of the first Attribute
      Flag::FileRecord flags;  // Flags
      DWORD real_size;         // Real size of the FILE record
      DWORD alloc_size;        // Allocated size of the FILE record
      ULONGLONG ref_to_base;   // File reference to the base FILE record
      WORD next_attr_id;       // Next Attribute Id
      WORD align;              // Align to 4 byte boundary
      DWORD record_no;         // Number of this MFT Record
    };
    BYTE raw[1024];
  };

  WORD us_number{0};
  std::vector<WORD> us_array{};
  size_t sector_size;

  FileRecordHeader(std::span<const BYTE> buffer, size_t sector_size);
  // Verify US and update sectors
  [[nodiscard]] bool PatchUS() noexcept;
  const AttrHeaderCommon& HeaderCommon() noexcept;
  static std::unique_ptr<FileRecordHeader>
      Factory(std::span<const BYTE> buffer, size_t sector_size,
              FileReader::Strategy strategy);
  virtual const FileRecordHeader::Data* GetData() const = 0;
};

struct FileRecordHeaderLight : public FileRecordHeader
{
  std::span<const BYTE> data_;

  FileRecordHeaderLight(std::span<const BYTE> buffer, size_t sector_size);

  const FileRecordHeader::Data* GetData() const override;
};

struct FileRecordHeaderHeavy : public FileRecordHeader
{
  FileRecordHeader::Data data_;

  FileRecordHeaderHeavy(std::span<const BYTE> buffer, size_t sector_size);

  const FileRecordHeader::Data* GetData() const override;
};

}  // namespace NtfsBrowser