#pragma once

#include <ntfs-browser/win-types.h>

#include <cstddef>
#include <optional>
#include <vector>

#include <ntfs-browser/namespace.h>

namespace NTFS_BROWSER_NAMESPACE::Tool::FuzzGen {

// Writes the $EFS stream of the hand-made inputs: a header, then a DDF whose
// entries each hold a credential with a thumbprint and a wrapped FEK. Every
// field a variant may damage is a member, named as the stream names it.
class HandMadeEfs {
 public:
  // One entry of the DDF. Offsets count from the start of the entry, of the
  // credential or of the hash record, as the stream does.
  struct Entry {
    // The length the entry declares. Empty means the bytes it has.
    std::optional<DWORD> length;
    // The offset of the credential.
    DWORD credential_offset = 0x14;
    // The offset of the certificate hash record in the credential.
    DWORD hash_offset = 0x18;
    // The offset and the size of the thumbprint in the hash record.
    DWORD thumbprint_offset = 8;
    DWORD thumbprint_size = 0x14;
    // The thumbprint: its first byte, then the byte that fills the rest.
    BYTE seed = 0;
    BYTE fill = 0;
    // The wrapped FEK, and the length the entry declares for it. Empty means
    // the bytes it has.
    std::vector<BYTE> fek;
    std::optional<DWORD> fek_length;
  };

  // The header and the DDF of a value.
  struct Value {
    // The offsets of the DDF and of the DRF. Zero means absent.
    DWORD ddf_offset = 0x48;
    DWORD drf_offset = 0;
    // The count the DDF declares. Empty means the entries given.
    std::optional<DWORD> count;
    // The entries, written one after the other.
    std::vector<Entry> entries;
    // The length of the value in bytes. Empty means the whole. A shorter
    // value cuts the bytes off. A longer one pads with zeros.
    std::optional<std::size_t> length;
  };

  // The bytes of one entry.
  [[nodiscard]] static std::vector<BYTE> EntryBytes(const Entry& entry);

  // The bytes of a value.
  [[nodiscard]] static std::vector<BYTE> ValueBytes(const Value& value);

  // The FEK blob of the first fixtures: 16 bytes of header, then the key,
  // which counts up from zero so that a misplaced slice shows. The header
  // holds the key length, a field that is zero, and the algorithm.
  [[nodiscard]] static std::vector<BYTE>
      FekBlob(DWORD key_length, DWORD algorithm, std::size_t key_bytes);

  // The algorithm codes of AES with a key of 128 and of 256 bits.
  static constexpr DWORD aes128 = 0x660E;
  static constexpr DWORD aes256 = 0x6610;
};

}  // namespace NTFS_BROWSER_NAMESPACE::Tool::FuzzGen
