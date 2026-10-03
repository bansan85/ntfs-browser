#pragma once

#include <ntfs-browser/win-types.h>

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include <ntfs-browser/strategy.h>

#include "../internal-export.h"

namespace NtfsBrowser
{
namespace Flag
{
enum class FileRecord : std::uint8_t;
}  // namespace Flag

constexpr uint32_t kFileRecordMagic('ELIF');

// Size of Data's named header fields, before the first attribute begins.
constexpr size_t kMinFileRecordHeaderSize = 48;

// Largest file record size a real NTFS volume can have (a 4Kn volume's).
constexpr size_t kMaxFileRecordSize = 4096;

// NTFS protects every 512-byte block of a record or index block with one
// update sequence word, whatever the volume's sector size (a 4Kn volume
// included).
constexpr size_t kUpdateSequenceStride = 512;

// Number of 512-byte blocks a buffer's update sequence array covers.
// size_of_us counts the sequence number itself. It bounds the result, so a
// forged value cannot make a caller read more array words than the header
// declares. A shorter array only protects the blocks it covers.
constexpr size_t UpdateSequenceBlockCount(size_t buffer_size,
                                          WORD size_of_us) noexcept
{
  const size_t declared = size_of_us > 0 ? size_of_us - 1U : 0U;
  const size_t blocks = buffer_size / kUpdateSequenceStride;
  return declared < blocks ? declared : blocks;
}

struct AttrHeaderCommon;
template <Strategy S>
struct FileRecordHeaderImpl;

struct NTFS_BROWSER_EXPORT_TESTS_ONLY FileRecordHeader
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
  // Actual buffer size this instance was constructed with.
  size_t buffer_size_;

  explicit FileRecordHeader(std::span<const BYTE> buffer);
  FileRecordHeader(const FileRecordHeader&) = delete;
  FileRecordHeader& operator=(const FileRecordHeader&) = delete;
  FileRecordHeader(FileRecordHeader&&) = delete;
  FileRecordHeader& operator=(FileRecordHeader&&) = delete;
  virtual ~FileRecordHeader() = default;
  // Verify US and update sectors
  [[nodiscard]] bool PatchUS() noexcept;
  // Returns nullptr if offset_of_attr doesn't fit in the record buffer.
  const AttrHeaderCommon* HeaderCommon() noexcept;

  virtual const FileRecordHeader::Data* GetData() const = 0;
};

template <Strategy S>
struct FileRecordHeaderImpl
{
};

template <>
struct NTFS_BROWSER_EXPORT_TESTS_ONLY
    FileRecordHeaderImpl<Strategy::NO_CACHE> : public FileRecordHeader
{
  std::span<const BYTE> data_;

  explicit FileRecordHeaderImpl(std::span<const BYTE> buffer);
  FileRecordHeaderImpl(const FileRecordHeaderImpl&) = delete;
  FileRecordHeaderImpl& operator=(const FileRecordHeaderImpl&) = delete;
  FileRecordHeaderImpl(FileRecordHeaderImpl&&) = delete;
  FileRecordHeaderImpl& operator=(FileRecordHeaderImpl&&) = delete;
  ~FileRecordHeaderImpl() override = default;

  const FileRecordHeader::Data* GetData() const override;
};

template <>
struct NTFS_BROWSER_EXPORT_TESTS_ONLY
    FileRecordHeaderImpl<Strategy::FULL_CACHE> : public FileRecordHeader
{
  FileRecordHeader::Data data_;

  explicit FileRecordHeaderImpl(std::span<const BYTE> buffer);
  FileRecordHeaderImpl(const FileRecordHeaderImpl&) = delete;
  FileRecordHeaderImpl& operator=(const FileRecordHeaderImpl&) = delete;
  FileRecordHeaderImpl(FileRecordHeaderImpl&&) = delete;
  FileRecordHeaderImpl& operator=(FileRecordHeaderImpl&&) = delete;
  ~FileRecordHeaderImpl() override = default;

  const FileRecordHeader::Data* GetData() const override;
};

}  // namespace NtfsBrowser