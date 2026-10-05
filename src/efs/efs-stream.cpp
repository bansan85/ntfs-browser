#include "efs/efs-stream.h"

#include <ntfs-browser/win-types.h>

#include <cstring>

#include "ntfs-common.h"

namespace NtfsBrowser::Efs
{

namespace
{
// Offsets of the DDF and DRF offset fields in the $EFS header. Zero means
// the field is absent.
constexpr size_t ddf_offset_field = 0x40;
constexpr size_t drf_offset_field = 0x44;

// The $EFS header ends after its two offset fields.
constexpr size_t header_size = 0x48;

// A field is a DWORD entry count, then the entries back to back.
constexpr size_t count_size = sizeof(DWORD);

// Layout of one entry: its total length, the offset of its credential, the
// length and offset of its wrapped FEK. All offsets are from the entry start.
constexpr size_t entry_length_field = 0x00;
constexpr size_t entry_credential_field = 0x04;
constexpr size_t entry_fek_length_field = 0x08;
constexpr size_t entry_fek_offset_field = 0x0C;
constexpr size_t entry_min_size = 0x14;

// Offset, in the credential, of the offset of the certificate hash record.
constexpr size_t credential_hash_field = 0x10;

// The certificate hash record starts with the offset and size of the
// thumbprint, relative to the record. Its own size is not needed.
constexpr size_t hash_thumbprint_offset_field = 0x00;
constexpr size_t hash_thumbprint_size_field = 0x04;
constexpr size_t hash_record_min_size = 0x08;

// A SHA-1 hash is 20 bytes.
constexpr size_t thumbprint_size_value = 20;

// EFS files carry a handful of users. A hostile count must not make the
// parser loop for four billion entries.
constexpr DWORD max_entries = 64;

// The largest wrapped FEK kept: an RSA-8192 block.
constexpr size_t max_wrapped_fek_size = 1024;

// Bounds-checked reads over the stream. Every accessor fails on an offset or
// length that leaves the stream, whatever the arithmetic would wrap to.
class Reader
{
 public:
  explicit Reader(std::span<const BYTE> bytes) noexcept : bytes_(bytes) {}

  [[nodiscard]] std::optional<DWORD> Dword(ULONGLONG offset) const noexcept
  {
    const std::optional<std::span<const BYTE>> slice =
        Slice(offset, sizeof(DWORD));
    if (!slice)
    {
      return std::nullopt;
    }
    DWORD value = 0;
    std::memcpy(&value, slice->data(), sizeof(value));
    return value;
  }

  [[nodiscard]] std::optional<std::span<const BYTE>>
      Slice(ULONGLONG offset, ULONGLONG length) const noexcept
  {
    if (offset > bytes_.size() || length > bytes_.size() - offset)
    {
      return std::nullopt;
    }
    return bytes_.subspan(static_cast<size_t>(offset),
                          static_cast<size_t>(length));
  }

  [[nodiscard]] size_t Size() const noexcept { return bytes_.size(); }

 private:
  std::span<const BYTE> bytes_;
};

// Parses the entry that starts at "entryOffset". Returns its length, so the
// caller can step to the next one, and appends its FEK copy to "out".
[[nodiscard]] std::optional<ULONGLONG> ParseEntry(const Reader& reader,
                                                  ULONGLONG entry_offset,
                                                  std::vector<WrappedFek>& out)
{
  const std::optional<DWORD> length =
      reader.Dword(entry_offset + entry_length_field);
  const std::optional<DWORD> credential =
      reader.Dword(entry_offset + entry_credential_field);
  const std::optional<DWORD> fek_length =
      reader.Dword(entry_offset + entry_fek_length_field);
  const std::optional<DWORD> fek_offset =
      reader.Dword(entry_offset + entry_fek_offset_field);
  // The entry header is readable, and the entry fits in the stream.
  if (!length || !credential || !fek_length || !fek_offset ||
      *length < entry_min_size || !reader.Slice(entry_offset, *length))
  {
    return std::nullopt;
  }

  const ULONGLONG credential_start = entry_offset + *credential;
  const std::optional<DWORD> hash_offset =
      reader.Dword(credential_start + credential_hash_field);
  if (!hash_offset)
  {
    return std::nullopt;
  }

  const ULONGLONG hash_start = credential_start + *hash_offset;
  if (!reader.Slice(hash_start, hash_record_min_size))
  {
    return std::nullopt;
  }
  const std::optional<DWORD> thumbprint_offset =
      reader.Dword(hash_start + hash_thumbprint_offset_field);
  const std::optional<DWORD> thumbprint_size =
      reader.Dword(hash_start + hash_thumbprint_size_field);
  // The hash record is readable, holds a SHA-1 thumbprint, and the wrapped
  // FEK has a plausible size.
  if (!thumbprint_offset || !thumbprint_size ||
      *thumbprint_size != thumbprint_size_value || *fek_length == 0 ||
      *fek_length > max_wrapped_fek_size)
  {
    return std::nullopt;
  }

  const auto thumbprint =
      reader.Slice(hash_start + *thumbprint_offset, thumbprint_size_value);
  const auto fek = reader.Slice(entry_offset + *fek_offset, *fek_length);
  if (!thumbprint || !fek)
  {
    return std::nullopt;
  }

  out.push_back(
      {{thumbprint->begin(), thumbprint->end()}, {fek->begin(), fek->end()}});
  return *length;
}

// Parses the field that starts at "fieldOffset", if the header names one.
[[nodiscard]] bool ParseField(const Reader& reader, size_t offset_field_pos,
                              std::vector<WrappedFek>& out)
{
  const std::optional<DWORD> field_offset = reader.Dword(offset_field_pos);
  if (!field_offset)
  {
    return false;
  }
  if (*field_offset == 0)
  {
    return true;
  }

  const std::optional<DWORD> count = reader.Dword(*field_offset);
  if (!count || *count > max_entries)
  {
    return false;
  }

  ULONGLONG entry_offset = static_cast<ULONGLONG>(*field_offset) + count_size;
  for (DWORD i = 0; i < *count; ++i)
  {
    const std::optional<ULONGLONG> length =
        ParseEntry(reader, entry_offset, out);
    if (!length)
    {
      return false;
    }
    entry_offset += *length;
  }
  return true;
}
}  // namespace

std::optional<std::vector<WrappedFek>>
    ParseEfsStream(std::span<const BYTE> stream)
{
  const Reader reader(stream);
  std::vector<WrappedFek> entries;
  if (reader.Size() < header_size ||
      !ParseField(reader, ddf_offset_field, entries) ||
      !ParseField(reader, drf_offset_field, entries))
  {
    LogWarn("Malformed $EFS stream.");
    return std::nullopt;
  }
  return entries;
}

}  // namespace NtfsBrowser::Efs
