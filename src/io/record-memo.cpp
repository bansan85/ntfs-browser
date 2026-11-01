#include "io/record-memo.h"

#include <ntfs-browser/win-types.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <span>
#include <vector>

#include <ntfs-browser/namespace.h>

#include "data/file-record-header.h"

namespace NTFS_BROWSER_NAMESPACE::Io {

namespace {

// Largest file record size a volume can have.
constexpr std::size_t max_record_size =
    Data::FileRecordHeader::max_file_record_size;

// Bytes between two update sequence words: the width of a sector as the
// update sequence protects it.
constexpr std::size_t sector_stride =
    Data::FileRecordHeader::update_sequence_stride;

// Where the header of a record puts the offset of its update sequence array,
// the number of words of that array, the sequence number and the flags.
constexpr std::size_t offset_of_us_at = 4;
constexpr std::size_t size_of_us_at = 6;
constexpr std::size_t seq_no_at = 0x10;
constexpr std::size_t flags_at = 0x16;

// The header up to the end of the flags: the array MUST start after it, or
// the bytes a probe reads of both would overlap.
constexpr std::size_t header_end = 0x18;

// Where a place keeps what it holds: the slot number plus one comes first, and
// the array and the sector ends follow these four WORDs.
constexpr std::size_t place_offset_of_us = 8;
constexpr std::size_t place_size_of_us = 10;
constexpr std::size_t place_seq_no = 12;
constexpr std::size_t place_flags = 14;
constexpr std::size_t place_array = 16;

// Bytes that a WORD takes.
constexpr std::size_t word_bytes = sizeof(WORD);

// The WORD of bytes at offset at. at + 2 MUST NOT pass the end of bytes.
WORD ReadWord(std::span<const BYTE> bytes, std::size_t at) noexcept {
  WORD value = 0;
  std::memcpy(&value, bytes.subspan(at, sizeof(value)).data(), sizeof(value));
  return value;
}

// Bytes of the update sequence array a probe of a record of record_size reads,
// the number included, as the header says.
std::size_t ArrayBytes(std::size_t record_size, WORD size_of_us) noexcept {
  return word_bytes * (1 + Data::FileRecordHeader::UpdateSequenceBlockCount(
                               record_size, size_of_us));
}

}  // namespace

RecordMemo::RecordMemo(std::size_t entries, std::size_t record_size)
    : record_size_(record_size) {
  const bool usable = record_size >= sector_stride &&
                      record_size <= max_record_size &&
                      record_size % sector_stride == 0;
  if (entries == 0 || !usable) {
    return;
  }
  sectors_ = record_size / sector_stride;
  const std::size_t bytes =
      place_array + (word_bytes * (1 + sectors_)) + (word_bytes * sectors_);
  // The slot number comes first, a place starts at a multiple of 8.
  const std::size_t aligned = (bytes + 7) / 8 * 8;
  // Zeroed pages the system commits when they are first written.
  // NOLINTNEXTLINE(cppcoreguidelines-no-malloc,cppcoreguidelines-owning-memory)
  table_.reset(static_cast<BYTE*>(std::calloc(entries, aligned)));
  if (table_) {
    entries_ = entries;
    stride_ = aligned;
  }
}

std::uint64_t RecordMemo::TableBytes() const noexcept {
  return std::uint64_t{entries_} * stride_;
}

std::span<BYTE> RecordMemo::PlaceOf(ULONGLONG slot) noexcept {
  // The remainder is below entries_, so the place is inside the table.
  // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
  return {&table_[(slot % entries_) * stride_], stride_};
}

void RecordMemo::Put(ULONGLONG first_slot, std::span<const BYTE> bytes) {
  if (entries_ == 0) {
    return;
  }
  const std::size_t count = bytes.size() / record_size_;
  for (std::size_t index = 0; index < count; index++) {
    PutOne(first_slot + index,
           bytes.subspan(index * record_size_, record_size_));
  }
}

void RecordMemo::PutOne(ULONGLONG slot, std::span<const BYTE> bytes) {
  const std::span<BYTE> place = PlaceOf(slot);
  const ULONGLONG marker = slot + 1;
  ULONGLONG held = 0;
  std::memcpy(&held, place.data(), sizeof(held));

  const std::size_t offset_of_us = ReadWord(bytes, offset_of_us_at);
  const std::size_t array_bytes =
      ArrayBytes(record_size_, ReadWord(bytes, size_of_us_at));
  DWORD magic = 0;
  std::memcpy(&magic, bytes.data(), sizeof(magic));

  // A record is kept only if what a probe reads of it sits in the header, the
  // array and the sector ends, apart from each other: the array must not touch
  // the header, nor the end of the first sector.
  if (magic != Data::FileRecordHeader::file_record_magic ||
      offset_of_us < header_end ||
      offset_of_us + array_bytes > sector_stride - word_bytes) {
    if (held == marker) {
      std::ranges::fill(place.first(sizeof(held)), BYTE{0});
    }
    return;
  }

  std::memcpy(place.data(), &marker, sizeof(marker));
  const auto copy_word = [&](std::size_t to, std::size_t from) {
    std::ranges::copy(bytes.subspan(from, word_bytes),
                      place.subspan(to).begin());
  };
  copy_word(place_offset_of_us, offset_of_us_at);
  copy_word(place_size_of_us, size_of_us_at);
  copy_word(place_seq_no, seq_no_at);
  copy_word(place_flags, flags_at);
  std::ranges::copy(bytes.subspan(offset_of_us, array_bytes),
                    place.subspan(place_array).begin());
  const std::size_t ends = place_array + (word_bytes * (1 + sectors_));
  for (std::size_t sector = 0; sector < sectors_; sector++) {
    std::ranges::copy(
        bytes.subspan(((sector + 1) * sector_stride) - word_bytes, word_bytes),
        place.subspan(ends + (sector * word_bytes)).begin());
  }
}

std::span<const BYTE> RecordMemo::Find(ULONGLONG slot) {
  if (entries_ == 0) {
    return {};
  }
  const std::span<const BYTE> place = PlaceOf(slot);
  ULONGLONG held = 0;
  std::memcpy(&held, place.data(), sizeof(held));
  if (held != slot + 1) {
    return {};
  }

  rebuilt_.assign(record_size_, 0);
  const std::span<BYTE> out(rebuilt_);
  const DWORD magic = Data::FileRecordHeader::file_record_magic;
  std::memcpy(out.data(), &magic, sizeof(magic));
  const auto copy_word = [&](std::size_t to, std::size_t from) {
    std::ranges::copy(place.subspan(from, word_bytes), out.subspan(to).begin());
  };
  copy_word(offset_of_us_at, place_offset_of_us);
  copy_word(size_of_us_at, place_size_of_us);
  copy_word(seq_no_at, place_seq_no);
  copy_word(flags_at, place_flags);
  const std::size_t offset_of_us = ReadWord(place, place_offset_of_us);
  const std::size_t array_bytes =
      ArrayBytes(record_size_, ReadWord(place, place_size_of_us));
  std::ranges::copy(place.subspan(place_array, array_bytes),
                    out.subspan(offset_of_us).begin());
  const std::size_t ends = place_array + (word_bytes * (1 + sectors_));
  for (std::size_t sector = 0; sector < sectors_; sector++) {
    std::ranges::copy(
        place.subspan(ends + (sector * word_bytes), word_bytes),
        out.subspan(((sector + 1) * sector_stride) - word_bytes).begin());
  }
  return rebuilt_;
}

}  // namespace NTFS_BROWSER_NAMESPACE::Io
