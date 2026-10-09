#pragma once

#include <ntfs-browser/win-types.h>

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include <ntfs-browser/strategy.h>

#include "../internal-export.h"

namespace NtfsBrowser {
namespace Flag {

enum class FileRecord : std::uint8_t;

}  // namespace Flag

struct AttrHeaderCommon;
template <Strategy S>
struct FileRecordHeaderImpl;

struct NTFS_BROWSER_EXPORT_TESTS_ONLY FileRecordHeader {
  // The "FILE" signature that opens a file record, read as a little-endian
  // DWORD.
  static constexpr uint32_t file_record_magic = 'ELIF';

  // Size of Data's named header fields, before the first attribute begins.
  static constexpr size_t min_file_record_header_size = 48;

  // Largest file record size a real NTFS volume can have (a 4Kn volume's).
  static constexpr size_t max_file_record_size = 4096;

  // NTFS protects every 512-byte block of a record or index block with one
  // update sequence word, whatever the volume's sector size (a 4Kn volume
  // included).
  static constexpr size_t update_sequence_stride = 512;

  // Number of 512-byte blocks a buffer's update sequence array covers.
  // size_of_us counts the sequence number itself. It bounds the result, so a
  // forged value cannot make a caller read more array words than the header
  // declares. A shorter array only protects the blocks it covers.
  static constexpr size_t UpdateSequenceBlockCount(size_t buffer_size,
                                                   WORD size_of_us) noexcept {
    const size_t declared = size_of_us > 0 ? size_of_us - 1U : 0U;
    const size_t blocks = buffer_size / update_sequence_stride;
    return declared < blocks ? declared : blocks;
  }

  union Data {
    struct {
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

    // NOLINTNEXTLINE(cppcoreguidelines-avoid-c-arrays,modernize-avoid-c-arrays)
    BYTE raw[max_file_record_size];
  };

  WORD us_number{0};
  std::vector<WORD> us_array;
  // Actual buffer size this instance was constructed with.
  size_t buffer_size;

  explicit FileRecordHeader(std::span<const BYTE> buffer);
  FileRecordHeader(const FileRecordHeader&) = delete;
  FileRecordHeader& operator=(const FileRecordHeader&) = delete;
  FileRecordHeader(FileRecordHeader&&) = delete;
  FileRecordHeader& operator=(FileRecordHeader&&) = delete;
  virtual ~FileRecordHeader() = default;
  // Verify US and update sectors
  [[nodiscard]] bool PatchUS() noexcept;
  // Returns nullptr if offset_of_attr doesn't fit in the record buffer.
  [[nodiscard]] const AttrHeaderCommon* HeaderCommon() const noexcept;

  [[nodiscard]] virtual const FileRecordHeader::Data* GetData() const = 0;
};

template <Strategy S>
struct FileRecordHeaderImpl {};

template <>
struct NTFS_BROWSER_EXPORT_TESTS_ONLY
    FileRecordHeaderImpl<Strategy::NoCache> : public FileRecordHeader {
  std::span<const BYTE> data;

  explicit FileRecordHeaderImpl(std::span<const BYTE> buffer);
  FileRecordHeaderImpl(const FileRecordHeaderImpl&) = delete;
  FileRecordHeaderImpl& operator=(const FileRecordHeaderImpl&) = delete;
  FileRecordHeaderImpl(FileRecordHeaderImpl&&) = delete;
  FileRecordHeaderImpl& operator=(FileRecordHeaderImpl&&) = delete;
  ~FileRecordHeaderImpl() override = default;

  [[nodiscard]] const FileRecordHeader::Data* GetData() const override;
};

template <>
struct NTFS_BROWSER_EXPORT_TESTS_ONLY
    FileRecordHeaderImpl<Strategy::FullCache> : public FileRecordHeader {
  FileRecordHeader::Data data{};

  explicit FileRecordHeaderImpl(std::span<const BYTE> buffer);
  FileRecordHeaderImpl(const FileRecordHeaderImpl&) = delete;
  FileRecordHeaderImpl& operator=(const FileRecordHeaderImpl&) = delete;
  FileRecordHeaderImpl(FileRecordHeaderImpl&&) = delete;
  FileRecordHeaderImpl& operator=(FileRecordHeaderImpl&&) = delete;
  ~FileRecordHeaderImpl() override = default;

  [[nodiscard]] const FileRecordHeader::Data* GetData() const override;
};

}  // namespace NtfsBrowser
