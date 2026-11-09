#pragma once

// Types of the Reader interface (see reader.h). They use no other type
// of the library, and nothing of <windows.h>. Dates are Date, and a
// file is named by a Handle, both from types.h.

#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>
#include <string>

#include <ntfs-browser/namespace.h>
#include <ntfs-browser/types.h>

namespace NTFS_BROWSER_NAMESPACE {

// Why a Reader call failed. The log (Log::Error, Log::Warn) holds the detail.
enum class ReadError : std::uint8_t {
  // The call went through.
  None,
  // The Reader could not open its target: it does not open, or holds no NTFS
  // volume. Every call fails with this.
  NotOpen,
  // The handle comes from another volume: its serial number is not the one of
  // the volume the Reader holds.
  WrongVolume,
  // The record number is past the end of the $MFT, or names a slot that holds
  // no
  // record that parses, or a record that is not the base record of a file.
  NoSuchRecord,
  // The record does not parse, or its attributes do not: nothing safe to read.
  RecordDamaged,
  // The record is another file's now: another file reused it since the handle
  // was made, or the handle says the file was deleted and the record is in use.
  // Also returned when a file was reused after it was freed and the sequence
  // number of the record is not the one of the handle.
  SequenceChanged,
  // The record has no such attribute.
  NoSuchAttribute,
  // The record has no such $DATA stream.
  NoSuchStream,
  // The data cannot be read: a damaged run list, a cluster that cannot be read,
  // a compressed stream in a build with no decompression.
  Unreadable,
  // The stream is encrypted (EFS) and cannot be read: no key opens it.
  Encrypted,
  // The sink of CopyTo() refused a chunk, which ends the copy.
  Aborted
};

// The answer of a Reader call: an error, and a value that only means something
// when the error is None.
template <typename T>
struct ReadResult {
  ReadError error{ReadError::None};
  T value{};

  // Whether the call went through.
  [[nodiscard]] constexpr bool Ok() const noexcept {
    return error == ReadError::None;
  }

  [[nodiscard]] explicit constexpr operator bool() const noexcept {
    return Ok();
  }
};

// What a Reader needs to know to open a target.
struct ReaderOptions {
  // What to read: a drive ("c:"), an image file or a device. As Options::target
  // of a Scan. A path inside the volume is ignored: a handle names its file by
  // record number.
  std::u16string target;
  // Salvage damaged records and attributes instead of refusing them. As
  // Options::recover_errors of a Scan.
  bool recover_errors{false};
};

// The attributes of $STANDARD_INFORMATION. The four times are in UTC, like
// every Date.
struct StandardInformation {
  Date created;
  Date modified;
  // The last change of the $MFT record, not of the content.
  Date mft_changed;
  Date accessed;
  // Bits of Attribute::.
  std::uint32_t attributes{0};
  // The fields below exist from NTFS 3.0 on. Zero on an older volume, with
  // has_extended false.
  bool has_extended{false};
  std::uint32_t max_versions{0};
  std::uint32_t version{0};
  std::uint32_t class_id{0};
  std::uint32_t owner_id{0};
  std::uint32_t security_id{0};
  std::uint64_t quota_charged{0};
  // The update sequence number of the change journal.
  std::uint64_t usn{0};
};

// The name space of a $FILE_NAME.
enum class NameSpace : std::uint8_t {
  // Any character but '\' and NUL. Two names of one folder MAY differ by case.
  Posix,
  // The long name of Windows.
  Win32,
  // The short 8.3 alias of another name.
  Dos,
  // A name that is both a long and a short name.
  Win32AndDos
};

// One $FILE_NAME of a record.
struct FileName {
  // Position among the names of the record that have a name. See
  // Handle::name_index.
  std::uint16_t index{0};
  // The name, in raw UTF-16 units as NTFS stores it.
  std::u16string name;
  // The directory the name is filed under, and its sequence number.
  std::uint64_t parent_record{0};
  std::uint16_t parent_sequence{0};
  NameSpace name_space{NameSpace::Win32};
  // The times that this copy of the name holds. NTFS refreshes them when the
  // file is renamed: they can lag the ones of StandardInformation.
  Date created;
  Date modified;
  Date mft_changed;
  Date accessed;
  // Size and allocated size that the name holds, which can lag as well.
  std::uint64_t size{0};
  std::uint64_t allocated_size{0};
  // Bits of Attribute::, the directory bit included.
  std::uint32_t attributes{0};
};

// One $DATA stream of a record.
struct StreamInfo {
  // The name of the stream. Empty for the unnamed stream, which is the content
  // of the file.
  std::u16string name;
  // Size of the stream in bytes, and bytes allocated on disk.
  std::uint64_t size{0};
  std::uint64_t allocated_size{0};
  // The stream sits in the record itself.
  bool resident{false};
  bool compressed{false};
  bool sparse{false};
  bool encrypted{false};
};

// The type code of an attribute that has no typed accessor here, as NTFS writes
// it: 0x10 $STANDARD_INFORMATION, 0x20 $ATTRIBUTE_LIST, 0x30 $FILE_NAME,
// 0x40 $OBJECT_ID, 0x50 $SECURITY_DESCRIPTOR, 0x60 $VOLUME_NAME,
// 0x70 $VOLUME_INFORMATION, 0x80 $DATA, 0x90 $INDEX_ROOT, 0xA0
// $INDEX_ALLOCATION, 0xB0 $BITMAP, 0xC0 $REPARSE_POINT, 0xD0 $EA_INFORMATION,
// 0xE0 $EA, 0x100 $LOGGED_UTILITY_STREAM.
using AttributeType = std::uint32_t;

// The value of AttributeKey::instance that matches any instance.
inline constexpr std::uint16_t any_instance = 0xFFFF;

// Which attribute of a record to read: its type, its name and its instance
// number. A record can hold several attributes of one type: they differ by
// name, and by instance when the name is the same.
struct AttributeKey {
  AttributeType type{0};
  // Empty for an unnamed attribute.
  std::u16string name;
  // any_instance takes the first attribute of the type and name.
  std::uint16_t instance{any_instance};
};

// One attribute of a record, as its header describes it.
struct AttributeInfo {
  AttributeType type{0};
  std::u16string name;
  std::uint16_t instance{0};
  // The content sits in the record itself.
  bool resident{false};
  // Size of the content in bytes, and bytes allocated on disk.
  std::uint64_t size{0};
  std::uint64_t allocated_size{0};
  // The flags word of the attribute header: 0x0001 compressed, 0x4000
  // encrypted, 0x8000 sparse.
  std::uint16_t flags{0};
};

// Where CopyTo() puts the bytes. Called with each chunk in order. Returns false
// to stop: the copy then fails with ReadError::Aborted. The chunk is valid
// during the call only.
using ByteSink = std::function<bool(std::span<const std::uint8_t> chunk)>;

// Bytes that CopyTo() hands to its sink at a time. 64 KiB keeps the memory of a
// copy small whatever the size of the file, and is a multiple of every cluster
// size.
inline constexpr std::size_t copy_chunk_bytes = 64 * 1024;

}  // namespace NTFS_BROWSER_NAMESPACE
