#include <ntfs-browser/win-types.h>

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <vector>

#include <ntfs-browser/namespace.h>
#include <ntfs-browser/tool/fuzzgen/hand-made-efs.h>

namespace NTFS_BROWSER_NAMESPACE::Tool::FuzzGen {

namespace {

// Where the DDF and DRF offsets sit in the header, where the header ends and
// the count of the DDF starts, and where the first entry starts.
constexpr std::size_t ddf_field = 0x40;
constexpr std::size_t drf_field = 0x44;
constexpr std::size_t count_at = 0x48;
constexpr std::size_t first_entry_at = 0x4C;

// The fields of an entry, counted from its start: its length, the offset of
// its credential, the length and the offset of its FEK.
constexpr std::size_t entry_length_field = 0x00;
constexpr std::size_t entry_credential_field = 0x04;
constexpr std::size_t entry_fek_length_field = 0x08;
constexpr std::size_t entry_fek_offset_field = 0x0C;

// The credential has its hash offset at 0x10. The hash record holds the
// offset and the size of the thumbprint.
constexpr std::size_t credential_hash_field = 0x10;
constexpr std::size_t hash_thumbprint_field = 0x00;
constexpr std::size_t hash_size_field = 0x04;

// In a healthy entry the credential follows the fields at 0x14, its hash
// record 0x18 bytes later, and the FEK follows the thumbprint at 0x48.
constexpr std::size_t healthy_credential_at = 0x14;
constexpr std::size_t healthy_hash_at = healthy_credential_at + 0x18;
constexpr std::size_t healthy_thumbprint_at = healthy_hash_at + 8;
constexpr std::size_t healthy_fek_at = 0x48;
constexpr std::size_t thumbprint_bytes = 20;

// The FEK blob header: its size, and the field of the algorithm.
constexpr std::size_t fek_header_bytes = 16;
constexpr std::size_t fek_algorithm_field = 8;

// Stores word at offset, when it fits.
void Put(std::vector<BYTE>& bytes, std::size_t offset, DWORD word) {
  if (offset + sizeof(word) <= bytes.size()) {
    std::memcpy(&bytes[offset], &word, sizeof(word));
  }
}

}  // namespace

std::vector<BYTE> HandMadeEfs::EntryBytes(const Entry& entry) {
  std::vector<BYTE> bytes(healthy_fek_at + entry.fek.size(), 0);
  Put(bytes, entry_length_field,
      entry.length.value_or(static_cast<DWORD>(bytes.size())));
  Put(bytes, entry_credential_field, entry.credential_offset);
  Put(bytes, entry_fek_length_field,
      entry.fek_length.value_or(static_cast<DWORD>(entry.fek.size())));
  Put(bytes, entry_fek_offset_field, static_cast<DWORD>(healthy_fek_at));
  Put(bytes, healthy_credential_at + credential_hash_field, entry.hash_offset);
  Put(bytes, healthy_hash_at + hash_thumbprint_field, entry.thumbprint_offset);
  Put(bytes, healthy_hash_at + hash_size_field, entry.thumbprint_size);
  bytes[healthy_thumbprint_at] = entry.seed;
  std::fill_n(bytes.begin() +
                  static_cast<std::ptrdiff_t>(healthy_thumbprint_at + 1),
              thumbprint_bytes - 1, entry.fill);
  std::ranges::copy(entry.fek, bytes.begin() +
                                   static_cast<std::ptrdiff_t>(healthy_fek_at));
  return bytes;
}

std::vector<BYTE> HandMadeEfs::ValueBytes(const Value& value) {
  std::vector<BYTE> bytes(first_entry_at, 0);
  Put(bytes, ddf_field, value.ddf_offset);
  Put(bytes, drf_field, value.drf_offset);
  Put(bytes, count_at,
      value.count.value_or(static_cast<DWORD>(value.entries.size())));
  for (const Entry& entry : value.entries) {
    const std::vector<BYTE> entry_bytes = EntryBytes(entry);
    bytes.insert(bytes.end(), entry_bytes.begin(), entry_bytes.end());
  }
  if (value.length) {
    bytes.resize(*value.length, 0);
  }
  return bytes;
}

std::vector<BYTE> HandMadeEfs::FekBlob(DWORD key_length, DWORD algorithm,
                                       std::size_t key_bytes) {
  std::vector<BYTE> blob(fek_header_bytes + key_bytes, 0);
  std::memcpy(blob.data(), &key_length, sizeof(key_length));
  std::memcpy(&blob[fek_algorithm_field], &algorithm, sizeof(algorithm));
  for (std::size_t i = 0; i < key_bytes; ++i) {
    blob[fek_header_bytes + i] = static_cast<BYTE>(i);
  }
  return blob;
}

}  // namespace NTFS_BROWSER_NAMESPACE::Tool::FuzzGen
