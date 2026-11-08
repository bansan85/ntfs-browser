#pragma once

#include <ntfs-browser/win-types.h>

#include <cstdint>
#include <functional>
#include <map>
#include <span>
#include <string_view>
#include <vector>

#include <ntfs-browser/namespace.h>

namespace NTFS_BROWSER_NAMESPACE::Tool::FuzzGen {

// The part of a forensic volume that a corpus trace touched, rebuilt without
// the image. A byte comes from the first rule that names it:
//   1. a patch of the residue, the bytes that no code can compute;
//   2. a layer, a structure that code computes: the "PTRN" fill, an identity
//      $UpCase table, a stamp repeated on every page, and the boot sector, file
//      records and index blocks that LayerScript builds from named fields, the
//      counter text of a log file, and bytes of a named resource. A
//      later layer wins over an earlier one;
//   3. zero.
// The addresses are volume relative, as the library asks for them.
class SparseImage {
 public:
  // Gives the bytes of a named resource, such as a boot code shared by the
  // volumes of one Windows version. It throws when the resource is missing.
  using BlobSource = std::function<std::vector<BYTE>(std::string_view name)>;

  // Parses the text of a layers file (see LayerScript for the lines) and the
  // binary residue of one trace. The residue is a series of patches, each a
  // 64-bit address, a 32-bit length (both little endian) and that many bytes.
  // Throws std::runtime_error on a line or a patch that it cannot read.
  [[nodiscard]] static SparseImage Load(std::string_view layers,
                                        std::span<const BYTE> residue,
                                        const BlobSource& blobs);

  // Adds the repeating "PTRN" fill over length bytes from address: the byte at
  // address a is "PTRN"[a % 4].
  void AddPtrn(ULONGLONG address, ULONGLONG length);

  // Adds an $UpCase table of units words from address that maps every unit to
  // itself: the 16-bit word at address + 2 * k is k.
  void AddIdentityUpcase(ULONGLONG address, ULONGLONG units);

  // Adds the text of CounterText over length bytes from address, so that the
  // byte at address is the byte of the text at offset origin.
  void AddCounterText(ULONGLONG address, ULONGLONG length, ULONGLONG origin);

  // Adds a copy of stamp at address, address + stride, and so on, count times.
  void AddStamp(ULONGLONG address, ULONGLONG stride, ULONGLONG count,
                std::vector<BYTE> stamp);

  // Adds bytes at address.
  void AddBytes(ULONGLONG address, std::vector<BYTE> bytes);

  // Adds the patches of residue (see Load).
  void AddPatches(std::span<const BYTE> residue);

  // Fills dest with the bytes at address.
  void Read(ULONGLONG address, std::span<BYTE> dest) const;

 private:
  // The structures that a layer can compute.
  enum class LayerKind : std::uint8_t {
    Ptrn,
    IdentityUpcase,
    Stamp,
    Bytes,
    CounterText
  };

  struct Layer {
    LayerKind kind = LayerKind::Ptrn;
    ULONGLONG address = 0;
    // Bytes the layer covers.
    ULONGLONG length = 0;
    // The distance between two stamps.
    ULONGLONG stride = 0;
    // The offset in the text of the first byte of a counter text layer.
    ULONGLONG origin = 0;
    // The bytes of a stamp, or of a block of bytes.
    std::vector<BYTE> bytes;
  };

  // Writes the bytes of layer that fall in the window starting at address.
  static void ApplyLayer(const Layer& layer, ULONGLONG address,
                         std::span<BYTE> dest);

  std::vector<Layer> layers_;
  // The patches, by address. They never overlap.
  std::map<ULONGLONG, std::vector<BYTE>> patches_;
};

}  // namespace NTFS_BROWSER_NAMESPACE::Tool::FuzzGen
