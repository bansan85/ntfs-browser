#ifdef _WIN32

  #include <ntfs-browser/win-types.h>

  #include <bcrypt.h>

  #include "efs/sector-cipher.h"
  #include "ntfs-common.h"

namespace NtfsBrowser::Efs
{

namespace
{
// BCrypt in CBC mode, restarted with a fresh IV per sector. Owns its
// algorithm provider and key handles.
class BCryptDecryptor final : public SectorDecryptor
{
 public:
  BCryptDecryptor() = default;
  BCryptDecryptor(BCryptDecryptor&& other) noexcept = delete;
  BCryptDecryptor(BCryptDecryptor const& other) = delete;
  BCryptDecryptor& operator=(BCryptDecryptor&& other) noexcept = delete;
  BCryptDecryptor& operator=(BCryptDecryptor const& other) = delete;

  ~BCryptDecryptor() override
  {
    if (key_ != nullptr)
    {
      BCryptDestroyKey(key_);
    }
    if (alg_ != nullptr)
    {
      BCryptCloseAlgorithmProvider(alg_, 0);
    }
  }

  // Opens the algorithm in CBC mode and imports the key. Returns false if
  // BCrypt refuses either.
  [[nodiscard]] bool Init(LPCWSTR algorithmId, std::span<const BYTE> key,
                          size_t blockSize)
  {
    block_size_ = blockSize;
    if (!BCRYPT_SUCCESS(
            BCryptOpenAlgorithmProvider(&alg_, algorithmId, nullptr, 0)))
    {
      return false;
    }

    // BCrypt wants the chaining mode as a wide string, terminator included.
    constexpr std::wstring_view kCbc = BCRYPT_CHAIN_MODE_CBC;
    if (!BCRYPT_SUCCESS(BCryptSetProperty(
            alg_, BCRYPT_CHAINING_MODE,
            reinterpret_cast<PUCHAR>(const_cast<wchar_t*>(kCbc.data())),
            static_cast<ULONG>((kCbc.size() + 1) * sizeof(wchar_t)), 0)))
    {
      return false;
    }

    return BCRYPT_SUCCESS(BCryptGenerateSymmetricKey(
        alg_, &key_, nullptr, 0, const_cast<PUCHAR>(key.data()),
        static_cast<ULONG>(key.size()), 0));
  }

  bool DecryptSector(ULONGLONG offset, std::span<BYTE> sector) const override
  {
    if (sector.size() != kSectorSize)
    {
      return false;
    }

    // BCrypt advances the IV it is given, so it gets a copy.
    std::array<BYTE, kMaxBlockSize> iv = MakeSectorIv(offset);
    ULONG produced = 0;
    const NTSTATUS status = BCryptDecrypt(
        key_, sector.data(), static_cast<ULONG>(sector.size()), nullptr,
        iv.data(), static_cast<ULONG>(block_size_), sector.data(),
        static_cast<ULONG>(sector.size()), &produced, 0);
    return BCRYPT_SUCCESS(status) && produced == sector.size();
  }

 private:
  BCRYPT_ALG_HANDLE alg_{nullptr};
  BCRYPT_KEY_HANDLE key_{nullptr};
  size_t block_size_{0};
};

// The AES block size, and the DES-family one, in bytes.
constexpr size_t kAesBlockSize = 16;
constexpr size_t kDesBlockSize = 8;
}  // namespace

std::unique_ptr<SectorDecryptor> MakeBCryptDecryptor(const Fek& fek)
{
  LPCWSTR algorithmId = nullptr;
  size_t blockSize = 0;
  switch (fek.GetAlgorithm())
  {
    case Algorithm::kAes128:
    case Algorithm::kAes192:
    case Algorithm::kAes256:
      algorithmId = BCRYPT_AES_ALGORITHM;
      blockSize = kAesBlockSize;
      break;
    case Algorithm::k3Des:
      algorithmId = BCRYPT_3DES_ALGORITHM;
      blockSize = kDesBlockSize;
      break;
    case Algorithm::kDesx:
      return nullptr;
  }

  auto decryptor = std::make_unique<BCryptDecryptor>();
  if (!decryptor->Init(algorithmId, fek.GetKey(), blockSize))
  {
    LogWarn("BCrypt cannot use this FEK.");
    return nullptr;
  }
  return decryptor;
}

}  // namespace NtfsBrowser::Efs

#endif  // _WIN32
