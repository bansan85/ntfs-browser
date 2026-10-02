#include <ntfs-browser/win-types.h>

#include <cstring>
#include <span>
#include <stdexcept>
#include <vector>

#include <catch2/catch_template_test_macros.hpp>
#include <catch2/catch_test_macros.hpp>
#include <gsl/narrow>

#include <ntfs-browser/strategy.h>

#include "data/file-record-header.h"
#include "file-record-header-edit.h"

using NtfsBrowser::FileRecordHeader;
using NtfsBrowser::kFileRecordMagic;
using NtfsBrowser::Strategy;

namespace
{

// Matches the record buffer size FileRecord always allocates.
constexpr size_t kDeclaredBufferSize = 1024;

// Number of WORDs FileRecordHeader reads into the US array: one per
// 512-byte block.
constexpr size_t kArrayWords =
    kDeclaredBufferSize / NtfsBrowser::kUpdateSequenceStride;

// Places the US array's first word exactly at the buffer's declared end.
constexpr WORD kOffsetOfUs =
    static_cast<WORD>(kDeclaredBufferSize - sizeof(WORD));

// Bases of the two WORD patterns below.
constexpr WORD kSentinelBase = 0xBEEF;
constexpr WORD kUsArrayFillBase = 0xA000;

// Pattern that never appears anywhere in the declared 1024-byte buffer.
WORD Sentinel(size_t i) { return gsl::narrow<WORD>(kSentinelBase + i); }

}  // namespace

TEMPLATE_TEST_CASE_SIG(
    "FileRecordHeader must not leak bytes past the declared buffer when "
    "offset_of_us leaves no room for the US array",
    "[file-record-header][regression]", ((Strategy S), S), Strategy::NO_CACHE,
    Strategy::FULL_CACHE)
{
  // Bytes past kDeclaredBufferSize are outside what FileRecordHeader sees.
  std::vector<BYTE> storage(kDeclaredBufferSize + kArrayWords * sizeof(WORD),
                            0);

  NtfsBrowserTests::EditFileRecordHeader(
      storage,
      [](FileRecordHeader::Data& header)
      {
        header.magic = kFileRecordMagic;
        header.offset_of_us = kOffsetOfUs;
        // Correct value; it bounds how many array words the ctor reads.
        header.size_of_us = static_cast<WORD>(kArrayWords + 1);
      });

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
    const auto fr = FileRecordHeader::Factory<S>(buffer);

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
    // A fix may reject the record outright instead of truncating it.
    leakedSentinel = false;
  }

  CHECK_FALSE(leakedSentinel);
}

TEMPLATE_TEST_CASE_SIG(
    "FileRecordHeader::PatchUS must restore the last word of every 512-byte "
    "block, whatever the volume's sector size (4Kn volumes)",
    "[file-record-header][regression]", ((Strategy S), S), Strategy::NO_CACHE,
    Strategy::FULL_CACHE)
{
  constexpr size_t kRecordSize = 4096;
  constexpr size_t kBlockSize = 512;
  constexpr size_t kBlocks = kRecordSize / kBlockSize;
  constexpr WORD kUsn = 0x7777;
  constexpr WORD kOffsetOfUsArray = 48;

  std::vector<BYTE> storage(kRecordSize, 0);
  NtfsBrowserTests::EditFileRecordHeader(storage,
                                         [](FileRecordHeader::Data& header)
                                         {
                                           header.magic = kFileRecordMagic;
                                           header.offset_of_us =
                                               kOffsetOfUsArray;
                                           header.size_of_us =
                                               static_cast<WORD>(kBlocks + 1);
                                         });

  const auto put_word = [&](size_t offset, WORD value)
  { std::memcpy(storage.data() + offset, &value, sizeof(value)); };

  put_word(kOffsetOfUsArray, kUsn);
  for (size_t i = 0; i < kBlocks; i++)
  {
    // The array holds each block's true last word; the block itself carries
    // the sequence number, as it does on disk.
    put_word(kOffsetOfUsArray + sizeof(WORD) * (1 + i),
             gsl::narrow<WORD>(kUsArrayFillBase + i));
    put_word((i + 1) * kBlockSize - sizeof(WORD), kUsn);
  }

  const std::span<const BYTE> buffer(storage.data(), storage.size());
  auto fr = FileRecordHeader::Factory<S>(buffer);

  REQUIRE(fr.PatchUS());

  for (size_t i = 0; i < kBlocks; i++)
  {
    WORD restored = 0;
    std::memcpy(&restored,
                &fr.GetData()->raw[(i + 1) * kBlockSize - sizeof(WORD)],
                sizeof(restored));
    CHECK(restored == gsl::narrow<WORD>(kUsArrayFillBase + i));
  }
}
