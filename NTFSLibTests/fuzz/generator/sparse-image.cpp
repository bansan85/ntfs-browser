#include <ntfs-browser/win-types.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include <gsl/narrow>

#include <ntfs-browser/namespace.h>
#include <ntfs-browser/tool/fuzzgen/sparse-image.h>

namespace NTFS_BROWSER_NAMESPACE::Tool::FuzzGen {

namespace {

// The fill of the PTRN images, written before the volume was formatted.
constexpr std::array<BYTE, 4> ptrn_fill = {'P', 'T', 'R', 'N'};

// Bytes of the header of a patch: a 64-bit address and a 32-bit length.
constexpr std::size_t patch_header_bytes =
    sizeof(std::uint64_t) + sizeof(std::uint32_t);

// Bytes of one word of an $UpCase table.
constexpr ULONGLONG upcase_word_bytes = 2;

// Bits in a byte.
constexpr unsigned byte_bits = 8;

// Reads an unsigned integer of type T, little endian, at the start of bytes.
template <typename T>
T LoadLittleEndian(std::span<const BYTE> bytes) {
  T value = 0;
  for (std::size_t i = 0; i < sizeof(T); ++i) {
    value |= static_cast<T>(static_cast<T>(bytes[i]) << (byte_bits * i));
  }
  return value;
}

}  // namespace

SparseImage SparseImage::Load(std::string_view layers,
                              std::span<const BYTE> residue) {
  SparseImage image;
  std::istringstream lines{std::string(layers)};
  std::string line;
  while (std::getline(lines, line)) {
    std::istringstream fields(line);
    std::string kind;
    ULONGLONG address = 0;
    ULONGLONG size = 0;
    if (!(fields >> kind)) {
      continue;
    }
    if (!(fields >> address >> size)) {
      throw std::runtime_error("bad layer line: " + line);
    }
    if (kind == "ptrn") {
      image.layers_.push_back({LayerKind::Ptrn, address, size});
    } else if (kind == "upcase") {
      image.layers_.push_back(
          {LayerKind::IdentityUpcase, address, size * upcase_word_bytes});
    } else {
      throw std::runtime_error("unknown layer kind: " + kind);
    }
  }

  std::size_t position = 0;
  while (position < residue.size()) {
    if (residue.size() - position < patch_header_bytes) {
      throw std::runtime_error("the residue ends inside a patch header");
    }
    const auto address =
        LoadLittleEndian<std::uint64_t>(residue.subspan(position));
    const auto length = LoadLittleEndian<std::uint32_t>(
        residue.subspan(position + sizeof(std::uint64_t)));
    position += patch_header_bytes;
    if (residue.size() - position < length) {
      throw std::runtime_error("the residue ends inside a patch");
    }
    const std::span<const BYTE> bytes = residue.subspan(position, length);
    image.patches_.emplace(address,
                           std::vector<BYTE>(bytes.begin(), bytes.end()));
    position += length;
  }
  return image;
}

void SparseImage::ApplyLayer(const Layer& layer, ULONGLONG address,
                             std::span<BYTE> dest) {
  const ULONGLONG low = std::max(address, layer.address);
  const ULONGLONG high =
      std::min(address + dest.size(), layer.address + layer.length);
  for (ULONGLONG position = low; position < high; ++position) {
    BYTE& out = dest[gsl::narrow<std::size_t>(position - address)];
    if (layer.kind == LayerKind::Ptrn) {
      out = ptrn_fill[position % ptrn_fill.size()];
    } else {
      const ULONGLONG word = (position - layer.address) / upcase_word_bytes;
      out = gsl::narrow_cast<BYTE>(
          word >> (byte_bits * (position % upcase_word_bytes)));
    }
  }
}

void SparseImage::Read(ULONGLONG address, std::span<BYTE> dest) const {
  std::ranges::fill(dest, BYTE{0});
  for (const Layer& layer : layers_) {
    ApplyLayer(layer, address, dest);
  }

  // The last patch that starts at or before the window may reach into it.
  auto patch = patches_.upper_bound(address);
  if (patch != patches_.begin()) {
    --patch;
  }
  for (; patch != patches_.end() && patch->first < address + dest.size();
       ++patch) {
    const ULONGLONG low = std::max(address, patch->first);
    const ULONGLONG high =
        std::min(address + dest.size(), patch->first + patch->second.size());
    if (low < high) {
      std::memcpy(dest.data() + (low - address),
                  patch->second.data() + (low - patch->first),
                  gsl::narrow<std::size_t>(high - low));
    }
  }
}

}  // namespace NTFS_BROWSER_NAMESPACE::Tool::FuzzGen
