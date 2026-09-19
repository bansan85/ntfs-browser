#include "lznt1/decompress.h"

#include <cstring>
#include <stdexcept>

namespace NtfsBrowser::Lznt1
{

namespace
{

// Chunk header bits ([MS-XCA] 2.5.1.2): compressed flag, signature, size.
constexpr WORD kHeaderCompressed = 0x8000;
constexpr WORD kHeaderSignatureMask = 0x7000;
constexpr WORD kHeaderSignature = 0x3000;  // "This value MUST always be 3"
constexpr WORD kHeaderSizeMask = 0x0FFF;

// Header is 2 bytes; declared size is the whole chunk minus a 3-byte bias.
constexpr size_t kHeaderSize = 2;
constexpr size_t kSizeBias = 3;

// Flag byte covers up to 8 elements, LSB-first ([MS-XCA] 2.5.1.3).
constexpr unsigned kFlagsPerByte = 8;

constexpr unsigned kBitsPerByte = 8;

// Compressed word: D+L=16 bits, D,L in [4,12] ([MS-XCA] 2.5.1.4), biased low.
constexpr size_t kCompressedWordSize = 2;
constexpr unsigned kWordBits = 16;
constexpr unsigned kMinDisplacementBits = 4;
constexpr unsigned kMaxDisplacementBits = 12;
constexpr size_t kLengthBias = 3;

[[nodiscard]] WORD ReadLe16(std::span<const BYTE> src, size_t offset) noexcept
{
  return static_cast<WORD>(
      static_cast<WORD>(src[offset]) |
      static_cast<WORD>(static_cast<WORD>(src[offset + 1]) << kBitsPerByte));
}

// Returns the displacement-field width [MS-XCA] 2.5.1.4 prescribes for a
// compressed word, given the bytes already produced in the current chunk.
[[nodiscard]] unsigned DisplacementBits(size_t producedInChunk) noexcept
{
  for (unsigned mBits = kMaxDisplacementBits; mBits > kMinDisplacementBits;
       mBits--)
  {
    if ((static_cast<size_t>(1) << (mBits - 1)) < producedInChunk)
    {
      return mBits;
    }
  }
  return kMinDisplacementBits;
}

// Copies "length" bytes from "displacement" bytes back in "dest" to its
// current end, one byte at a time so overlapping back-references resolve.
void CopyBackReference(std::span<BYTE> dest, size_t& out, size_t displacement,
                       size_t length) noexcept
{
  size_t from = out - displacement;
  for (size_t i = 0; i < length; i++)
  {
    dest[out] = dest[from];
    out++;
    from++;
  }
}

}  // namespace

size_t Decompress(std::span<const BYTE> src, std::span<BYTE> dest)
{
  size_t inPos = 0;
  size_t out = 0;

  while (inPos + kHeaderSize <= src.size())
  {
    const WORD header = ReadLe16(src, inPos);
    inPos += kHeaderSize;

    // header == 0 is the End_of_buffer terminal ([MS-XCA] 2.5.1.2).
    if (header == 0)
    {
      break;
    }

    if ((header & kHeaderSignatureMask) != kHeaderSignature)
    {
      throw std::runtime_error("LZNT1: invalid chunk header signature.\n");
    }

    // payload = size + 3-byte bias - header; always >= 1, so loop advances.
    const size_t payload =
        (static_cast<size_t>(header & kHeaderSizeMask) + kSizeBias) -
        kHeaderSize;
    if (payload > src.size() - inPos)
    {
      throw std::runtime_error(
          "LZNT1: chunk exceeds compressed data bounds.\n");
    }
    const size_t chunkEnd = inPos + payload;

    if ((header & kHeaderCompressed) == 0)
    {
      // Uncompressed chunk: literal bytes follow the header ([MS-XCA] 2.5.1.2).
      if (payload > dest.size() - out)
      {
        throw std::runtime_error(
            "LZNT1: uncompressed chunk exceeds decompressed bounds.\n");
      }
      std::memcpy(dest.data() + out, src.data() + inPos, payload);
      out += payload;
      inPos = chunkEnd;
      continue;
    }

    // Declared chunk size, not flag bits, bounds the data ([MS-XCA] 2.5.3).
    const size_t chunkOutStart = out;
    while (inPos < chunkEnd)
    {
      const BYTE flags = src[inPos];
      inPos++;

      for (unsigned bit = 0; bit < kFlagsPerByte && inPos < chunkEnd; bit++)
      {
        // A word can decode past kChunkSize; nothing in the encoding caps it.
        if (out - chunkOutStart >= kChunkSize)
        {
          throw std::runtime_error(
              "LZNT1: chunk decompresses to more than 4096 bytes.\n");
        }

        if ((flags & static_cast<BYTE>(1U << bit)) == 0)
        {
          // Literal byte.
          if (out == dest.size())
          {
            throw std::runtime_error(
                "LZNT1: literal exceeds decompressed bounds.\n");
          }
          dest[out] = src[inPos];
          out++;
          inPos++;
          continue;
        }

        if (chunkEnd - inPos < kCompressedWordSize)
        {
          throw std::runtime_error("LZNT1: truncated compressed word.\n");
        }
        const WORD word = ReadLe16(src, inPos);
        inPos += kCompressedWordSize;

        const size_t produced = out - chunkOutStart;
        const unsigned displacementBits = DisplacementBits(produced);
        const unsigned lengthBits = kWordBits - displacementBits;
        const auto lengthMask = static_cast<WORD>((1U << lengthBits) - 1U);

        const size_t length =
            static_cast<size_t>(word & lengthMask) + kLengthBias;
        const size_t displacement = static_cast<size_t>(word >> lengthBits) + 1;

        // A displacement before this chunk has no history to resolve.
        if (displacement > produced)
        {
          throw std::runtime_error(
              "LZNT1: back-reference before start of chunk.\n");
        }
        if (length > kChunkSize - produced)
        {
          throw std::runtime_error(
              "LZNT1: chunk decompresses to more than 4096 bytes.\n");
        }
        if (length > dest.size() - out)
        {
          throw std::runtime_error(
              "LZNT1: back-reference exceeds decompressed bounds.\n");
        }

        CopyBackReference(dest, out, displacement, length);
      }
    }

    inPos = chunkEnd;
  }

  return out;
}

}  // namespace NtfsBrowser::Lznt1
