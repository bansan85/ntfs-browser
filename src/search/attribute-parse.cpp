#include "search/attribute-parse.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <span>
#include <string>

#include <ntfs-browser/namespace.h>
#include <ntfs-browser/reader-types.h>
#include <ntfs-browser/types.h>

namespace NTFS_BROWSER_NAMESPACE {

namespace {

// Bytes of $STANDARD_INFORMATION before NTFS 3: the four times and the flags.
constexpr std::size_t std_info_old_size = 48;

// Bytes of $STANDARD_INFORMATION from NTFS 3 on: the old fields plus six more.
constexpr std::size_t std_info_size = 72;

// Bytes of a $FILE_NAME before the name: the fixed part.
constexpr std::size_t file_name_fixed_size = 66;

// Mask of the attribute bits of a flags word: the low 16 bits, which are the
// Win32 FILE_ATTRIBUTE_* values.
constexpr std::uint32_t attribute_bits = 0xFFFF;

// The bit of a $FILE_NAME flags word that marks a directory. It sits above the
// Win32 bits and has no FILE_ATTRIBUTE_* value of its own.
constexpr std::uint32_t file_name_directory_bit = 0x10000000;

// Offsets of the fields of $STANDARD_INFORMATION.
constexpr std::size_t std_created = 0x00;
constexpr std::size_t std_modified = 0x08;
constexpr std::size_t std_mft_changed = 0x10;
constexpr std::size_t std_accessed = 0x18;
constexpr std::size_t std_flags = 0x20;
constexpr std::size_t std_max_versions = 0x24;
constexpr std::size_t std_version = 0x28;
constexpr std::size_t std_class_id = 0x2C;
constexpr std::size_t std_owner_id = 0x30;
constexpr std::size_t std_security_id = 0x34;
constexpr std::size_t std_quota = 0x38;
constexpr std::size_t std_usn = 0x40;

// Offsets of the fields of $FILE_NAME.
constexpr std::size_t name_parent = 0x00;
constexpr std::size_t name_created = 0x08;
constexpr std::size_t name_modified = 0x10;
constexpr std::size_t name_mft_changed = 0x18;
constexpr std::size_t name_accessed = 0x20;
constexpr std::size_t name_allocated = 0x28;
constexpr std::size_t name_size = 0x30;
constexpr std::size_t name_flags = 0x38;
constexpr std::size_t name_length = 0x40;
constexpr std::size_t name_space_offset = 0x41;

// The little-endian integer of type T at offset of bytes. The caller checked
// the bounds.
template <typename T>
T LoadAt(std::span<const std::uint8_t> bytes, std::size_t offset) {
  T value{};
  std::memcpy(&value, bytes.data() + offset, sizeof(T));
  return value;
}

// The date of 8 bytes at offset of bytes. The caller checked the bounds.
Date LoadDate(std::span<const std::uint8_t> bytes, std::size_t offset) {
  return {.ticks = LoadAt<std::uint64_t>(bytes, offset)};
}

// The attribute bits of a $FILE_NAME flags word: the Win32 bits, and the
// directory bit of the record moved to Attribute::directory.
std::uint32_t FileNameAttributes(std::uint32_t flags) {
  return (flags & attribute_bits) |
         ((flags & file_name_directory_bit) != 0 ? Attribute::directory : 0U);
}

// The name space byte of a $FILE_NAME, as NameSpace. A value past 3 is a
// damaged record and reads as Posix, the space that has no rule.
NameSpace NameSpaceOf(std::uint8_t value) {
  switch (value) {
    case 1:
      return NameSpace::Win32;
    case 2:
      return NameSpace::Dos;
    case 3:
      return NameSpace::Win32AndDos;
    default:
      break;
  }
  return NameSpace::Posix;
}

}  // namespace

std::optional<StandardInformation>
    ParseStandardInformation(std::span<const std::uint8_t> content) {
  if (content.size() < std_info_old_size) {
    return std::nullopt;
  }
  StandardInformation info;
  info.created = LoadDate(content, std_created);
  info.modified = LoadDate(content, std_modified);
  info.mft_changed = LoadDate(content, std_mft_changed);
  info.accessed = LoadDate(content, std_accessed);
  info.attributes = LoadAt<std::uint32_t>(content, std_flags) & attribute_bits;
  if (content.size() >= std_info_size) {
    info.has_extended = true;
    info.max_versions = LoadAt<std::uint32_t>(content, std_max_versions);
    info.version = LoadAt<std::uint32_t>(content, std_version);
    info.class_id = LoadAt<std::uint32_t>(content, std_class_id);
    info.owner_id = LoadAt<std::uint32_t>(content, std_owner_id);
    info.security_id = LoadAt<std::uint32_t>(content, std_security_id);
    info.quota_charged = LoadAt<std::uint64_t>(content, std_quota);
    info.usn = LoadAt<std::uint64_t>(content, std_usn);
  }
  return info;
}

std::optional<FileName> ParseFileName(std::span<const std::uint8_t> content,
                                      std::uint16_t index) {
  if (content.size() < file_name_fixed_size) {
    return std::nullopt;
  }
  const std::size_t units = content[name_length];
  if (units == 0 || content.size() < file_name_fixed_size + units * 2) {
    return std::nullopt;
  }
  FileName name;
  name.index = index;
  name.parent_record =
      LoadAt<std::uint64_t>(content, name_parent) & 0x0000FFFFFFFFFFFFULL;
  name.parent_sequence = LoadAt<std::uint16_t>(content, name_parent + 6);
  name.name_space = NameSpaceOf(content[name_space_offset]);
  name.created = LoadDate(content, name_created);
  name.modified = LoadDate(content, name_modified);
  name.mft_changed = LoadDate(content, name_mft_changed);
  name.accessed = LoadDate(content, name_accessed);
  name.allocated_size = LoadAt<std::uint64_t>(content, name_allocated);
  name.size = LoadAt<std::uint64_t>(content, name_size);
  name.attributes =
      FileNameAttributes(LoadAt<std::uint32_t>(content, name_flags));
  name.name.resize(units);
  std::memcpy(name.name.data(), content.data() + file_name_fixed_size,
              units * 2);
  return name;
}

}  // namespace NTFS_BROWSER_NAMESPACE
