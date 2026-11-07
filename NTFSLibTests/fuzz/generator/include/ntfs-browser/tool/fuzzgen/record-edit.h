#pragma once

#include <ntfs-browser/win-types.h>

#include <cstddef>
#include <vector>

#include <ntfs-browser/namespace.h>

namespace NTFS_BROWSER_NAMESPACE::Tool::FuzzGen {

// Edits the fields of the file records of a fake image by name, so that a
// damaged record reads as the field that is wrong, not as a table of offsets.
// The records are those of the 26-slot base: 1 KiB each, from the cluster of
// the $MFT on.
class RecordEdit {
 public:
  // Attribute type codes the edits look for.
  static constexpr DWORD type_file_name = 0x30;
  static constexpr DWORD type_volume_information = 0x70;
  static constexpr DWORD type_data = 0x80;
  static constexpr DWORD type_index_allocation = 0xA0;

  // Byte offset of record idx in the image.
  [[nodiscard]] static std::size_t RecordOffset(ULONGLONG idx);

  // Byte offset of the nth attribute of type in record idx. Throws
  // std::out_of_range when the record has fewer.
  [[nodiscard]] static std::size_t Attribute(const std::vector<BYTE>& image,
                                             ULONGLONG idx, DWORD type,
                                             std::size_t nth = 0);

  // Byte offset of the value of the resident attribute at attr.
  [[nodiscard]] static std::size_t ValueOffset(const std::vector<BYTE>& image,
                                               std::size_t attr);

  // Overwrites the "FILE" magic of record idx, so it no longer parses.
  static void BreakMagic(std::vector<BYTE>& image, ULONGLONG idx);

  // Sets the total_size field of the attribute header at attr.
  static void SetTotalSize(std::vector<BYTE>& image, std::size_t attr,
                           DWORD total_size);

  // Sets the non-resident flag of the attribute header at attr.
  static void SetNonResidentFlag(std::vector<BYTE>& image, std::size_t attr,
                                 BYTE flag);

  // Sets the attribute type of the header at attr.
  static void SetType(std::vector<BYTE>& image, std::size_t attr, DWORD type);

  // Sets the start VCN of the non-resident attribute at attr.
  static void SetStartVcn(std::vector<BYTE>& image, std::size_t attr,
                          ULONGLONG vcn);

  // Sets the length of the value of the resident attribute at attr.
  static void SetValueLength(std::vector<BYTE>& image, std::size_t attr,
                             DWORD length);

  // Sets the name length (in characters) of the $FILE_NAME at attr.
  static void SetFileNameLength(std::vector<BYTE>& image, std::size_t attr,
                                BYTE length);

  // Sets the parent reference of the $FILE_NAME at attr.
  static void SetFileNameParent(std::vector<BYTE>& image, std::size_t attr,
                                ULONGLONG record, WORD sequence);

  // Writes value at offset of the image. Throws std::out_of_range when the
  // value does not fit.
  static void StoreByte(std::vector<BYTE>& image, std::size_t offset,
                        BYTE value);
  static void StoreWord(std::vector<BYTE>& image, std::size_t offset,
                        WORD value);
  static void StoreDword(std::vector<BYTE>& image, std::size_t offset,
                         DWORD value);
  static void StoreQword(std::vector<BYTE>& image, std::size_t offset,
                         ULONGLONG value);
};

}  // namespace NTFS_BROWSER_NAMESPACE::Tool::FuzzGen
