#include <cstring>
#include <span>
#include <stdexcept>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include <ntfs-browser/data/file-record-header.h>
#include <ntfs-browser/strategy.h>

using NtfsBrowser::FileRecordHeader;
using NtfsBrowser::kFileRecordMagic;
using NtfsBrowser::Strategy;

namespace
{

// Regression fixture for bug F3: FileRecordHeader's ctor only checks
// offset_of_us < buffer.size() (src/data/file-record-header.cpp:30) before
// reading the Update Sequence Number array. It never checks that the array
// itself - which starts right after the WORD at offset_of_us and holds
// buffer.size() / sector_size further WORDs - actually fits inside buffer,
// and size_of_us (the on-disk field that states the array's real size) is
// never consulted at all. With a small sector_size (allowed: the only guard
// in ntfs-volume.cpp is sector_size >= sizeof(WORD)) and offset_of_us near
// the end of the 1024-byte record buffer, the array read runs far past the
// buffer's declared end.
//
// To make the over-read deterministic without depending on ASan or on
// whatever garbage happens to sit past a real heap allocation, this test
// backs FileRecordHeader with a 2048-byte allocation but only ever hands it
// a 1024-byte span over the first half - matching the ctor's hard
// requirement that buffer.size() == 1024, exactly what FileRecord's real
// record_buffer_ is always resized to (see volume_.GetFileRecordSize() in
// src/file-record.cpp). The second half is filled with a recognizable,
// index-derived sentinel pattern that the declared 1024-byte buffer never
// contains anywhere else. If FileRecordHeader stayed within buffer's
// declared bounds, us_array could never end up holding that sentinel
// pattern; because of F3, with offset_of_us = 1022 and sector_size = 2, it
// does - every single one of the 512 words making up us_array is read from
// past byte 1024, i.e. entirely outside the buffer that was declared to be
// exactly 1024 bytes long. In the real code path (record_buffer_, a
// std::vector<BYTE> resized to exactly file_record_size_) those same reads
// walk off the end of the heap allocation.
constexpr size_t kDeclaredBufferSize = 1024;
constexpr size_t kSectorSize = 2;
constexpr size_t kArrayWords = kDeclaredBufferSize / kSectorSize;  // 512

// Last WORD-aligned slot inside the declared buffer: the US number itself
// (at offset_of_us) still reads from inside buffer, but the array right
// after it (offset_of_us + sizeof(WORD)) lands exactly on the buffer's
// declared end, so the whole 512-word array is read from past it.
constexpr WORD kOffsetOfUs =
    static_cast<WORD>(kDeclaredBufferSize - sizeof(WORD));

WORD Sentinel(size_t i) { return static_cast<WORD>(0xBEEF + i); }

}  // namespace

TEST_CASE(
    "FileRecordHeader must not leak bytes past the declared buffer when "
    "offset_of_us leaves no room for the US array (F3)",
    "[file-record-header][regression]")
{
  // storage[0, kDeclaredBufferSize) is the "record buffer" FileRecordHeader
  // is told about (a 1024-byte span, satisfying its own size check).
  // storage[kDeclaredBufferSize, end) is memory FileRecordHeader was never
  // given and must never read - filled with the sentinel pattern so any
  // read past the declared end is caught deterministically.
  std::vector<BYTE> storage(kDeclaredBufferSize + kArrayWords * sizeof(WORD),
                            0);

  auto& header = *reinterpret_cast<FileRecordHeader::Data*>(storage.data());
  header.magic = kFileRecordMagic;
  header.offset_of_us = kOffsetOfUs;
  // Per NTFS format, size_of_us should be buffer.size() / sector_size + 1
  // (kArrayWords + 1); set correctly here too, since F3 also notes this
  // field is never validated against reality - it must not matter to the
  // outcome of this test either way.
  header.size_of_us = static_cast<WORD>(kArrayWords + 1);

  for (size_t i = 0; i < kArrayWords; i++)
  {
    const WORD sentinel = Sentinel(i);
    std::memcpy(storage.data() + kDeclaredBufferSize + i * sizeof(WORD),
                &sentinel, sizeof(sentinel));
  }

  const std::span<const BYTE> buffer(storage.data(), kDeclaredBufferSize);

  bool leakedSentinel = false;
  try
  {
    const auto fr =
        FileRecordHeader::Factory<Strategy::NO_CACHE>(buffer, kSectorSize);

    leakedSentinel = fr.us_array.size() == kArrayWords && [&]
    {
      for (size_t i = 0; i < kArrayWords; i++)
      {
        if (fr.us_array[i] != Sentinel(i))
        {
          return false;
        }
      }
      return true;
    }();
  }
  catch (const std::runtime_error&)
  {
    // A well-formed fix is allowed to reject this record outright instead
    // of truncating/clamping the array - either way, no sentinel data
    // reaches us_array.
    leakedSentinel = false;
  }

  // Before the F3 fix: no exception is thrown, and us_array is populated
  // entirely from storage[kDeclaredBufferSize, ...) - bytes that were never
  // part of the 1024-byte buffer FileRecordHeader was given. That is the
  // out-of-bounds read this test exists to catch.
  CHECK_FALSE(leakedSentinel);
}
