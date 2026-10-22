#include "record/slack-carver.h"

#include <ntfs-browser/win-types.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstring>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <ntfs-browser/namespace.h>
#include <ntfs-browser/record/carved-name.h>

#include "data/filename.h"
#include "mft/file-reference.h"
#include "utf/utf.h"

namespace NTFS_BROWSER_NAMESPACE::Record {

namespace {

// Where the fields the carver reads sit in a $FILE_NAME.
constexpr size_t parent_ref_offset = offsetof(Data::Filename, parent_ref);
constexpr size_t create_time_offset = offsetof(Data::Filename, create_time);
constexpr size_t alter_time_offset = offsetof(Data::Filename, alter_time);
constexpr size_t mft_time_offset = offsetof(Data::Filename, mft_time);
constexpr size_t read_time_offset = offsetof(Data::Filename, read_time);
constexpr size_t flags_offset = offsetof(Data::Filename, flags);
constexpr size_t name_length_offset = offsetof(Data::Filename, name_length);
constexpr size_t name_space_offset = offsetof(Data::Filename, name_space);
constexpr size_t name_offset = offsetof(Data::Filename, name);

// The scan anchors on the length byte and the namespace byte, which sit
// together right before the name.
static_assert(name_space_offset == name_length_offset + 1);
static_assert(name_offset == name_space_offset + 1);

// Size of the end marker word (Attr::Type::All).
constexpr size_t end_marker_size = 4;

// Size of the end marker plus the four bytes NTFS writes after it. The used
// size of a record ends after them, and they hold no leftover: whatever sits
// there belongs to the live record.
constexpr size_t live_terminator_size = 8;

// The scan moves 2 bytes at a time: a name is UTF-16, so its first character
// sits on an even offset whenever the structure does. The leftovers seen on
// real volumes start on a multiple of 4, not always of 8, so a step of 8 would
// miss some; 2 is the largest step the format itself guarantees.
constexpr size_t scan_step = 2;

// Bytes of the shortest candidate the scan can test: the length byte, the
// namespace byte and one character.
constexpr size_t shortest_core = name_offset - name_length_offset + 2;

// Largest namespace byte NTFS writes: Win32 and DOS together.
constexpr BYTE max_name_space = 3;

// A partial candidate has lost the start of its structure, so its name alone
// must carry the proof. Fewer characters match too many random bytes.
constexpr size_t min_partial_name_length = 5;

// Earliest time a candidate may hold: 1980-01-01 UTC, in 100 ns ticks since
// 1601. It is the first date a FAT timestamp, and so a file copied from one,
// can keep.
constexpr ULONGLONG min_plausible_time = 119'600'064'000'000'000ULL;

// Latest time a candidate may hold: 2100-01-01 UTC. Four 64-bit values all in
// this 120-year window match random bytes about once in 10^10.
constexpr ULONGLONG max_plausible_time = 157'469'184'000'000'000ULL;

// A parent record number from this value up is not a record number: the file
// record header holds 32 bits, and a low half of all ones is what an older end
// marker leaves in a reference it overwrote.
constexpr ULONGLONG first_impossible_parent = 0xFFFFFFFFULL;

// The attribute flag bits NTFS sets in a $FILE_NAME: the Windows file
// attributes (0x0000 to 0xFFFF, the low half of them in use) and the two
// index-only bits (directory, index view). Random flags fall inside about once
// in 2^14.
constexpr DWORD known_flag_bits = 0x3000FFFF;

// Bits in a DWORD.
constexpr unsigned dword_bits = 32;

// Surrogate halves of UTF-16.
constexpr char16_t high_surrogate_first = 0xD800;
constexpr char16_t high_surrogate_last = 0xDBFF;
constexpr char16_t low_surrogate_first = 0xDC00;
constexpr char16_t low_surrogate_last = 0xDFFF;

// A range of UTF-16 code units.
struct UnitRange {
  char16_t first;
  char16_t last;
};

// The blocks a name with no other proof than its characters may use: Basic
// Latin, Latin supplements and extensions, Greek to Arabic, kana to CJK,
// Hangul and fullwidth forms. About half of random 16-bit units fall outside,
// which makes 5 in a row unlikely by chance.
constexpr std::array<UnitRange, 6> plain_text_ranges{{{0x20, 0x7E},
                                                      {0xA0, 0x24F},
                                                      {0x370, 0x6FF},
                                                      {0x3040, 0x9FFF},
                                                      {0xAC00, 0xD7A3},
                                                      {0xFF01, 0xFF5E}}};

// Reads a T at offset of bytes. The caller keeps offset + sizeof(T) in range.
template <typename T>
T Load(std::span<const BYTE> bytes, size_t offset) noexcept {
  T value{};
  std::memcpy(&value, bytes.subspan(offset, sizeof(T)).data(), sizeof(T));
  return value;
}

bool IsHighSurrogate(char16_t unit) noexcept {
  return unit >= high_surrogate_first && unit <= high_surrogate_last;
}

bool IsLowSurrogate(char16_t unit) noexcept {
  return unit >= low_surrogate_first && unit <= low_surrogate_last;
}

// Whether a name may hold unit: no control character, no path separator, no
// noncharacter. A surrogate is judged in pairs, by IsValidName().
bool IsNameUnit(char16_t unit) noexcept {
  constexpr char16_t first_printable = 0x20;
  constexpr char16_t delete_char = 0x7F;
  constexpr char16_t last_c1_control = 0x9F;
  constexpr char16_t noncharacter_low = 0xFFFE;
  constexpr char16_t noncharacter_high = 0xFFFF;
  return unit >= first_printable && unit != delete_char &&
         !(unit > delete_char && unit <= last_c1_control) && unit != u'/' &&
         unit != u'\\' && unit != noncharacter_low && unit != noncharacter_high;
}

bool IsPlainTextUnit(char16_t unit) noexcept {
  return std::ranges::any_of(plain_text_ranges, [unit](const UnitRange& range) {
    return unit >= range.first && unit <= range.last;
  });
}

// Whether units is a well-formed UTF-16 name.
bool IsValidName(std::u16string_view units) noexcept {
  for (auto unit = units.begin(); unit != units.end(); ++unit) {
    if (IsHighSurrogate(*unit)) {
      ++unit;
      if (unit == units.end() || !IsLowSurrogate(*unit)) {
        return false;
      }
    } else if (IsLowSurrogate(*unit) || !IsNameUnit(*unit)) {
      return false;
    }
  }
  return true;
}

bool IsPlausibleTime(ULONGLONG ticks) noexcept {
  return ticks >= min_plausible_time && ticks < max_plausible_time;
}

FILETIME ToFileTime(ULONGLONG ticks) noexcept {
  return {.dwLowDateTime = static_cast<DWORD>(ticks),
          .dwHighDateTime = static_cast<DWORD>(ticks >> dword_bits)};
}

// A candidate, and where its name ends.
struct Found {
  CarvedName name;
  // End (exclusive) of the name's characters in the record.
  size_t end{0};
};

// Whether the field at field_offset of a $FILE_NAME whose length byte sits at
// length_at lies in the bytes the record still owns to the leftover, that is
// at or past trusted_begin.
bool IsTrusted(size_t length_at, size_t field_offset,
               size_t trusted_begin) noexcept {
  return length_at >= trusted_begin + (name_length_offset - field_offset);
}

// Where the field at field_offset sits, for a $FILE_NAME whose length byte is
// at length_at. IsTrusted() MUST hold.
size_t FieldAt(size_t length_at, size_t field_offset) noexcept {
  return length_at - (name_length_offset - field_offset);
}

// Reads the four times of the structure into name. False when a time is not
// plausible: the bytes are then not a $FILE_NAME.
bool ReadTimes(std::span<const BYTE> record, size_t length_at,
               CarvedName& name) {
  const auto time_at = [&](size_t field_offset) {
    return Load<ULONGLONG>(record, FieldAt(length_at, field_offset));
  };
  const std::array<ULONGLONG, 4> ticks{
      time_at(create_time_offset), time_at(alter_time_offset),
      time_at(mft_time_offset), time_at(read_time_offset)};
  if (!std::ranges::all_of(ticks, IsPlausibleTime)) {
    return false;
  }
  name.create_time = ToFileTime(ticks.at(0));
  name.write_time = ToFileTime(ticks.at(1));
  name.change_time = ToFileTime(ticks.at(2));
  name.access_time = ToFileTime(ticks.at(3));
  name.times_known = true;
  return true;
}

// Reads the parent reference into name. A reference to record 0 ($MFT) is
// what a zeroed one looks like, and no file lives in $MFT. A record number that
// cannot exist is what an overwritten one looks like. Either stays unknown.
void ReadParent(std::span<const BYTE> record, size_t length_at,
                CarvedName& name) {
  const ULONGLONG reference =
      Load<ULONGLONG>(record, FieldAt(length_at, parent_ref_offset));
  const ULONGLONG parent = reference & Mft::mft_record_number_mask;
  if (parent == 0 || parent >= first_impossible_parent) {
    return;
  }
  name.parent_record = parent;
  name.parent_sequence =
      static_cast<WORD>(reference >> Mft::mft_sequence_shift);
  name.parent_known = true;
}

// Tests the bytes with a $FILE_NAME length byte at length_at. Nullopt when
// they are not a candidate. trusted_begin is the first offset of the leftover.
std::optional<Found> ProbeAt(std::span<const BYTE> record, size_t length_at,
                             size_t trusted_begin) {
  const size_t length = Load<BYTE>(record, length_at);
  const BYTE name_space = Load<BYTE>(record, length_at + 1);
  const size_t name_first = length_at + (name_offset - name_length_offset);
  const size_t end = name_first + (length * sizeof(char16_t));
  if (length == 0 || name_space > max_name_space || end > record.size()) {
    return std::nullopt;
  }

  std::u16string units(length, u'\0');
  std::memcpy(units.data(),
              record.subspan(name_first, length * sizeof(char16_t)).data(),
              length * sizeof(char16_t));
  if (!IsValidName(units)) {
    return std::nullopt;
  }

  Found found;
  found.end = end;
  CarvedName& name = found.name;
  name.name_space = name_space;

  const bool flags_trusted = IsTrusted(length_at, flags_offset, trusted_begin);
  if (flags_trusted) {
    const DWORD flags = Load<DWORD>(record, FieldAt(length_at, flags_offset));
    if ((flags & ~known_flag_bits) != 0) {
      return std::nullopt;
    }
  }
  if (IsTrusted(length_at, create_time_offset, trusted_begin) &&
      !ReadTimes(record, length_at, name)) {
    return std::nullopt;
  }
  if (IsTrusted(length_at, parent_ref_offset, trusted_begin)) {
    ReadParent(record, length_at, name);
  }

  name.partial = !name.parent_known || !name.times_known;
  if (name.partial) {
    if (length < min_partial_name_length) {
      return std::nullopt;
    }
    // Not even the attribute flags survive: the characters are all there is.
    if (!flags_trusted && !std::ranges::all_of(units, IsPlainTextUnit)) {
      return std::nullopt;
    }
  }
  name.name = Utf::Utf16ToWide(units);
  return found;
}

bool SameTime(const FILETIME& left, const FILETIME& right) noexcept {
  return left.dwLowDateTime == right.dwLowDateTime &&
         left.dwHighDateTime == right.dwHighDateTime;
}

// The scan both public entry points share. It steps from scan_begin, rounded
// up to an even offset, and trusts only the fields at or after trusted_begin.
std::vector<CarvedName> CarveFrom(std::span<const BYTE> bytes,
                                  size_t scan_begin, size_t trusted_begin) {
  std::vector<CarvedName> carved;
  size_t at = (scan_begin + 1) & ~size_t{1};
  while (at + shortest_core <= bytes.size()) {
    std::optional<Found> found = ProbeAt(bytes, at, trusted_begin);
    if (!found) {
      at += scan_step;
      continue;
    }
    // A name is at least 4 bytes long: the scan always moves forward.
    at = found->end;
    if (std::ranges::none_of(carved, [&](const CarvedName& seen) {
          return SameCarvedName(seen, found->name);
        })) {
      carved.push_back(std::move(found->name));
    }
  }
  return carved;
}

}  // namespace

bool SameCarvedName(const CarvedName& left, const CarvedName& right) noexcept {
  return left.name == right.name && left.parent_record == right.parent_record &&
         left.parent_sequence == right.parent_sequence &&
         left.parent_known == right.parent_known &&
         left.times_known == right.times_known &&
         left.name_space == right.name_space && left.partial == right.partial &&
         SameTime(left.create_time, right.create_time) &&
         SameTime(left.write_time, right.write_time) &&
         SameTime(left.change_time, right.change_time) &&
         SameTime(left.access_time, right.access_time);
}

std::vector<CarvedName> CarveFileNames(std::span<const BYTE> record,
                                       size_t marker_offset) {
  if (marker_offset > record.size() ||
      record.size() - marker_offset < end_marker_size) {
    return {};
  }
  return CarveFrom(
      record, marker_offset + end_marker_size,
      std::min(marker_offset + live_terminator_size, record.size()));
}

std::vector<CarvedName> CarveFileNamesFrom(std::span<const BYTE> bytes,
                                           size_t slack_begin,
                                           CarveSource source) {
  if (slack_begin > bytes.size()) {
    return {};
  }
  std::vector<CarvedName> carved = CarveFrom(bytes, slack_begin, slack_begin);
  for (CarvedName& name : carved) {
    name.source = source;
  }
  return carved;
}

}  // namespace NTFS_BROWSER_NAMESPACE::Record
