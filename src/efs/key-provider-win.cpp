#ifdef _WIN32

  #include <ntfs-browser/win-types.h>

  #include <filesystem>
  #include <fstream>
  #include <string>

  #include <gsl/narrow>
  #include <ncrypt.h>
  #include <wincrypt.h>

  #include <ntfs-browser/efs.h>

  #include "efs/fek.h"
  #include "ntfs-common.h"
  #include "util.h"

namespace NtfsBrowser::Efs {

namespace {

// Certificate encodings a lookup by hash accepts.
constexpr DWORD cert_encoding = X509_ASN_ENCODING | PKCS_7_ASN_ENCODING;

// A PFX bigger than this is not a certificate bundle. Bounds the read.
constexpr std::streamsize max_pfx_size = 16 * 1024 * 1024;

using CertPtr =
    std::unique_ptr<const CERT_CONTEXT, decltype(&CertFreeCertificateContext)>;

// Owns a certificate's private key handle, CNG or CryptoAPI, and frees it
// when the certificate did not keep it.
class PrivateKey final {
 public:
  PrivateKey(const PrivateKey& other) = delete;
  PrivateKey& operator=(const PrivateKey& other) = delete;
  PrivateKey(PrivateKey&& other) noexcept = delete;
  PrivateKey& operator=(PrivateKey&& other) noexcept = delete;

  // Opens the key of "cert" without any user interface. Check IsValid().
  explicit PrivateKey(const CERT_CONTEXT& cert) noexcept {
    if (TakeEphemeralKey(cert)) {
      return;
    }

    BOOL must_free = FALSE;
    valid_ =
        CryptAcquireCertificatePrivateKey(
            &cert,
            CRYPT_ACQUIRE_PREFER_NCRYPT_KEY_FLAG | CRYPT_ACQUIRE_SILENT_FLAG,
            nullptr, &handle_, &key_spec_, &must_free) != FALSE;
    must_free_ = must_free != FALSE;
    if (!valid_) {
      Log::Debug("CryptAcquireCertificatePrivateKey failed: 0x{:08X}.",
                 GetLastError());
    }
  }

  ~PrivateKey() {
    if (!valid_ || !must_free_) {
      return;
    }
    if (key_spec_ == CERT_NCRYPT_KEY_SPEC) {
      NCryptFreeObject(handle_);
    } else {
      CryptReleaseContext(handle_, 0);
    }
  }

  [[nodiscard]] bool IsValid() const noexcept { return valid_; }

  // RSA-decrypts a wrapped FEK, PKCS#1 v1.5 padded, stored little-endian.
  [[nodiscard]] std::optional<std::vector<BYTE>>
      Decrypt(std::span<const BYTE> wrapped) const {
    if (key_spec_ == CERT_NCRYPT_KEY_SPEC) {
      return DecryptCng(wrapped);
    }
    return DecryptCapi(wrapped);
  }

 private:
  HCRYPTPROV_OR_NCRYPT_KEY_HANDLE handle_{0};
  DWORD key_spec_{0};
  bool must_free_{false};
  bool valid_{false};

  // A key imported without persisting has no container to open. The
  // certificate holds a live handle to it instead, which stays the
  // certificate's to free. Returns false if there is none.
  [[nodiscard]] bool TakeEphemeralKey(const CERT_CONTEXT& cert) noexcept {
    DWORD size = sizeof(handle_);
    if (CertGetCertificateContextProperty(&cert, CERT_NCRYPT_KEY_HANDLE_PROP_ID,
                                          &handle_, &size) != FALSE) {
      key_spec_ = CERT_NCRYPT_KEY_SPEC;
      valid_ = true;
      return true;
    }

    size = sizeof(handle_);
    DWORD spec = 0;
    DWORD spec_size = sizeof(spec);
    if (CertGetCertificateContextProperty(&cert, CERT_KEY_PROV_HANDLE_PROP_ID,
                                          &handle_, &size) != FALSE &&
        CertGetCertificateContextProperty(&cert, CERT_KEY_SPEC_PROP_ID, &spec,
                                          &spec_size) != FALSE) {
      key_spec_ = spec;
      valid_ = true;
      return true;
    }
    return false;
  }

  // CNG takes and gives big-endian numbers, so the little-endian FEK is
  // reversed on the way in.
  [[nodiscard]] std::optional<std::vector<BYTE>>
      DecryptCng(std::span<const BYTE> wrapped) const {
    std::vector<BYTE> input(wrapped.rbegin(), wrapped.rend());
    const auto key = static_cast<NCRYPT_KEY_HANDLE>(handle_);

    DWORD size = 0;
    if (FAILED(NCryptDecrypt(key, input.data(),
                             gsl::narrow<DWORD>(input.size()), nullptr, nullptr,
                             0, &size, NCRYPT_PAD_PKCS1_FLAG))) {
      return std::nullopt;
    }

    std::vector<BYTE> output(size);
    if (FAILED(NCryptDecrypt(
            key, input.data(), gsl::narrow<DWORD>(input.size()), nullptr,
            output.data(), size, &size, NCRYPT_PAD_PKCS1_FLAG))) {
      Util::SecureZero(output);
      return std::nullopt;
    }
    output.resize(size);
    return output;
  }

  // CryptoAPI is little-endian, like the stored FEK.
  [[nodiscard]] std::optional<std::vector<BYTE>>
      DecryptCapi(std::span<const BYTE> wrapped) const {
    HCRYPTKEY key = 0;
    if (CryptGetUserKey(handle_, key_spec_, &key) == FALSE) {
      return std::nullopt;
    }

    std::vector<BYTE> buffer(wrapped.begin(), wrapped.end());
    auto size = gsl::narrow<DWORD>(buffer.size());
    const BOOL ok = CryptDecrypt(key, 0, TRUE, 0, buffer.data(), &size);
    CryptDestroyKey(key);
    if (ok == FALSE) {
      Util::SecureZero(buffer);
      return std::nullopt;
    }
    buffer.resize(size);
    return buffer;
  }
};

// Unwraps FEKs with the keys of the certificates in one store.
class StoreKeyProvider final : public IEfsKeyProvider {
 public:
  explicit StoreKeyProvider(HCERTSTORE store) noexcept : store_(store) {}

  ~StoreKeyProvider() override { CertCloseStore(store_, 0); }

  StoreKeyProvider(StoreKeyProvider&& other) noexcept = delete;
  StoreKeyProvider(const StoreKeyProvider& other) = delete;
  StoreKeyProvider& operator=(StoreKeyProvider&& other) noexcept = delete;
  StoreKeyProvider& operator=(const StoreKeyProvider& other) = delete;

  std::optional<std::vector<BYTE>>
      UnwrapFek(std::span<const BYTE> thumbprint,
                std::span<const BYTE> wrapped_fek) const override {
    CRYPT_HASH_BLOB hash{gsl::narrow<DWORD>(thumbprint.size()),
                         const_cast<BYTE*>(thumbprint.data())};
    const CertPtr cert(CertFindCertificateInStore(store_, cert_encoding, 0,
                                                  CERT_FIND_SHA1_HASH, &hash,
                                                  nullptr),
                       &CertFreeCertificateContext);
    if (!cert) {
      return std::nullopt;
    }

    const PrivateKey key(*cert);
    if (!key.IsValid()) {
      Log::Debug("The certificate has no usable private key.");
      return std::nullopt;
    }
    return key.Decrypt(wrapped_fek);
  }

 private:
  HCERTSTORE store_;
};

}  // namespace

std::shared_ptr<IEfsKeyProvider> MakeCertStoreKeyProvider() {
  HCERTSTORE store = CertOpenStore(CERT_STORE_PROV_SYSTEM_W, 0, 0,
                                   CERT_SYSTEM_STORE_CURRENT_USER, L"My");
  if (store == nullptr) {
    Log::Warn("Cannot open the CurrentUser\\My certificate store.");
    return nullptr;
  }
  return std::make_shared<StoreKeyProvider>(store);
}

std::shared_ptr<IEfsKeyProvider>
    MakePfxKeyProvider(const std::filesystem::path& pfx_path,
                       std::wstring_view password) {
  std::error_code size_error;
  const std::uintmax_t file_size =
      std::filesystem::file_size(pfx_path, size_error);
  if (!size_error && file_size > static_cast<std::uintmax_t>(max_pfx_size)) {
    Log::Warn("The PFX is too large to be a certificate bundle.");
    return nullptr;
  }

  std::ifstream file(pfx_path, std::ios::binary);
  std::vector<BYTE> bytes;
  if (file) {
    // One byte more than expected: a file that grew still trips the limit.
    const std::streamsize capacity =
        (size_error ? max_pfx_size : static_cast<std::streamsize>(file_size)) +
        1;
    bytes.resize(gsl::narrow<size_t>(capacity));
    file.read(reinterpret_cast<char*>(bytes.data()), capacity);
    bytes.resize(gsl::narrow<size_t>(file.gcount()));
  }
  if (bytes.empty() || bytes.size() > static_cast<size_t>(max_pfx_size)) {
    Log::Warn("Cannot read a PFX from the given path.");
    return nullptr;
  }

  CRYPT_DATA_BLOB blob{gsl::narrow<DWORD>(bytes.size()), bytes.data()};
  const std::wstring password_z(password);
  HCERTSTORE store =
      PFXImportCertStore(&blob, password_z.c_str(), PKCS12_NO_PERSIST_KEY);
  if (store == nullptr) {
    Log::Warn("Cannot import the PFX: wrong password, or not a PFX.");
    return nullptr;
  }
  return std::make_shared<StoreKeyProvider>(store);
}

}  // namespace NtfsBrowser::Efs

#endif  // _WIN32
