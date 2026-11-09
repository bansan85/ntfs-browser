#include "lznt1-compressor.h"

#include <algorithm>
#include <array>
#include <climits>
#include <cstddef>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <unordered_map>
#include <vector>

#include <gsl/narrow>

#include <ntfs-browser/namespace.h>

#include "win-types.h"

namespace NTFS_BROWSER_NAMESPACE::Tool::FuzzGen {

namespace {

// The signature bits of a chunk header (bits 12 to 14), the flag of a
// compressed chunk (bit 15), and the mask of the size field (bits 0 to 11).
constexpr WORD header_signature = 0x3000;
constexpr WORD header_compressed = 0x8000;
constexpr WORD header_size_mask = 0x0FFF;

// The shortest match worth a token, and the fewest bits of an offset: the
// format fixes both.
constexpr std::size_t min_match = 3;
constexpr unsigned min_offset_bits = 4;

// Bits in a byte, and the mask of a byte.
constexpr unsigned byte_bits = CHAR_BIT;
constexpr unsigned byte_mask = 0xFF;

// The bits of the 16-bit token of a match: the offset and the length share
// them.
constexpr unsigned token_bits = 16;

// Items in a group: one flag byte tells which are matches.
constexpr unsigned group_items = 8;

// How many places of a three-byte string the table keeps: XP finds the
// matches of its text output with two.
constexpr std::size_t places_per_string = 2;

// The places of one string, oldest first.
struct Places {
  std::array<std::size_t, places_per_string> at{};
  std::size_t count = 0;
};

// The fewest bits that an offset can need at position pos of the chunk.
unsigned OffsetBits(std::size_t pos) {
  unsigned bits = min_offset_bits;
  while ((std::size_t{1} << bits) < pos) {
    ++bits;
  }
  return bits;
}

// The three bytes at pos, as one key.
std::uint32_t KeyAt(std::span<const BYTE> plain, std::size_t pos) {
  return (std::uint32_t{plain[pos]} << (2 * byte_bits)) |
         (std::uint32_t{plain[pos + 1]} << byte_bits) | plain[pos + 2];
}

// One item of the compressed data: a literal byte, or the token of a match.
struct Item {
  bool match = false;
  WORD token = 0;
  BYTE literal = 0;
};

// Writes the items in groups of eight, each after its flag byte.
std::vector<BYTE> Pack(const std::vector<Item>& items) {
  std::vector<BYTE> out;
  for (std::size_t first = 0; first < items.size(); first += group_items) {
    const std::size_t last = std::min(items.size(), first + group_items);
    BYTE flags = 0;
    for (std::size_t i = first; i < last; ++i) {
      if (items[i].match) {
        flags = static_cast<BYTE>(flags | (1U << (i - first)));
      }
    }
    out.push_back(flags);
    for (std::size_t i = first; i < last; ++i) {
      if (items[i].match) {
        out.push_back(static_cast<BYTE>(items[i].token & byte_mask));
        out.push_back(static_cast<BYTE>(items[i].token >> byte_bits));
      } else {
        out.push_back(items[i].literal);
      }
    }
  }
  return out;
}

// The best match that the table finds for a position: its length in bytes (0
// when none) and how far back it starts.
struct Match {
  std::size_t length = 0;
  std::size_t offset = 0;
};

// The bits of the length field of a match at position pos of the chunk.
unsigned LengthBits(std::size_t pos) { return token_bits - OffsetBits(pos); }

// The latest places of every three-byte string of one chunk.
class MatchTable {
 public:
  explicit MatchTable(std::span<const BYTE> plain) : plain_(plain) {}

  // Adds the string that starts at pos. A position too near the end holds no
  // three-byte string and is left out.
  void Remember(std::size_t pos) {
    if (pos + min_match > plain_.size()) {
      return;
    }
    Places& places = table_[KeyAt(plain_, pos)];
    if (places.count == places_per_string) {
      places.at[0] = places.at[1];
      --places.count;
    }
    places.at[places.count++] = pos;
  }

  // The longest match for the text at pos among the places of its string. The
  // nearest place goes first: a longer match wins, an equal one does not.
  [[nodiscard]] Match FindBest(std::size_t pos) const {
    Match best;
    if (pos + min_match > plain_.size()) {
      return best;
    }
    const auto found = table_.find(KeyAt(plain_, pos));
    if (found == table_.end()) {
      return best;
    }
    const std::size_t max_length =
        (std::size_t{1} << LengthBits(pos)) + min_match - 1;
    for (std::size_t left = found->second.count; left > 0; --left) {
      const std::size_t from = found->second.at[left - 1];
      std::size_t length = 0;
      while (pos + length < plain_.size() && length < max_length &&
             plain_[from + length] == plain_[pos + length]) {
        ++length;
      }
      if (length > best.length) {
        best = {.length = length, .offset = pos - from};
      }
    }
    return best;
  }

 private:
  std::span<const BYTE> plain_;
  std::unordered_map<std::uint32_t, Places> table_;
};

// The item for a match at position pos: the offset and the length share the
// 16 bits of the token.
Item MatchItem(const Match& match, std::size_t pos) {
  const std::size_t coded =
      ((match.offset - 1) << LengthBits(pos)) | (match.length - min_match);
  return {.match = true, .token = gsl::narrow<WORD>(coded), .literal = 0};
}

// The two bytes of a chunk header, low byte first.
std::vector<BYTE> HeaderBytes(WORD header) {
  return {static_cast<BYTE>(header & byte_mask),
          static_cast<BYTE>(header >> byte_bits)};
}

// The chunk of plain with its header. The body is the packed items. The chunk
// keeps the plain bytes as is when the body does not shrink them.
std::vector<BYTE> WrapChunk(std::span<const BYTE> plain,
                            const std::vector<BYTE>& body) {
  if (body.size() >= Lznt1Compressor::chunk_bytes) {
    std::vector<BYTE> chunk =
        HeaderBytes(gsl::narrow<WORD>(header_signature | (plain.size() - 1)));
    chunk.insert(chunk.end(), plain.begin(), plain.end());
    return chunk;
  }
  std::vector<BYTE> chunk =
      HeaderBytes(gsl::narrow<WORD>(header_signature | header_compressed |
                                    ((body.size() - 1) & header_size_mask)));
  chunk.insert(chunk.end(), body.begin(), body.end());
  return chunk;
}

}  // namespace

std::vector<BYTE> Lznt1Compressor::Chunk(std::span<const BYTE> plain) {
  if (plain.empty() || plain.size() > chunk_bytes) {
    throw std::invalid_argument("an LZNT1 chunk holds 1 to 4096 bytes");
  }
  MatchTable table(plain);
  std::vector<Item> items;
  std::size_t pos = 0;
  while (pos < plain.size()) {
    const Match match = table.FindBest(pos);
    if (match.length >= min_match) {
      items.push_back(MatchItem(match, pos));
      table.Remember(pos);
      pos += match.length;
    } else {
      items.push_back({.match = false, .token = 0, .literal = plain[pos]});
      table.Remember(pos);
      ++pos;
    }
  }
  return WrapChunk(plain, Pack(items));
}

}  // namespace NTFS_BROWSER_NAMESPACE::Tool::FuzzGen
