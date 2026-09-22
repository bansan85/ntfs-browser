#include "efs/efs-stream.h"

#include <cstring>

#include "ntfs-common.h"

namespace NtfsBrowser::Efs
{

namespace
{
// Offsets of the DDF and DRF offset fields in the $EFS header. Zero means
// the field is absent.
constexpr size_t kDdfOffsetField = 0x40;
constexpr size_t kDrfOffsetField = 0x44;

// The $EFS header ends after its two offset fields.
constexpr size_t kHeaderSize = 0x48;

// A field is a DWORD entry count, then the entries back to back.
constexpr size_t kCountSize = sizeof(DWORD);

// Layout of one entry: its total length, the offset of its credential, the
// length and offset of its wrapped FEK. All offsets are from the entry start.
constexpr size_t kEntryLengthField = 0x00;
constexpr size_t kEntryCredentialField = 0x04;
constexpr size_t kEntryFekLengthField = 0x08;
constexpr size_t kEntryFekOffsetField = 0x0C;
constexpr size_t kEntryMinSize = 0x14;

// Offset, in the credential, of the offset of the certificate hash record.
constexpr size_t kCredentialHashField = 0x10;

// The certificate hash record starts with the offset and size of the
// thumbprint, relative to the record. Its own size is not needed.
constexpr size_t kHashThumbprintOffsetField = 0x00;
constexpr size_t kHashThumbprintSizeField = 0x04;
constexpr size_t kHashRecordMinSize = 0x08;

// A SHA-1 hash is 20 bytes.
constexpr size_t kThumbprintSize = 20;

// EFS files carry a handful of users. A hostile count must not make the
// parser loop for four billion entries.
constexpr DWORD kMaxEntries = 64;

// The largest wrapped FEK kept: an RSA-8192 block.
constexpr size_t kMaxWrappedFekSize = 1024;

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
                                                  ULONGLONG entryOffset,
                                                  std::vector<WrappedFek>& out)
{
  const std::optional<DWORD> length =
      reader.Dword(entryOffset + kEntryLengthField);
  const std::optional<DWORD> credential =
      reader.Dword(entryOffset + kEntryCredentialField);
  const std::optional<DWORD> fekLength =
      reader.Dword(entryOffset + kEntryFekLengthField);
  const std::optional<DWORD> fekOffset =
      reader.Dword(entryOffset + kEntryFekOffsetField);
  // The entry header is readable, and the entry fits in the stream.
  if (!length || !credential || !fekLength || !fekOffset ||
      *length < kEntryMinSize || !reader.Slice(entryOffset, *length))
  {
    return std::nullopt;
  }

  const ULONGLONG credentialStart = entryOffset + *credential;
  const std::optional<DWORD> hashOffset =
      reader.Dword(credentialStart + kCredentialHashField);
  if (!hashOffset)
  {
    return std::nullopt;
  }

  const ULONGLONG hashStart = credentialStart + *hashOffset;
  if (!reader.Slice(hashStart, kHashRecordMinSize))
  {
    return std::nullopt;
  }
  const std::optional<DWORD> thumbprintOffset =
      reader.Dword(hashStart + kHashThumbprintOffsetField);
  const std::optional<DWORD> thumbprintSize =
      reader.Dword(hashStart + kHashThumbprintSizeField);
  // The hash record is readable, holds a SHA-1 thumbprint, and the wrapped
  // FEK has a plausible size.
  if (!thumbprintOffset || !thumbprintSize ||
      *thumbprintSize != kThumbprintSize || *fekLength == 0 ||
      *fekLength > kMaxWrappedFekSize)
  {
    return std::nullopt;
  }

  const auto thumbprint =
      reader.Slice(hashStart + *thumbprintOffset, kThumbprintSize);
  const auto fek = reader.Slice(entryOffset + *fekOffset, *fekLength);
  if (!thumbprint || !fek)
  {
    return std::nullopt;
  }

  out.push_back(
      {{thumbprint->begin(), thumbprint->end()}, {fek->begin(), fek->end()}});
  return *length;
}

// Parses the field that starts at "fieldOffset", if the header names one.
[[nodiscard]] bool ParseField(const Reader& reader, size_t offsetFieldPos,
                              std::vector<WrappedFek>& out)
{
  const std::optional<DWORD> fieldOffset = reader.Dword(offsetFieldPos);
  if (!fieldOffset)
  {
    return false;
  }
  if (*fieldOffset == 0)
  {
    return true;
  }

  const std::optional<DWORD> count = reader.Dword(*fieldOffset);
  if (!count || *count > kMaxEntries)
  {
    return false;
  }

  ULONGLONG entryOffset = static_cast<ULONGLONG>(*fieldOffset) + kCountSize;
  for (DWORD i = 0; i < *count; ++i)
  {
    const std::optional<ULONGLONG> length =
        ParseEntry(reader, entryOffset, out);
    if (!length)
    {
      return false;
    }
    entryOffset += *length;
  }
  return true;
}
}  // namespace

std::optional<std::vector<WrappedFek>>
    ParseEfsStream(std::span<const BYTE> stream)
{
  const Reader reader(stream);
  std::vector<WrappedFek> entries;
  if (reader.Size() < kHeaderSize ||
      !ParseField(reader, kDdfOffsetField, entries) ||
      !ParseField(reader, kDrfOffsetField, entries))
  {
    LogWarn("Malformed $EFS stream.");
    return std::nullopt;
  }
  return entries;
}

}  // namespace NtfsBrowser::Efs
