#include "lznt1/decompress.h"

#include <ntfs-browser/win-types.h>

#include <cstring>
#include <stdexcept>

#include <gsl/narrow>

namespace NtfsBrowser::Lznt1 {

namespace {

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

[[nodiscard]] WORD ReadLe16(std::span<const BYTE> src, size_t offset) noexcept {
  // Every caller has checked that two bytes remain at "offset".
  // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
  const auto low = static_cast<WORD>(src[offset]);
  // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
  const auto high = static_cast<WORD>(src[offset + 1]);
  return static_cast<WORD>(low | static_cast<WORD>(high << bits_per_byte));
}

// Returns the displacement-field width [MS-XCA] 2.5.1.4 prescribes for a
// compressed word, given the bytes already produced in the current chunk.
[[nodiscard]] unsigned DisplacementBits(size_t produced_in_chunk) noexcept {
  for (unsigned m_bits = max_displacement_bits; m_bits > min_displacement_bits;
       m_bits--) {
    if ((static_cast<size_t>(1) << (m_bits - 1)) < produced_in_chunk) {
      return m_bits;
    }
  }
  return min_displacement_bits;
}

// Copies "length" bytes from "displacement" bytes back in "dest" to its
// current end, one byte at a time so overlapping back-references resolve.
void CopyBackReference(std::span<BYTE> dest, size_t& out, size_t displacement,
                       size_t length) noexcept {
  size_t from = out - displacement;
  for (size_t i = 0; i < length; i++) {
    // The caller checked displacement <= out and length <= dest.size() - out.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    dest[out] = dest[from];
    out++;
    from++;
  }
}

// Where decoding stands: the input and output cursors, the end of the chunk
// being decoded in the input, and the output offset that chunk began at.
struct Cursor {
  std::span<const BYTE> src;
  std::span<BYTE> dest;
  size_t in_pos = 0;
  size_t out = 0;
  size_t chunk_end = 0;
  size_t chunk_out_start = 0;
};

// Decodes one compressed word - a back-reference - at cur.in_pos into the
// output. Advances in_pos and out.
void DecodeBackReference(Cursor& cur) {
  if (cur.chunk_end - cur.in_pos < compressed_word_size) {
    throw std::runtime_error("LZNT1: truncated compressed word.\n");
  }
  const WORD word = ReadLe16(cur.src, cur.in_pos);
  cur.in_pos += compressed_word_size;

  const size_t produced = cur.out - cur.chunk_out_start;
  const unsigned displacement_bits = DisplacementBits(produced);
  const unsigned length_bits = word_bits - displacement_bits;
  const auto length_mask = gsl::narrow<WORD>((1U << length_bits) - 1U);

  const size_t length = static_cast<size_t>(word & length_mask) + length_bias;
  const size_t displacement = static_cast<size_t>(word >> length_bits) + 1;

  // A displacement before this chunk has no history to resolve.
  if (displacement > produced) {
    throw std::runtime_error("LZNT1: back-reference before start of chunk.\n");
  }
  if (length > chunk_size - produced) {
    throw std::runtime_error(
        "LZNT1: chunk decompresses to more than 4096 bytes.\n");
  }
  if (length > cur.dest.size() - cur.out) {
    throw std::runtime_error(
        "LZNT1: back-reference exceeds decompressed bounds.\n");
  }

  CopyBackReference(cur.dest, cur.out, displacement, length);
}

// Copies the literal byte at cur.in_pos to the output. Advances in_pos and out.
void CopyLiteral(Cursor& cur) {
  if (cur.out == cur.dest.size()) {
    throw std::runtime_error("LZNT1: literal exceeds decompressed bounds.\n");
  }
  // out < dest.size() just above; inPos < chunkEnd <= src.size().
  // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
  cur.dest[cur.out] = cur.src[cur.in_pos];
  cur.out++;
  cur.in_pos++;
}

// Decodes the elements of the compressed chunk that ends at cur.chunk_end.
// Advances in_pos and out.
void DecompressChunk(Cursor& cur) {
  // Declared chunk size, not flag bits, bounds the data ([MS-XCA] 2.5.3).
  cur.chunk_out_start = cur.out;
  while (cur.in_pos < cur.chunk_end) {
    // The enclosing loop tests inPos < chunkEnd <= src.size().
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    const BYTE flags = cur.src[cur.in_pos];
    cur.in_pos++;

    for (unsigned bit = 0; bit < flags_per_byte && cur.in_pos < cur.chunk_end;
         bit++) {
      // A word can decode past chunk_size; nothing in the encoding caps it.
      if (cur.out - cur.chunk_out_start >= chunk_size) {
        throw std::runtime_error(
            "LZNT1: chunk decompresses to more than 4096 bytes.\n");
      }

      if ((flags & static_cast<BYTE>(1U << bit)) == 0) {
        CopyLiteral(cur);
      } else {
        DecodeBackReference(cur);
      }
    }
  }
}

}  // namespace

size_t Decompress(std::span<const BYTE> src, std::span<BYTE> dest) {
  Cursor cur{.src = src, .dest = dest};

  while (cur.in_pos + header_size <= src.size()) {
    const WORD header = ReadLe16(src, cur.in_pos);
    cur.in_pos += header_size;

    // header == 0 is the End_of_buffer terminal ([MS-XCA] 2.5.1.2).
    if (header == 0) {
      break;
    }

    if ((header & header_signature_mask) != header_signature) {
      throw std::runtime_error("LZNT1: invalid chunk header signature.\n");
    }

    // payload = size + 3-byte bias - header; always >= 1, so loop advances.
    const size_t payload =
        (static_cast<size_t>(header & header_size_mask) + size_bias) -
        header_size;
    if (payload > src.size() - cur.in_pos) {
      throw std::runtime_error(
          "LZNT1: chunk exceeds compressed data bounds.\n");
    }
    cur.chunk_end = cur.in_pos + payload;

    if ((header & header_compressed) == 0) {
      // Uncompressed chunk: literal bytes follow the header ([MS-XCA] 2.5.1.2).
      if (payload > dest.size() - cur.out) {
        throw std::runtime_error(
            "LZNT1: uncompressed chunk exceeds decompressed bounds.\n");
      }
      // payload was checked against both src and dest above, so both are in
      // range.
      // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
      std::memcpy(&dest[cur.out], &src[cur.in_pos], payload);
      cur.out += payload;
      cur.in_pos = cur.chunk_end;
      continue;
    }

    DecompressChunk(cur);

    cur.in_pos = cur.chunk_end;
  }

  return cur.out;
}

}  // namespace NtfsBrowser::Lznt1
