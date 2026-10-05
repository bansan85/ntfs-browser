#include "lznt1/decompress.h"

#include <ntfs-browser/win-types.h>

#include <cstring>
#include <stdexcept>

#include <gsl/narrow>

namespace NtfsBrowser::Lznt1
{

namespace
{

// Chunk header bits ([MS-XCA] 2.5.1.2): compressed flag, signature, size.
constexpr WORD header_compressed = 0x8000;
constexpr WORD header_signature_mask = 0x7000;
constexpr WORD header_signature = 0x3000;  // "This value MUST always be 3"
constexpr WORD header_size_mask = 0x0FFF;

// Header is 2 bytes; declared size is the whole chunk minus a 3-byte bias.
constexpr size_t header_size = 2;
constexpr size_t size_bias = 3;

// Flag byte covers up to 8 elements, LSB-first ([MS-XCA] 2.5.1.3).
constexpr unsigned flags_per_byte = 8;

constexpr unsigned bits_per_byte = 8;

// Compressed word: D+L=16 bits, D,L in [4,12] ([MS-XCA] 2.5.1.4), biased low.
constexpr size_t compressed_word_size = 2;
constexpr unsigned word_bits = 16;
constexpr unsigned min_displacement_bits = 4;
constexpr unsigned max_displacement_bits = 12;
constexpr size_t length_bias = 3;

[[nodiscard]] WORD ReadLe16(std::span<const BYTE> src, size_t offset) noexcept
{
  // Every caller has checked that two bytes remain at "offset".
  // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
  const auto low = static_cast<WORD>(src[offset]);
  // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
  const auto high = static_cast<WORD>(src[offset + 1]);
  return static_cast<WORD>(low | static_cast<WORD>(high << bits_per_byte));
}

// Returns the displacement-field width [MS-XCA] 2.5.1.4 prescribes for a
// compressed word, given the bytes already produced in the current chunk.
[[nodiscard]] unsigned DisplacementBits(size_t produced_in_chunk) noexcept
{
  for (unsigned m_bits = max_displacement_bits; m_bits > min_displacement_bits;
       m_bits--)
  {
    if ((static_cast<size_t>(1) << (m_bits - 1)) < produced_in_chunk)
    {
      return m_bits;
    }
  }
  return min_displacement_bits;
}

// Copies "length" bytes from "displacement" bytes back in "dest" to its
// current end, one byte at a time so overlapping back-references resolve.
void CopyBackReference(std::span<BYTE> dest, size_t& out, size_t displacement,
                       size_t length) noexcept
{
  size_t from = out - displacement;
  for (size_t i = 0; i < length; i++)
  {
    // The caller checked displacement <= out and length <= dest.size() - out.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    dest[out] = dest[from];
    out++;
    from++;
  }
}

// Decodes one compressed word - a back-reference - at inPos into dest, whose
// current chunk began at chunkOutStart. Advances inPos and out.
void DecodeBackReference(std::span<const BYTE> src, std::span<BYTE> dest,
                         size_t& in_pos, size_t chunk_end,
                         size_t chunk_out_start, size_t& out)
{
  if (chunk_end - in_pos < compressed_word_size)
  {
    throw std::runtime_error("LZNT1: truncated compressed word.\n");
  }
  const WORD word = ReadLe16(src, in_pos);
  in_pos += compressed_word_size;

  const size_t produced = out - chunk_out_start;
  const unsigned displacement_bits = DisplacementBits(produced);
  const unsigned length_bits = word_bits - displacement_bits;
  const auto length_mask = gsl::narrow<WORD>((1U << length_bits) - 1U);

  const size_t length = static_cast<size_t>(word & length_mask) + length_bias;
  const size_t displacement = static_cast<size_t>(word >> length_bits) + 1;

  // A displacement before this chunk has no history to resolve.
  if (displacement > produced)
  {
    throw std::runtime_error("LZNT1: back-reference before start of chunk.\n");
  }
  if (length > chunk_size - produced)
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

// Decodes the elements of a compressed chunk: the bytes from inPos to chunkEnd.
// Advances inPos and out.
void DecompressChunk(std::span<const BYTE> src, std::span<BYTE> dest,
                     size_t& in_pos, size_t chunk_end, size_t& out)
{
  // Declared chunk size, not flag bits, bounds the data ([MS-XCA] 2.5.3).
  const size_t chunk_out_start = out;
  while (in_pos < chunk_end)
  {
    // The enclosing loop tests inPos < chunkEnd <= src.size().
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    const BYTE flags = src[in_pos];
    in_pos++;

    for (unsigned bit = 0; bit < flags_per_byte && in_pos < chunk_end; bit++)
    {
      // A word can decode past chunk_size; nothing in the encoding caps it.
      if (out - chunk_out_start >= chunk_size)
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
        // out < dest.size() just above; inPos < chunkEnd <= src.size().
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        dest[out] = src[in_pos];
        out++;
        in_pos++;
        continue;
      }

      DecodeBackReference(src, dest, in_pos, chunk_end, chunk_out_start, out);
    }
  }
}

}  // namespace

size_t Decompress(std::span<const BYTE> src, std::span<BYTE> dest)
{
  size_t in_pos = 0;
  size_t out = 0;

  while (in_pos + header_size <= src.size())
  {
    const WORD header = ReadLe16(src, in_pos);
    in_pos += header_size;

    // header == 0 is the End_of_buffer terminal ([MS-XCA] 2.5.1.2).
    if (header == 0)
    {
      break;
    }

    if ((header & header_signature_mask) != header_signature)
    {
      throw std::runtime_error("LZNT1: invalid chunk header signature.\n");
    }

    // payload = size + 3-byte bias - header; always >= 1, so loop advances.
    const size_t payload =
        (static_cast<size_t>(header & header_size_mask) + size_bias) -
        header_size;
    if (payload > src.size() - in_pos)
    {
      throw std::runtime_error(
          "LZNT1: chunk exceeds compressed data bounds.\n");
    }
    const size_t chunk_end = in_pos + payload;

    if ((header & header_compressed) == 0)
    {
      // Uncompressed chunk: literal bytes follow the header ([MS-XCA] 2.5.1.2).
      if (payload > dest.size() - out)
      {
        throw std::runtime_error(
            "LZNT1: uncompressed chunk exceeds decompressed bounds.\n");
      }
      // payload was checked against both src and dest above, so both are in
      // range.
      // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
      std::memcpy(&dest[out], &src[in_pos], payload);
      out += payload;
      in_pos = chunk_end;
      continue;
    }

    DecompressChunk(src, dest, in_pos, chunk_end, out);

    in_pos = chunk_end;
  }

  return out;
}

}  // namespace NtfsBrowser::Lznt1
