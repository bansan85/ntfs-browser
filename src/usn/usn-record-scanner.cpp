#include "usn/usn-record-scanner.h"

#include <ntfs-browser/win-types.h>

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <gsl/narrow>

#include <ntfs-browser/namespace.h>
#include <ntfs-browser/usn/types.h>

#include "data/usn-record.h"
#include "log/ntfs-common.h"
#include "mft/file-reference.h"
#include "utf/utf.h"

namespace NTFS_BROWSER_NAMESPACE::Usn {

namespace {

using NTFS_BROWSER_NAMESPACE::Data::UsnRecordCommon;
using NTFS_BROWSER_NAMESPACE::Data::UsnRecordV2;
using NTFS_BROWSER_NAMESPACE::Data::UsnRecordV3;
using NTFS_BROWSER_NAMESPACE::Data::UsnRecordV4;

// Longest V2 or V3 record this reader accepts. A name holds at most 255
// characters, 510 bytes, after a header of at most 76: 586 bytes. 4096 leaves
// room for padding and for a future field, and still rejects a forged length
// that would make a scan swallow the stream.
constexpr size_t max_name_record_length = 4096;

// Longest V4 record this reader accepts. It lists extents of 16 bytes each, so
// it can be longer than a named record. 64 KiB bounds what a scan keeps while
// it waits for a record's end.
constexpr size_t max_range_record_length = size_t{64} * 1024;

// Smallest block a record or a stretch of padding takes: records start on a
// multiple of this many bytes.
constexpr size_t word_size = Data::usn_record_alignment;

// Bytes of one extent of a V4 record.
constexpr size_t v4_extent_bytes = 16;

// The journal versions this reader knows.
constexpr WORD version_2 = 2;
constexpr WORD version_3 = 3;
constexpr WORD version_4 = 4;

// Smallest length the header of a version names, or 0 for a version that is not
// a journal version.
size_t MinimumLength(WORD major) noexcept {
  switch (major) {
    case version_2:
      return sizeof(UsnRecordV2);
    case version_3:
      return sizeof(UsnRecordV3);
    case version_4:
      return sizeof(UsnRecordV4);
    default:
      return 0;
  }
}

// Whether bytes starts with a header that a record can have: a known version,
// a minor version of 0, and a length that is aligned and inside the bounds of
// its version. Says nothing of the rest of the record.
bool PlausibleHeader(const UsnRecordCommon& header) noexcept {
  const size_t minimum = MinimumLength(header.major_version);
  if (minimum == 0 || header.minor_version != 0) {
    return false;
  }
  const size_t maximum = header.major_version == version_4
                             ? max_range_record_length
                             : max_name_record_length;
  return header.record_length % word_size == 0 &&
         header.record_length >= minimum && header.record_length <= maximum;
}

// Whether the eight bytes at the start of window are all zero: the padding that
// the journal leaves between records and at the end of a page.
bool IsZeroWord(std::span<const BYTE> window) noexcept {
  return std::ranges::all_of(window.first(word_size),
                             [](BYTE byte) { return byte == 0; });
}

// Whether units hold well-formed UTF-16: every high surrogate is followed by a
// low one, and no low one stands alone.
bool IsWellFormedUtf16(std::u16string_view units) noexcept {
  constexpr char16_t high_first = 0xD800;
  constexpr char16_t high_last = 0xDBFF;
  constexpr char16_t low_first = 0xDC00;
  constexpr char16_t low_last = 0xDFFF;
  for (auto unit = units.begin(); unit != units.end(); ++unit) {
    if (*unit >= high_first && *unit <= high_last) {
      // A high surrogate is followed by its low half.
      ++unit;
      if (unit == units.end() || *unit < low_first || *unit > low_last) {
        return false;
      }
    } else if (*unit >= low_first && *unit <= low_last) {
      return false;
    }
  }
  return true;
}

// The bytes of an on-disk structure, copied out so that no read is misaligned.
template <class T>
T ReadAt(std::span<const BYTE> window, size_t offset) noexcept {
  T value{};
  std::memcpy(&value, window.subspan(offset, sizeof(T)).data(), sizeof(T));
  return value;
}

// Splits a 64-bit file reference into the record number and the sequence.
void SplitReference(ULONGLONG reference, ULONGLONG& record,
                    WORD& sequence) noexcept {
  record = reference & Mft::mft_record_number_mask;
  sequence = static_cast<WORD>(reference >> Mft::mft_sequence_shift);
}

// Whether a name of name_length bytes at name_offset is placed as a V2 or V3
// record requires: inside the record, after the fixed part, on a 2-byte
// boundary, with an even length.
bool IsNamePlacementValid(size_t window_size, size_t fixed_size,
                          size_t name_offset, size_t name_length) noexcept {
  const bool aligned = name_offset % sizeof(char16_t) == 0 &&
                       name_length % sizeof(char16_t) == 0;
  // The subtraction cannot wrap: name_offset is checked against the size first.
  const bool inside =
      name_offset <= window_size && name_length <= window_size - name_offset;
  return name_offset >= fixed_size && aligned && inside;
}

// The name of a V2 or V3 record, or nothing when it is not well-formed: its
// placement MUST be valid and it MUST be valid UTF-16.
bool ReadName(std::span<const BYTE> window, size_t fixed_size,
              size_t name_offset, size_t name_length, std::wstring& name) {
  if (!IsNamePlacementValid(window.size(), fixed_size, name_offset,
                            name_length)) {
    return false;
  }
  std::u16string units(name_length / sizeof(char16_t), u'\0');
  if (name_length != 0) {
    std::memcpy(units.data(), window.subspan(name_offset, name_length).data(),
                name_length);
  }
  if (!IsWellFormedUtf16(units)) {
    return false;
  }
  name = Utf::Utf16ToWide(units);
  return true;
}

// A FILETIME that holds the 64-bit value the journal stores.
FILETIME ToFileTime(ULONGLONG value) noexcept {
  constexpr unsigned dword_bits = 32;
  constexpr ULONGLONG low_dword_mask = (ULONGLONG{1} << dword_bits) - 1;
  return FILETIME{.dwLowDateTime = static_cast<DWORD>(value & low_dword_mask),
                  .dwHighDateTime = static_cast<DWORD>(value >> dword_bits)};
}

}  // namespace

void RecordScanner::Reset() noexcept {
  pending_.clear();
  pending_offset_ = 0;
  resyncing_ = false;
}

void RecordScanner::Feed(std::span<const BYTE> bytes, ULONGLONG stream_offset,
                         std::vector<Record>& out) {
  if (pending_.empty()) {
    pending_offset_ = stream_offset;
  }
  pending_.insert(pending_.end(), bytes.begin(), bytes.end());
  Drain(out, /*last=*/false);
}

void RecordScanner::Finish(std::vector<Record>& out) {
  Drain(out, /*last=*/true);
  Reset();
}

ULONGLONG RecordScanner::DamagedRecords() const noexcept { return damaged_; }

ULONGLONG RecordScanner::SkippedRecords() const noexcept { return skipped_; }

// Reads pending_ from its start. A zero word is padding. Otherwise the bytes
// are a record, or they are damage: the scan then moves on 8 bytes at a time,
// and takes a record again only when its USN is its own offset in the stream,
// which makes a false match in damaged bytes very unlikely. One count of damage
// per stretch.
void RecordScanner::Drain(std::vector<Record>& out, bool last) {
  size_t pos = 0;
  const auto fail = [&](ULONGLONG offset) {
    if (!resyncing_) {
      resyncing_ = true;
      ++damaged_;
      Log::Trace("USN journal: no record at offset {}", offset);
    }
  };

  while (pending_.size() - pos >= word_size) {
    const ULONGLONG offset = pending_offset_ + pos;
    const std::span<const BYTE> window =
        std::span<const BYTE>(pending_).subspan(pos);

    // Records start on a multiple of 8 of the stream.
    if (offset % word_size != 0) {
      if (window.front() != 0) {
        fail(offset);
      }
      ++pos;
      continue;
    }
    if (IsZeroWord(window)) {
      pos += word_size;
      continue;
    }

    const auto header = ReadAt<UsnRecordCommon>(window, 0);
    if (!PlausibleHeader(header)) {
      fail(offset);
      pos += word_size;
      continue;
    }
    if (header.record_length > window.size()) {
      // The rest of the record is not in yet: keep the bytes for the next
      // Feed(). At the end of the range it never will be.
      if (!last) {
        break;
      }
      fail(offset);
      pos += word_size;
      continue;
    }

    const size_t length =
        DecodeRecord(window.first(header.record_length), offset, out);
    if (length == 0) {
      fail(offset);
      pos += word_size;
      continue;
    }
    resyncing_ = false;
    pos += length;
  }

  // A few stray bytes after the last whole word, at the end of a range.
  if (last && !resyncing_ &&
      std::ranges::any_of(std::span<const BYTE>(pending_).subspan(pos),
                          [](BYTE byte) { return byte != 0; })) {
    ++damaged_;
    Log::Trace("USN journal: stray bytes at offset {}", pending_offset_ + pos);
  }

  pending_.erase(pending_.begin(),
                 pending_.begin() + gsl::narrow<std::ptrdiff_t>(pos));
  pending_offset_ += pos;
}

// Decodes the one record in window, whose header PlausibleHeader() accepted.
// While resynchronising, it also requires the record's USN to equal offset.
size_t RecordScanner::DecodeRecord(std::span<const BYTE> window,
                                   ULONGLONG offset, std::vector<Record>& out) {
  const auto header = ReadAt<UsnRecordCommon>(window, 0);
  Record record;
  record.major_version = header.major_version;

  if (header.major_version == version_4) {
    const auto range = ReadAt<UsnRecordV4>(window, 0);
    const size_t extents_bytes =
        static_cast<size_t>(range.number_of_extents) * v4_extent_bytes;
    if (extents_bytes > window.size() - sizeof(UsnRecordV4) ||
        (resyncing_ && std::cmp_not_equal(range.usn, offset))) {
      return 0;
    }
    ++skipped_;
    return header.record_length;
  }

  if (header.major_version == version_2) {
    const auto narrow = ReadAt<UsnRecordV2>(window, 0);
    if ((resyncing_ && std::cmp_not_equal(narrow.usn, offset)) ||
        !ReadName(window, sizeof(UsnRecordV2), narrow.file_name_offset,
                  narrow.file_name_length, record.name)) {
      return 0;
    }
    SplitReference(narrow.file_reference, record.file_record,
                   record.file_sequence);
    SplitReference(narrow.parent_reference, record.parent_record,
                   record.parent_sequence);
    record.usn = narrow.usn;
    record.time = ToFileTime(narrow.timestamp);
    record.reason = narrow.reason;
    record.file_attributes = narrow.file_attributes;
  } else {
    const auto wide = ReadAt<UsnRecordV3>(window, 0);
    if ((resyncing_ && std::cmp_not_equal(wide.usn, offset)) ||
        !ReadName(window, sizeof(UsnRecordV3), wide.file_name_offset,
                  wide.file_name_length, record.name)) {
      return 0;
    }
    SplitReference(wide.file_reference.low, record.file_record,
                   record.file_sequence);
    record.file_reference_high = wide.file_reference.high;
    SplitReference(wide.parent_reference.low, record.parent_record,
                   record.parent_sequence);
    record.parent_reference_high = wide.parent_reference.high;
    record.usn = wide.usn;
    record.time = ToFileTime(wide.timestamp);
    record.reason = wide.reason;
    record.file_attributes = wide.file_attributes;
  }

  out.push_back(std::move(record));
  return header.record_length;
}

}  // namespace NTFS_BROWSER_NAMESPACE::Usn
