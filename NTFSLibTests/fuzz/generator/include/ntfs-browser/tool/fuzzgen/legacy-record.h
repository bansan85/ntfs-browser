#pragma once

#include <ntfs-browser/win-types.h>

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

#include <ntfs-browser/namespace.h>

namespace NTFS_BROWSER_NAMESPACE::Tool::FuzzGen {

// Writes a file record in the layout of a real NTFS volume, as the author of
// the first hand-made inputs did: the update sequence array sits at offset
// 0x30 and the attributes start at 0x38. The fake builders put the array at
// the end of the record. The record spans two sectors of 512 bytes.
class LegacyRecord {
 public:
  // One run of a run list: clusters clusters at lcn, or a hole when lcn is
  // empty.
  struct Run {
    std::optional<ULONGLONG> lcn;
    ULONGLONG clusters = 0;
  };

  // One attribute of the record.
  struct Attribute {
    DWORD type = 0;
    // The attribute flags: 1 compressed, 0x4000 encrypted.
    WORD flags = 0;
    // The name of the attribute, empty for none.
    std::wstring name;
    // The offset of the name that the attribute declares. Empty means the end
    // of the header, and for a non-resident attribute without a name the end
    // of the common header. The builders write 0 for an attribute without a
    // name.
    std::optional<WORD> name_offset;
    // The instance number of the attribute in its record.
    WORD id = 0;
    // A resident attribute holds value. Otherwise the fields below apply.
    std::vector<BYTE> value;
    // The indexed flag of a resident attribute, which the $FILE_NAME of a
    // real volume sets.
    bool indexed = false;
    bool non_resident = false;
    ULONGLONG start_vcn = 0;
    ULONGLONG last_vcn = 0;
    // The size of a compression unit, as a power of two in clusters. Zero for
    // a stream that is not compressed.
    BYTE compression_unit = 0;
    ULONGLONG allocated_size = 0;
    ULONGLONG real_size = 0;
    ULONGLONG initialized_size = 0;
    // The compressed size of a compressed stream. Empty means the allocated
    // size.
    std::optional<ULONGLONG> compressed_size;
    // The runs, encoded by the writer. raw_runs replaces them when it is not
    // empty: the bytes of the run list, with the end byte if it has one.
    std::vector<Run> runs;
    std::vector<BYTE> raw_runs;
  };

  // One record.
  struct Spec {
    // The flags of the record: 1 in use, 2 directory.
    WORD flags = 1;
    // The $LogFile sequence number of the record.
    ULONGLONG log_sequence = 0;
    // The reference of the base record, zero for a base record.
    ULONGLONG base_reference = 0;
    // The sequence number of the record and its count of hard links.
    WORD sequence = 1;
    WORD links = 1;
    // Where the update sequence array sits, and the number it holds. The
    // builders of the fake volumes put the array at the end of the record,
    // with the number 0.
    WORD array_offset = 0x30;
    WORD update_sequence_number = 1;
    // The used size the record declares. Empty means the end of the
    // attributes, with the end marker.
    std::optional<DWORD> used_size;
    // The bytes the used size counts after the last attribute: the end marker
    // and, in most inputs, four bytes of padding. Some inputs count the marker
    // alone.
    DWORD used_tail = 8;
    // The four bytes after the end marker. Zero unless a real volume left a
    // value there.
    DWORD end_padding = 0;
    // The id that the next attribute of the record will get.
    WORD next_attribute_id = 0;
    // Where the first attribute starts, and the allocated size the record
    // declares.
    WORD first_attribute_offset = 0x38;
    DWORD allocated_size = 0x400;
    // The number of the record, which NTFS 3.1 keeps at offset 0x2C. Empty
    // leaves the field at zero.
    std::optional<DWORD> record_number;
    std::vector<Attribute> attributes;
  };

  // The bytes of the record that spec describes.
  [[nodiscard]] static std::vector<BYTE> Bytes(const Spec& spec);

  // Writes the record idx of image. The last word of each sector moves into the
  // update sequence array.
  static void Put(std::vector<BYTE>& image, ULONGLONG idx, const Spec& spec);

  // Writes the $MFT record (0) of a volume whose file records fill clusters
  // clusters, from the cluster of the $MFT on.
  static void PutMft(std::vector<BYTE>& image, ULONGLONG clusters);

  // The bytes of one attribute, padded to a multiple of 8.
  [[nodiscard]] static std::vector<BYTE>
      AttributeBytes(const Attribute& attribute);

  // The bytes of the run list of runs, with its end byte.
  [[nodiscard]] static std::vector<BYTE>
      EncodeRuns(const std::vector<Run>& runs);

  // The type codes of the attributes the inputs use.
  static constexpr DWORD type_standard_information = 0x10;
  static constexpr DWORD type_attribute_list = 0x20;
  static constexpr DWORD type_volume_information = 0x70;
  static constexpr DWORD type_data = 0x80;
  static constexpr DWORD type_bitmap = 0xB0;
  static constexpr DWORD type_logged_utility_stream = 0x100;

  // The attribute flag of a compressed stream.
  static constexpr WORD flag_compressed = 0x0001;

  // The attribute flag of an encrypted stream.
  static constexpr WORD flag_encrypted = 0x4000;
};

}  // namespace NTFS_BROWSER_NAMESPACE::Tool::FuzzGen
