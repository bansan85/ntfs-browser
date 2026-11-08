#include <ntfs-browser/win-types.h>

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <stdexcept>
#include <vector>

#include <ntfs-browser/namespace.h>
#include <ntfs-browser/test/fake-image/fake-ntfs-image.h>
#include <ntfs-browser/tool/fuzzgen/legacy-record.h>
#include <ntfs-browser/tool/fuzzgen/record-edit.h>
#include <ntfs-browser/tool/fuzzgen/update-sequence.h>

namespace NTFS_BROWSER_NAMESPACE::Tool::FuzzGen {

namespace {

namespace FakeImage = Test::FakeImage;

// The record is 1 KiB, in sectors of 512 bytes. The array holds the sequence
// number and one word per sector.
constexpr std::size_t record_bytes = FakeImage::fake_file_record_size;
constexpr std::size_t sector_bytes = 512;
constexpr WORD array_words = 1 + (record_bytes / sector_bytes);

// The cluster of the $MFT in every fake image.
constexpr ULONGLONG mft_cluster = 1;

// The "FILE" signature, read as a little-endian DWORD.
constexpr DWORD file_magic = 0x454C4946;

// The end of the attributes: the marker, and four more bytes. The record
// declares the whole as used.
constexpr DWORD end_marker = 0xFFFFFFFF;
constexpr std::size_t end_bytes = 8;

// Attributes start on a multiple of 8 bytes.
constexpr std::size_t alignment = 8;

// The header of a resident attribute, of a non-resident one, and of a
// non-resident one that is compressed (it adds the compressed size).
constexpr std::size_t resident_header_bytes = 0x18;
constexpr std::size_t non_resident_header_bytes = 0x40;
constexpr std::size_t compressed_header_bytes = 0x48;

// Offsets of the fields of the record header.
constexpr std::size_t field_magic = 0;
constexpr std::size_t field_array_offset = 4;
constexpr std::size_t field_array_words = 6;
constexpr std::size_t field_log_sequence = 8;
constexpr std::size_t field_sequence = 16;
constexpr std::size_t field_links = 18;
constexpr std::size_t field_attribute_offset = 20;
constexpr std::size_t field_flags = 22;
constexpr std::size_t field_used = 24;
constexpr std::size_t field_allocated = 28;
constexpr std::size_t field_base = 32;
constexpr std::size_t field_next_attribute = 40;
constexpr std::size_t field_record_number = 44;

// Offsets of the fields of an attribute header.
constexpr std::size_t attr_type = 0;
constexpr std::size_t attr_total = 4;
constexpr std::size_t attr_non_resident = 8;
constexpr std::size_t attr_name_length = 9;
constexpr std::size_t attr_id = 14;
constexpr std::size_t attr_name_offset = 10;
constexpr std::size_t attr_flags = 12;
constexpr std::size_t attr_value_length = 16;
constexpr std::size_t attr_value_offset = 20;
constexpr std::size_t attr_indexed = 22;
constexpr std::size_t attr_start_vcn = 16;
constexpr std::size_t attr_last_vcn = 24;
constexpr std::size_t attr_runs_offset = 32;
constexpr std::size_t attr_unit = 34;
constexpr std::size_t attr_allocated = 40;
constexpr std::size_t attr_real = 48;
constexpr std::size_t attr_initialized = 56;
constexpr std::size_t attr_compressed_size = 64;

// The bits of a run header: the byte count of the length field in the low
// nibble, that of the offset field in the high one.
constexpr unsigned run_offset_shift = 4;

std::size_t Align(std::size_t value) {
  return (value + alignment - 1) / alignment * alignment;
}

template <typename T>
void Store(std::vector<BYTE>& bytes, std::size_t offset, T value) {
  std::memcpy(&bytes.at(offset), &value, sizeof(T));
}

// The bytes of one number in the fewest bytes that hold it, as a signed or an
// unsigned number.
std::vector<BYTE> Minimal(ULONGLONG value, bool is_signed) {
  std::vector<BYTE> out;
  for (std::size_t i = 0; i < sizeof(ULONGLONG); ++i) {
    out.push_back(static_cast<BYTE>(value >> (8 * i)));
  }
  while (out.size() > 1) {
    const BYTE last = out.back();
    const BYTE before = out[out.size() - 2];
    const bool negative_before = (before & 0x80) != 0;
    const bool drop = is_signed ? (last == 0 && !negative_before) ||
                                      (last == 0xFF && negative_before)
                                : last == 0;
    if (!drop) {
      break;
    }
    out.pop_back();
  }
  return out;
}

// Writes the name of attr at offset.
void StoreName(std::vector<BYTE>& out, std::size_t offset,
               const LegacyRecord::Attribute& attr) {
  for (std::size_t i = 0; i < attr.name.size(); ++i) {
    Store<WORD>(out, offset + (i * sizeof(WORD)),
                static_cast<WORD>(attr.name[i]));
  }
}

// The bytes of a resident attribute.
std::vector<BYTE> ResidentBytes(const LegacyRecord::Attribute& attr) {
  const std::size_t name_bytes = attr.name.size() * sizeof(WORD);
  const std::size_t value_offset = Align(resident_header_bytes + name_bytes);
  std::vector<BYTE> out(Align(value_offset + attr.value.size()), 0);
  Store<DWORD>(out, attr_type, attr.type);
  Store<DWORD>(out, attr_total, static_cast<DWORD>(out.size()));
  Store<BYTE>(out, attr_name_length, static_cast<BYTE>(attr.name.size()));
  Store<WORD>(out, attr_name_offset,
              attr.name_offset.value_or(resident_header_bytes));
  Store<WORD>(out, attr_flags, attr.flags);
  Store<WORD>(out, attr_id, attr.id);
  Store<DWORD>(out, attr_value_length, static_cast<DWORD>(attr.value.size()));
  Store<WORD>(out, attr_value_offset, static_cast<WORD>(value_offset));
  Store<BYTE>(out, attr_indexed, attr.indexed ? 1 : 0);
  std::ranges::copy(attr.value,
                    out.begin() + static_cast<std::ptrdiff_t>(value_offset));
  StoreName(out, resident_header_bytes, attr);
  return out;
}

// The bytes of a non-resident attribute.
std::vector<BYTE> NonResidentBytes(const LegacyRecord::Attribute& attr) {
  const std::size_t name_bytes = attr.name.size() * sizeof(WORD);
  const std::size_t header = attr.compression_unit != 0
                                 ? compressed_header_bytes
                                 : non_resident_header_bytes;
  const std::vector<BYTE> runs = attr.raw_runs.empty()
                                     ? LegacyRecord::EncodeRuns(attr.runs)
                                     : attr.raw_runs;
  const std::size_t runs_offset = Align(header + name_bytes);
  std::vector<BYTE> out(Align(runs_offset + runs.size()), 0);
  Store<DWORD>(out, attr_type, attr.type);
  Store<DWORD>(out, attr_total, static_cast<DWORD>(out.size()));
  Store<BYTE>(out, attr_non_resident, 1);
  Store<BYTE>(out, attr_name_length, static_cast<BYTE>(attr.name.size()));
  // A stream without a name points its name at the end of the common header.
  Store<WORD>(out, attr_name_offset,
              attr.name_offset.value_or(static_cast<WORD>(
                  attr.name.empty() ? non_resident_header_bytes : header)));
  Store<WORD>(out, attr_flags, attr.flags);
  Store<WORD>(out, attr_id, attr.id);
  Store<ULONGLONG>(out, attr_start_vcn, attr.start_vcn);
  Store<ULONGLONG>(out, attr_last_vcn, attr.last_vcn);
  Store<WORD>(out, attr_runs_offset, static_cast<WORD>(runs_offset));
  Store<WORD>(out, attr_unit, attr.compression_unit);
  Store<ULONGLONG>(out, attr_allocated, attr.allocated_size);
  Store<ULONGLONG>(out, attr_real, attr.real_size);
  Store<ULONGLONG>(out, attr_initialized, attr.initialized_size);
  if (attr.compression_unit != 0) {
    Store<ULONGLONG>(out, attr_compressed_size,
                     attr.compressed_size.value_or(attr.allocated_size));
  }
  StoreName(out, header, attr);
  std::ranges::copy(runs,
                    out.begin() + static_cast<std::ptrdiff_t>(runs_offset));
  return out;
}

}  // namespace

std::vector<BYTE> LegacyRecord::AttributeBytes(const Attribute& attribute) {
  return attribute.non_resident ? NonResidentBytes(attribute)
                                : ResidentBytes(attribute);
}

std::vector<BYTE> LegacyRecord::EncodeRuns(const std::vector<Run>& runs) {
  std::vector<BYTE> out;
  ULONGLONG previous = 0;
  for (const Run& run : runs) {
    const std::vector<BYTE> length = Minimal(run.clusters, false);
    std::vector<BYTE> offset;
    if (run.lcn) {
      offset = Minimal(*run.lcn - previous, true);
      previous = *run.lcn;
    }
    out.push_back(
        static_cast<BYTE>((offset.size() << run_offset_shift) | length.size()));
    out.insert(out.end(), length.begin(), length.end());
    out.insert(out.end(), offset.begin(), offset.end());
  }
  out.push_back(0);
  return out;
}

std::vector<BYTE> LegacyRecord::Bytes(const Spec& spec) {
  std::vector<BYTE> record(record_bytes, 0);
  Store<DWORD>(record, field_magic, file_magic);
  Store<WORD>(record, field_array_offset, spec.array_offset);
  Store<WORD>(record, field_array_words, array_words);
  Store<ULONGLONG>(record, field_log_sequence, spec.log_sequence);
  Store<WORD>(record, field_sequence, spec.sequence);
  Store<WORD>(record, field_links, spec.links);
  Store<WORD>(record, field_attribute_offset, spec.first_attribute_offset);
  Store<WORD>(record, field_flags, spec.flags);
  Store<DWORD>(record, field_allocated, spec.allocated_size);
  Store<ULONGLONG>(record, field_base, spec.base_reference);
  Store<WORD>(record, field_next_attribute, spec.next_attribute_id);
  if (spec.record_number) {
    Store<DWORD>(record, field_record_number, *spec.record_number);
  }
  std::size_t at = spec.first_attribute_offset;
  for (const Attribute& attr : spec.attributes) {
    const std::vector<BYTE> bytes =
        attr.non_resident ? NonResidentBytes(attr) : ResidentBytes(attr);
    if (at + bytes.size() + end_bytes > record_bytes - sizeof(WORD)) {
      throw std::out_of_range("the attributes do not fit in the record");
    }
    std::ranges::copy(bytes, record.begin() + static_cast<std::ptrdiff_t>(at));
    at += bytes.size();
  }
  Store<DWORD>(record, at, end_marker);
  Store<DWORD>(record, at + sizeof(DWORD), spec.end_padding);
  Store<DWORD>(
      record, field_used,
      spec.used_size.value_or(static_cast<DWORD>(at + spec.used_tail)));
  // Each sector gives its last word to the array and takes the sequence
  // number in exchange.
  UpdateSequence::Protect(record, spec.array_offset,
                          spec.update_sequence_number);
  return record;
}

void LegacyRecord::Put(std::vector<BYTE>& image, ULONGLONG idx,
                       const Spec& spec) {
  const std::vector<BYTE> record = Bytes(spec);
  const std::size_t offset = RecordEdit::RecordOffset(idx);
  static_cast<void>(image.at(offset + record_bytes - 1));
  std::ranges::copy(record,
                    image.begin() + static_cast<std::ptrdiff_t>(offset));
}

void LegacyRecord::PutMft(std::vector<BYTE>& image, ULONGLONG clusters) {
  const ULONGLONG bytes = clusters * FakeImage::fake_cluster_size;
  Attribute data{.type = type_data,
                 .non_resident = true,
                 .last_vcn = clusters - 1,
                 .allocated_size = bytes,
                 .real_size = bytes,
                 .initialized_size = bytes,
                 .runs = {{.lcn = mft_cluster, .clusters = clusters}}};
  Put(image, 0, {.attributes = {data}});
}

}  // namespace NTFS_BROWSER_NAMESPACE::Tool::FuzzGen
