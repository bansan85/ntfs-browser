#include "sparse-image.h"

#include <ntfs-browser/win-types.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <gsl/narrow>

#include <ntfs-browser/namespace.h>

#include "counter-text.h"
#include "layer-script.h"

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
                              std::span<const BYTE> residue,
                              const BlobSource& blobs) {
  SparseImage image;
  LayerScript::Fill(image, layers, blobs);
  image.AddPatches(residue);
  return image;
}

void SparseImage::AddPtrn(ULONGLONG address, ULONGLONG length) {
  layers_.push_back({.kind = LayerKind::Ptrn,
                     .address = address,
                     .length = length,
                     .stride = 0,
                     .origin = 0,
                     .bytes = {}});
}

void SparseImage::AddIdentityUpcase(ULONGLONG address, ULONGLONG units) {
  layers_.push_back({.kind = LayerKind::IdentityUpcase,
                     .address = address,
                     .length = units * upcase_word_bytes,
                     .stride = 0,
                     .origin = 0,
                     .bytes = {}});
}

void SparseImage::AddCounterText(ULONGLONG address, ULONGLONG length,
                                 ULONGLONG origin) {
  layers_.push_back({.kind = LayerKind::CounterText,
                     .address = address,
                     .length = length,
                     .stride = 0,
                     .origin = origin,
                     .bytes = {}});
}

void SparseImage::AddStamp(ULONGLONG address, ULONGLONG stride, ULONGLONG count,
                           std::vector<BYTE> stamp) {
  if (count == 0 || stride < stamp.size()) {
    throw std::runtime_error(
        "a stamp must repeat at least once, and its "
        "copies must not overlap");
  }
  const ULONGLONG length = ((count - 1) * stride) + stamp.size();
  layers_.push_back({.kind = LayerKind::Stamp,
                     .address = address,
                     .length = length,
                     .stride = stride,
                     .origin = 0,
                     .bytes = std::move(stamp)});
}

void SparseImage::AddBytes(ULONGLONG address, std::vector<BYTE> bytes) {
  const ULONGLONG length = bytes.size();
  layers_.push_back({.kind = LayerKind::Bytes,
                     .address = address,
                     .length = length,
                     .stride = 0,
                     .origin = 0,
                     .bytes = std::move(bytes)});
}

void SparseImage::AddPatches(std::span<const BYTE> residue) {
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
    patches_.emplace(address, std::vector<BYTE>(bytes.begin(), bytes.end()));
    position += length;
  }
}

void SparseImage::ApplyLayer(const Layer& layer, ULONGLONG address,
                             std::span<BYTE> dest) {
  const ULONGLONG low = std::max(address, layer.address);
  const ULONGLONG high =
      std::min(address + dest.size(), layer.address + layer.length);
  if (layer.kind == LayerKind::CounterText) {
    if (low < high) {
      CounterText::Fill(layer.origin + (low - layer.address),
                        dest.subspan(gsl::narrow<std::size_t>(low - address),
                                     gsl::narrow<std::size_t>(high - low)));
    }
    return;
  }
  for (ULONGLONG position = low; position < high; ++position) {
    BYTE& out = dest[gsl::narrow<std::size_t>(position - address)];
    const ULONGLONG inside = position - layer.address;
    switch (layer.kind) {
      case LayerKind::Ptrn:
        out = ptrn_fill[position % ptrn_fill.size()];
        break;
      case LayerKind::IdentityUpcase:
        out = gsl::narrow_cast<BYTE>(
            (inside / upcase_word_bytes) >>
            (byte_bits * (position % upcase_word_bytes)));
        break;
      case LayerKind::Stamp: {
        // The bytes between two stamps belong to the layers below.
        const ULONGLONG within = inside % layer.stride;
        if (within < layer.bytes.size()) {
          out = layer.bytes[gsl::narrow<std::size_t>(within)];
        }
        break;
      }
      case LayerKind::Bytes:
        out = layer.bytes[gsl::narrow<std::size_t>(inside)];
        break;
      case LayerKind::CounterText:
        break;
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
