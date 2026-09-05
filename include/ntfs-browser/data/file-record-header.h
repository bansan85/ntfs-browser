#pragma once

#include <cstdint>
#include <memory>
#include <span>
#include <vector>

#include <gsl/pointers>

#include <ntfs-browser/win-types.h>

#include <ntfs-browser/file-reader.h>
#include <ntfs-browser/strategy.h>

#include "../flag/file-record.h"

namespace NtfsBrowser
{
constexpr uint32_t kFileRecordMagic('ELIF');

// Smallest buffer FileRecordHeader's ctor will accept: exactly enough to
// hold Data's named header fields (48 bytes, right after which the first
// attribute starts - see NTFSLibTests/unit-tests/fake-ntfs-image.cpp's
// kAttrOffset). Anything smaller can't even be reinterpreted as a valid
// header. This is a structural fact about Data's named-fields layout, not
// about Data::raw's capacity - see kMaxFileRecordSize below and bug F9 in
// docs/bug-reports/2026-09-03-full-repo.md for why the two must be kept
// distinct.
constexpr size_t kMinFileRecordHeaderSize = 48;

// Largest file record buffer FileRecordHeader::Data::raw can physically
// hold. A real NTFS file record is exactly one of a handful of sizes
// derived from the boot sector's BPB (see NtfsVolume<S>::ParseBootSector(),
// src/ntfs-volume.cpp); 4096 is generous enough to cover every one of them
// that occurs in practice, in particular the 4096-byte records a 4Kn
// (4096-byte-sector) disk's NTFS volume necessarily uses - a file record can
// never be smaller than a sector. FileRecordHeaderImpl<FULL_CACHE>'s ctor
// memcpy()s the whole input buffer into a by-value Data data_, so this is
// also the bound that keeps that memcpy from overflowing data_. See bug F9
// in docs/bug-reports/2026-09-03-full-repo.md.
constexpr size_t kMaxFileRecordSize = 4096;

struct AttrHeaderCommon;

template <Strategy S>
struct FileRecordHeaderImpl;

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
    BYTE raw[kMaxFileRecordSize];
  };

  WORD us_number{0};
  std::vector<WORD> us_array{};
  size_t sector_size;
  // Actual size of the buffer this instance was constructed with - always
  // <= sizeof(Data::raw), but not necessarily equal to it (bug F9: raw[]'s
  // capacity now has to cover the largest supported record size, 4096,
  // while a given instance's real record could be smaller, eg. 1024).
  // HeaderCommon() must bound offset_of_attr against THIS, not against
  // Data::raw's static capacity, or a record smaller than kMaxFileRecordSize
  // could let offset_of_attr point past this instance's own real buffer.
  size_t buffer_size_;

  FileRecordHeader(std::span<const BYTE> buffer, size_t sector_size);
  virtual ~FileRecordHeader() = default;
  // Verify US and update sectors
  [[nodiscard]] bool PatchUS() noexcept;
  const AttrHeaderCommon* HeaderCommon() noexcept;

  template <Strategy S>
  static FileRecordHeaderImpl<S> Factory(std::span<const BYTE> buffer,
                                         size_t sector_size);

  virtual const FileRecordHeader::Data* GetData() const = 0;
};

template <Strategy S>
struct FileRecordHeaderImpl
{
};

template <>
struct FileRecordHeaderImpl<Strategy::NO_CACHE> : public FileRecordHeader
{
  std::span<const BYTE> data_;

  FileRecordHeaderImpl(std::span<const BYTE> buffer, size_t sector_size);
  virtual ~FileRecordHeaderImpl() = default;

  const FileRecordHeader::Data* GetData() const override;
};

template <>
struct FileRecordHeaderImpl<Strategy::FULL_CACHE> : public FileRecordHeader
{
  FileRecordHeader::Data data_;

  FileRecordHeaderImpl(std::span<const BYTE> buffer, size_t sector_size);
  virtual ~FileRecordHeaderImpl() = default;

  const FileRecordHeader::Data* GetData() const override;
};

}  // namespace NtfsBrowser