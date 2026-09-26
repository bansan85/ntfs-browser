#pragma once

#include <ntfs-browser/win-types.h>

#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include <ntfs-browser/efs.h>
#include <ntfs-browser/strategy.h>

#include "efs/efs-stream.h"
#include "efs/sector-cipher.h"

namespace NtfsBrowser
{
template <Strategy S>
class AttrNonResident;
}  // namespace NtfsBrowser

namespace NtfsBrowser::Efs
{

// The bit of an attribute header's flags that marks its stream as encrypted.
inline constexpr WORD kAttrFlagEncrypted = 0x4000;

// Where a record's key provider comes from. It is called on the first
// decryption only, so a volume creates its default provider lazily.
using KeyProviderSource = std::function<std::shared_ptr<IEfsKeyProvider>()>;

// The decryption state one file record shares between its encrypted $DATA
// streams: its $EFS entries, and the key resolved from them once. A failed
// resolution is remembered too, so a file nobody holds a key for is not
// retried on every read.
class Context
{
 public:
  // "entries" is empty when the record has no usable $EFS stream.
  Context(std::vector<WrappedFek> entries, KeyProviderSource providerSource);
  Context(Context&& other) noexcept = delete;
  Context(Context const& other) = delete;
  Context& operator=(Context&& other) noexcept = delete;
  Context& operator=(Context const& other) = delete;
  ~Context() = default;

  template <Strategy S>
  friend class NtfsBrowser::AttrNonResident;

 private:
  std::vector<WrappedFek> entries_;
  KeyProviderSource provider_source_;

  // Set by the first Decrypt(), whether or not it found a key.
  mutable bool resolved_{false};
  mutable std::unique_ptr<SectorDecryptor> decryptor_;
  mutable std::string failure_;

  // Decrypts data in place. "streamOffset" is the byte offset of data[0] in
  // its stream, and both it and the size MUST be sector aligned. Returns
  // false, with a warning naming the cause, if the data cannot be decrypted.
  [[nodiscard]] bool Decrypt(ULONGLONG streamOffset,
                             std::span<BYTE> data) const;

  void Resolve() const;
  [[nodiscard]] std::unique_ptr<SectorDecryptor>
      MakeDecryptor(const Fek& fek) const;
};

}  // namespace NtfsBrowser::Efs
