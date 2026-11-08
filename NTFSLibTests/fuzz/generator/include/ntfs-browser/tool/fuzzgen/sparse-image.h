#pragma once

#include <ntfs-browser/win-types.h>

#include <cstdint>
#include <map>
#include <span>
#include <string_view>
#include <vector>

#include <ntfs-browser/namespace.h>

namespace NTFS_BROWSER_NAMESPACE::Tool::FuzzGen {

// The part of a forensic volume that a corpus trace touched, rebuilt without
// the image. A byte comes from the first rule that names it:
//   1. a patch of the residue, the bytes that no code can compute;
//   2. a layer, a structure that code computes (the "PTRN" fill, an identity
//      $UpCase table);
//   3. zero.
// The addresses are volume relative, as the library asks for them.
class SparseImage {
 public:
  // The structures that a layer can compute.
  enum class LayerKind : std::uint8_t {
    // The repeating "PTRN" fill: the byte at address a is "PTRN"[a % 4].
    Ptrn,
    // An $UpCase table that maps every unit to itself: the 16-bit word at
    // address + 2 * k is k.
    IdentityUpcase
  };

  // Parses the text of a layers file and the binary residue of one trace.
  // The text holds one layer per line: "ptrn ADDRESS LENGTH" or
  // "upcase ADDRESS UNITS". The residue is a series of patches, each a 64-bit
  // address, a 32-bit length (both little endian) and that many bytes. Throws
  // std::runtime_error on a line or a patch that it cannot read.
  [[nodiscard]] static SparseImage Load(std::string_view layers,
                                        std::span<const BYTE> residue);

  // Fills dest with the bytes at address.
  void Read(ULONGLONG address, std::span<BYTE> dest) const;

 private:
  struct Layer {
    LayerKind kind = LayerKind::Ptrn;
    ULONGLONG address = 0;
    // Bytes the layer covers.
    ULONGLONG length = 0;
  };

  // Writes the bytes of layer that fall in the window starting at address.
  static void ApplyLayer(const Layer& layer, ULONGLONG address,
                         std::span<BYTE> dest);

  std::vector<Layer> layers_;
  // The patches, by address. They never overlap.
  std::map<ULONGLONG, std::vector<BYTE>> patches_;
};

}  // namespace NTFS_BROWSER_NAMESPACE::Tool::FuzzGen
