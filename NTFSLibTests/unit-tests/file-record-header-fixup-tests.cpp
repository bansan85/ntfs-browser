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

// Matches the record buffer size FileRecord always allocates.
constexpr size_t kDeclaredBufferSize = 1024;

// Smallest sector size ntfs-volume.cpp allows (>= sizeof(WORD)).
constexpr size_t kSectorSize = 2;

// Number of WORDs FileRecordHeader reads into the US array.
constexpr size_t kArrayWords = kDeclaredBufferSize / kSectorSize;

// Places the US array's first word exactly at the buffer's declared end.
constexpr WORD kOffsetOfUs =
    static_cast<WORD>(kDeclaredBufferSize - sizeof(WORD));

// Pattern that never appears anywhere in the declared 1024-byte buffer.
WORD Sentinel(size_t i) { return static_cast<WORD>(0xBEEF + i); }

}  // namespace

TEST_CASE(
    "FileRecordHeader must not leak bytes past the declared buffer when "
    "offset_of_us leaves no room for the US array",
    "[file-record-header][regression]")
{
  // Bytes past kDeclaredBufferSize are outside what FileRecordHeader sees.
  std::vector<BYTE> storage(kDeclaredBufferSize + kArrayWords * sizeof(WORD),
                            0);

  auto& header = *reinterpret_cast<FileRecordHeader::Data*>(storage.data());
  header.magic = kFileRecordMagic;
  header.offset_of_us = kOffsetOfUs;
  // Correct value; the ctor never actually checks it against reality.
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
    // A fix may reject the record outright instead of truncating it.
    leakedSentinel = false;
  }

  CHECK_FALSE(leakedSentinel);
}
