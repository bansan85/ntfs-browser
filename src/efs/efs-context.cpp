#include "efs/efs-context.h"

#include <ntfs-browser/win-types.h>

#include <cstddef>
#include <optional>
#include <utility>

#include <ntfs-browser/efs/efs.h>

#include "efs/fek.h"
#include "log/ntfs-common.h"
#include "util/util.h"

namespace NtfsBrowser::Efs {

Context::Context(std::vector<WrappedFek> entries,
                 KeyProviderSource provider_source, CipherBackend backend)
    : entries_(std::move(entries)),
      provider_source_(std::move(provider_source)),
      backend_(backend) {}

// Builds the decryptor of the selected backend. Falls back to Crypto++ when
// BCrypt is not the selected one, or cannot take this cipher (DESX).
// NOLINTNEXTLINE(readability-convert-member-functions-to-static)
std::unique_ptr<SectorDecryptor> Context::MakeDecryptor(const Fek& fek) const {
#if defined(_WIN32) && defined(NTFS_BROWSER_ENABLE_EFS_BCRYPT)
  if (backend_ == CipherBackend::BCrypt) {
    if (std::unique_ptr<SectorDecryptor> decryptor = MakeBCryptDecryptor(fek)) {
      return decryptor;
    }
  #ifdef NTFS_BROWSER_ENABLE_EFS_CRYPTOPP
    Log::Debug("BCrypt declined the FEK. Using Crypto++.");
  #else
    Log::Debug("BCrypt declined the FEK. No Crypto++ fallback compiled in.");
  #endif
  }
#endif
#ifdef NTFS_BROWSER_ENABLE_EFS_CRYPTOPP
  return MakeCryptoPpDecryptor(fek);
#else
  return nullptr;
#endif
}

// Tries each $EFS entry with the provider, until one yields a usable key.
// Leaves the cause in failure_ when none does.
void Context::Resolve() const {
  resolved_ = true;

  if (entries_.empty()) {
    failure_ = "the record has no usable $EFS stream";
    return;
  }

  const std::shared_ptr<IEfsKeyProvider> provider =
      provider_source_ ? provider_source_() : nullptr;
  if (!provider) {
    failure_ = "no EFS key provider is installed";
    return;
  }

  failure_ = "no key provider holds a key for this file";
  for (const WrappedFek& entry : entries_) {
    std::optional<std::vector<BYTE>> blob =
        provider->UnwrapFek(entry.thumbprint, entry.wrapped_fek);
    if (!blob) {
      continue;
    }

    const std::optional<Fek> fek = Fek::Parse(*blob);
    Util::SecureZero(*blob);
    if (!fek) {
      failure_ = "the unwrapped FEK is unusable";
      continue;
    }

    decryptor_ = MakeDecryptor(*fek);
    if (decryptor_) {
      failure_.clear();
      return;
    }
    failure_ = "the cipher cannot be set up with this FEK";
  }
}

bool Context::Decrypt(ULONGLONG stream_offset, std::span<BYTE> data) const {
  if (!resolved_) {
    Resolve();
  }

  if (!decryptor_) {
    Log::Warn("Cannot decrypt the stream: {}.", failure_);
    return false;
  }

  if (data.size() % sector_size != 0 || stream_offset % sector_size != 0) {
    Log::Warn("Encrypted read is not sector aligned.");
    return false;
  }

  for (size_t done = 0; done < data.size(); done += sector_size) {
    if (!decryptor_->DecryptSector(stream_offset + done,
                                   data.subspan(done, sector_size))) {
      return false;
    }
  }
  return true;
}

}  // namespace NtfsBrowser::Efs
