#include "efs/efs-context.h"

#include "ntfs-common.h"

namespace NtfsBrowser::Efs
{

Context::Context(std::vector<WrappedFek> entries,
                 KeyProviderSource providerSource)
    : entries_(std::move(entries)), provider_source_(std::move(providerSource))
{
}

// Builds the decryptor of the selected backend. Falls back to Crypto++ when
// BCrypt is not the selected one, or cannot take this cipher (DESX).
std::unique_ptr<SectorDecryptor> Context::MakeDecryptor(const Fek& fek) const
{
#ifdef _WIN32
  if (GetCipherBackend() == CipherBackend::kBCrypt)
  {
    if (std::unique_ptr<SectorDecryptor> decryptor = MakeBCryptDecryptor(fek))
    {
      return decryptor;
    }
    LogDebug("BCrypt declined the FEK. Using Crypto++.");
  }
#endif
  return MakeCryptoPpDecryptor(fek);
}

// Tries each $EFS entry with the provider, until one yields a usable key.
// Leaves the cause in failure_ when none does.
void Context::Resolve() const
{
  resolved_ = true;

  if (entries_.empty())
  {
    failure_ = "the record has no usable $EFS stream";
    return;
  }

  const std::shared_ptr<IEfsKeyProvider> provider =
      provider_source_ ? provider_source_() : nullptr;
  if (!provider)
  {
    failure_ = "no EFS key provider is installed";
    return;
  }

  failure_ = "no key provider holds a key for this file";
  for (const WrappedFek& entry : entries_)
  {
    std::optional<std::vector<BYTE>> blob =
        provider->UnwrapFek(entry.thumbprint, entry.wrapped_fek);
    if (!blob)
    {
      continue;
    }

    const std::optional<Fek> fek = Fek::Parse(*blob);
    SecureZero(*blob);
    if (!fek)
    {
      failure_ = "the unwrapped FEK is unusable";
      continue;
    }

    decryptor_ = MakeDecryptor(*fek);
    if (decryptor_)
    {
      failure_.clear();
      return;
    }
    failure_ = "the cipher cannot be set up with this FEK";
  }
}

bool Context::Decrypt(ULONGLONG streamOffset, std::span<BYTE> data) const
{
  if (!resolved_)
  {
    Resolve();
  }

  if (!decryptor_)
  {
    LogWarn("Cannot decrypt the stream: {}.", failure_);
    return false;
  }

  if (data.size() % kSectorSize != 0 || streamOffset % kSectorSize != 0)
  {
    LogWarn("Encrypted read is not sector aligned.");
    return false;
  }

  for (size_t done = 0; done < data.size(); done += kSectorSize)
  {
    if (!decryptor_->DecryptSector(streamOffset + done,
                                   data.subspan(done, kSectorSize)))
    {
      return false;
    }
  }
  return true;
}

}  // namespace NtfsBrowser::Efs
