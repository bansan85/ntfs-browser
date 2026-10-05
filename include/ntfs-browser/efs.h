#pragma once

#include <ntfs-browser/win-types.h>

#include <cstdint>
#include <optional>
#include <span>
#include <vector>

#ifdef _WIN32
  #include <filesystem>
#endif

#include <ntfs-browser/export.h>

namespace NtfsBrowser::Efs
{

// Which library decrypts the file data once the key is known.
enum class CipherBackend : std::uint8_t
{
  CryptoPp,
  BCrypt
};

// Unwraps the File Encryption Key (FEK) of an EFS file. A file carries one
// RSA-wrapped copy of it per user allowed to read the file.
class IEfsKeyProvider
{
 public:
  IEfsKeyProvider() = default;
  IEfsKeyProvider(IEfsKeyProvider&& other) noexcept = delete;
  IEfsKeyProvider(IEfsKeyProvider const& other) = delete;
  IEfsKeyProvider& operator=(IEfsKeyProvider&& other) noexcept = delete;
  IEfsKeyProvider& operator=(IEfsKeyProvider const& other) = delete;
  virtual ~IEfsKeyProvider() = default;

  // RSA-decrypts wrappedFek with the private key of the certificate whose
  // SHA-1 hash is thumbprint. Returns the decrypted FEK blob, or nullopt if
  // this provider does not hold that key. wrappedFek is as stored on disk:
  // little-endian, the CryptoAPI byte order. The caller wipes the result.
  [[nodiscard]] virtual std::optional<std::vector<BYTE>>
      UnwrapFek(std::span<const BYTE> thumbprint,
                std::span<const BYTE> wrapped_fek) const = 0;
};

#ifdef _WIN32
// Keys from the current user's personal certificate store (CurrentUser\My).
// This is the provider a volume creates by itself, on its first decryption,
// when none was installed.
[[nodiscard]] NTFS_BROWSER_EXPORT std::shared_ptr<IEfsKeyProvider>
    MakeCertStoreKeyProvider();

// Keys from a PFX (PKCS#12) file. Returns null, with a warning logged, if the
// file cannot be read or the password is wrong. The keys stay in memory: they
// are never added to the user's key storage.
[[nodiscard]] NTFS_BROWSER_EXPORT std::shared_ptr<IEfsKeyProvider>
    MakePfxKeyProvider(const std::filesystem::path& pfx_path,
                       std::wstring_view password);
#endif

}  // namespace NtfsBrowser::Efs
