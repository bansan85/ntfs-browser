#pragma once

#include <cstddef>
#include <span>
#include <vector>

#include <ntfs-browser/namespace.h>

#include "win-types.h"

namespace NTFS_BROWSER_NAMESPACE::Tool::FuzzGen {

// An LZNT1 compressor for the files that Windows XP compressed on an NTFS
// volume. It reproduces the bytes of XP for text, to within a few chunks in a
// hundred: XP finds its matches in a hash table of unknown shape, and this
// class stands for it with the two latest places of every three-byte string.
// The library only decompresses, so the generator carries this one.
class Lznt1Compressor {
 public:
  // Bytes of plain text in a chunk.
  static constexpr std::size_t chunk_bytes = 4096;

  // The chunk of plain, which holds at most chunk_bytes bytes: a two-byte
  // header and the compressed data, or the data as is when it does not shrink.
  [[nodiscard]] static std::vector<BYTE> Chunk(std::span<const BYTE> plain);
};

}  // namespace NTFS_BROWSER_NAMESPACE::Tool::FuzzGen
