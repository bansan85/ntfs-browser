#pragma once

#include <ntfs-browser/win-types.h>

#include <cstddef>
#include <cstring>
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
  static constexpr DWORD type_index_root = 0x90;
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

  // Cuts the last attribute of a record, at attr, to total_size bytes, and
  // moves the end marker up to follow it. The padding bytes go. Throws
  // std::out_of_range when the marker no longer fits.
  static void ShrinkLastAttribute(std::vector<BYTE>& image, std::size_t attr,
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

  // Reads the structure T that starts at offset, lets change edit its named
  // fields, and writes it back. T is a packed on-disk structure of the
  // library. Throws std::out_of_range when it does not fit in the image.
  template <typename T, typename Change>
  static void Edit(std::vector<BYTE>& image, std::size_t offset,
                   const Change& change) {
    static_cast<void>(image.at(offset + sizeof(T) - 1));
    T value{};
    std::memcpy(&value, &image[offset], sizeof(T));
    change(value);
    std::memcpy(&image[offset], &value, sizeof(T));
  }

  // Byte offset of the first index entry of the $INDEX_ROOT at attr.
  [[nodiscard]] static std::size_t
      FirstIndexRootEntry(const std::vector<BYTE>& image, std::size_t attr);

  // Replaces the bytes of the run list of the non-resident attribute at attr.
  static void SetRunList(std::vector<BYTE>& image, std::size_t attr,
                         const std::vector<BYTE>& runs);

  // Replaces the run list of the non-resident attribute at attr, and gives the
  // attribute the size of its header and the new list, rounded up to 8 bytes.
  // The attributes after it move with it. Throws std::out_of_range when the
  // record cannot hold them.
  static void ReplaceRunList(std::vector<BYTE>& image, std::size_t attr,
                             const std::vector<BYTE>& runs);

  // Puts a resident attribute after the last one of record idx, then moves
  // the end marker after it. The attribute is as long as its header and its
  // value, with no padding. Returns the offset of the attribute. Throws
  // std::out_of_range when it does not fit in the record.
  static std::size_t AppendResident(std::vector<BYTE>& image, ULONGLONG idx,
                                    DWORD type, WORD id,
                                    const std::vector<BYTE>& value);

  // Puts a non-resident attribute after the last one of record idx, with a
  // run list that is exactly runs (the terminating zero byte is part of runs
  // when the list has one). The attribute spans VCN 0 to last_vcn. Returns
  // the offset of the attribute.
  static std::size_t
      AppendNonResident(std::vector<BYTE>& image, ULONGLONG idx, DWORD type,
                        const std::vector<BYTE>& runs, ULONGLONG last_vcn,
                        ULONGLONG allocated_size, ULONGLONG real_size);

  // Removes every attribute of record idx.
  static void ClearAttributes(std::vector<BYTE>& image, ULONGLONG idx);

  // Copies record from onto record to.
  static void CopyRecord(std::vector<BYTE>& image, ULONGLONG from,
                         ULONGLONG to);

  // Copies record from of the image donor onto record to of image.
  static void CopyRecordFrom(std::vector<BYTE>& image,
                             const std::vector<BYTE>& donor, ULONGLONG from,
                             ULONGLONG to);

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
