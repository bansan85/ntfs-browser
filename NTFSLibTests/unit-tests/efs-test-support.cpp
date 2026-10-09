#include "efs-test-support.h"

#include <ntfs-browser/win-types.h>

#include <algorithm>
#include <cstring>
#include <optional>
#include <random>
#include <utility>

#include <gsl/narrow>

// The reference cipher is Crypto++ when it is compiled in, BCrypt otherwise.
#ifdef NTFS_BROWSER_ENABLE_EFS_CRYPTOPP

  #include <cryptopp/aes.h>
  #include <cryptopp/des.h>
  // Silences the weak-algorithm notice: MD5 is what the DESX key expansion
  // uses.
  // NOLINTNEXTLINE(cppcoreguidelines-macro-usage)
  #define CRYPTOPP_ENABLE_NAMESPACE_WEAK 1
  #include <cryptopp/md5.h>
  #include <cryptopp/modes.h>

#elif defined(_WIN32) && defined(NTFS_BROWSER_ENABLE_EFS_BCRYPT)

  #include <array>
  #include <cwchar>
  #include <format>
  #include <stdexcept>

  #include <bcrypt.h>

#else

  #error "efs-test-support.cpp needs Crypto++ or, on Windows, BCrypt"

#endif

namespace NtfsBrowserTests {

namespace {

// EFS encrypts one 512-byte sector at a time.
constexpr size_t sector_value = 512;

// Cipher block sizes, in bytes.
constexpr size_t aes_block_size = 16;
constexpr size_t des_block_size = 8;

// Size of an MD5 digest, and of one DESX salt: 11 characters and their NUL.
constexpr size_t md5_digest_size = 16;
constexpr size_t desx_salt_size = 12;

// Key lengths, in bytes. DESX takes 128 bits, which the cipher expands.
constexpr size_t aes128_key_size = 16;
constexpr size_t aes192_key_size = 24;
constexpr size_t aes256_key_size = 32;
constexpr size_t _3_des_key_size = 24;
constexpr size_t desx_key_size = 16;

// How TestKey() fills a key byte: (index * step) + seed.
constexpr size_t key_byte_step = 7;
constexpr size_t key_byte_seed = 0x21;

// Size of the FEK blob header: key length, entropy, algorithm, reserved.
constexpr size_t fek_header_size = 16;

// How TestThumbprint() steps from one byte to the next.
constexpr size_t thumbprint_byte_step = 13;

// Seed of the plaintext pattern's engine: the 64-bit golden ratio constant.
constexpr ULONGLONG pattern_seed = 0x9E3779B97F4A7C15ULL;

// Shift that keeps the top byte of a 64-bit engine word.
constexpr unsigned top_byte_shift = 56;

// Field offsets of a synthetic $EFS stream, entry, credential and hash record.
constexpr size_t stream_version_field = 0x08;
constexpr size_t ddf_offset_field = 0x40;
constexpr size_t drf_offset_field = 0x44;
constexpr size_t entry_fek_length_field = 0x08;
constexpr size_t entry_fek_offset_field = 0x0C;
constexpr size_t credential_hash_field = 0x10;

// Offset, in the credential, of its SID type DWORD, and the value it holds.
constexpr size_t credential_sid_type_field = 0x08;
constexpr DWORD sid_type_user = 3;

// Offsets, in a FEK blob, of the key length, entropy length and algorithm.
constexpr size_t fek_key_length_field = 0;
constexpr size_t fek_entropy_length_field = 4;
constexpr size_t fek_algorithm_field = 8;

// The two IV words, as read off a real AES-256 EFS file. Repeated here on
// purpose: the tests must not share the library's copy.
constexpr ULONGLONG iv_word0 = 0x5816657BE9161312ULL;
constexpr ULONGLONG iv_word1 = 0x1989ADBE44918961ULL;

// The IV word of the DES family: 3DES and DESX. It is a single 8-byte word,
// since the block is 8 bytes. Taken from ntfs-3g, not from the AES constants.
constexpr ULONGLONG des_iv_word = 0x169119629891AD13ULL;

// Little-endian CBC IV of the sector at "offset": two words for the 16-byte
// AES block, one for the 8-byte DES block.
std::vector<BYTE> SectorIv(ULONGLONG offset, size_t block_size) {
  std::vector<BYTE> initialization_vector(block_size, 0);
  if (block_size == aes_block_size) {
    const ULONGLONG first_word = iv_word0 + offset;
    const ULONGLONG second_word = iv_word1 + offset;
    std::memcpy(initialization_vector.data(), &first_word, sizeof(first_word));
    // iv holds blockSize == aes_block_size = 16 bytes here.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    std::memcpy(&initialization_vector[sizeof(first_word)], &second_word,
                sizeof(second_word));
  } else {
    const ULONGLONG first_word = des_iv_word + offset;
    std::memcpy(initialization_vector.data(), &first_word, sizeof(first_word));
  }
  return initialization_vector;
}

// Pads the plaintext with zeros up to a whole number of sectors.
std::vector<BYTE> PadToSectors(std::span<const BYTE> plaintext) {
  std::vector<BYTE> data(plaintext.begin(), plaintext.end());
  data.resize(((data.size() + sector_value - 1) / sector_value) * sector_value,
              0);
  return data;
}

#ifdef NTFS_BROWSER_ENABLE_EFS_CRYPTOPP

template <class BlockCipher>
std::vector<BYTE> EncryptWith(std::span<const BYTE> key,
                              std::span<const BYTE> plaintext,
                              ULONGLONG stream_offset) {
  std::vector<BYTE> data = PadToSectors(plaintext);

  typename BlockCipher::Encryption cipher;
  cipher.SetKey(key.data(), key.size());
  for (size_t done = 0; done < data.size(); done += sector_value) {
    const auto initialization_vector =
        SectorIv(stream_offset + done, BlockCipher::BLOCKSIZE);
    CryptoPP::CBC_Mode_ExternalCipher::Encryption cbc(
        cipher, initialization_vector.data());
    // done < data.size() by the loop condition.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    BYTE* const sector = &data[done];
    cbc.ProcessData(sector, sector, sector_value);
  }
  return data;
}

std::vector<BYTE> EncryptAes(std::span<const BYTE> key,
                             std::span<const BYTE> plaintext,
                             ULONGLONG stream_offset) {
  return EncryptWith<CryptoPP::AES>(key, plaintext, stream_offset);
}

std::vector<BYTE> Encrypt3Des(std::span<const BYTE> key,
                              std::span<const BYTE> plaintext,
                              ULONGLONG stream_offset) {
  return EncryptWith<CryptoPP::DES_EDE3>(key, plaintext, stream_offset);
}

// MD5 of "first" followed by "second".
std::array<BYTE, md5_digest_size> Md5Of(std::span<const BYTE> first,
                                        std::span<const BYTE> second) {
  std::array<BYTE, md5_digest_size> digest{};
  CryptoPP::Weak::MD5 hash;
  hash.Update(first.data(), first.size());
  hash.Update(second.data(), second.size());
  hash.Final(digest.data());
  return digest;
}

// Single-block DES in ECB mode: the core of the hand-written DESX.
class DesBlockCipher {
 public:
  explicit DesBlockCipher(std::span<const BYTE> key) {
    des_.SetKey(key.data(), key.size());
  }

  void ProcessBlock(BYTE* block) const { des_.ProcessBlock(block); }

 private:
  CryptoPP::DES::Encryption des_;
};

#else

void CheckStatus(NTSTATUS status, const char* what) {
  if (!BCRYPT_SUCCESS(status)) {
    throw std::runtime_error(
        std::format("{} failed: 0x{:08x}", what, static_cast<ULONG>(status)));
  }
}

// An open BCrypt algorithm provider, set to one chaining mode.
class BcryptAlgorithm {
 public:
  BcryptAlgorithm(LPCWSTR algorithm_id, LPCWSTR chaining_mode) {
    CheckStatus(BCryptOpenAlgorithmProvider(&handle_, algorithm_id, nullptr, 0),
                "BCryptOpenAlgorithmProvider");
    // The property value is the mode name with its terminating NUL.
    const size_t mode_bytes = (std::wcslen(chaining_mode) + 1) * sizeof(WCHAR);
    CheckStatus(BCryptSetProperty(
                    handle_, BCRYPT_CHAINING_MODE,
                    reinterpret_cast<PUCHAR>(const_cast<LPWSTR>(chaining_mode)),
                    gsl::narrow<ULONG>(mode_bytes), 0),
                "BCryptSetProperty");
  }

  BcryptAlgorithm(const BcryptAlgorithm&) = delete;
  BcryptAlgorithm& operator=(const BcryptAlgorithm&) = delete;

  ~BcryptAlgorithm() { BCryptCloseAlgorithmProvider(handle_, 0); }

  [[nodiscard]] BCRYPT_ALG_HANDLE Get() const noexcept { return handle_; }

 private:
  BCRYPT_ALG_HANDLE handle_ = nullptr;
};

// A BCrypt symmetric key. Encrypts without padding, so the data MUST be a
// whole number of blocks.
class BcryptKey {
 public:
  BcryptKey(const BcryptAlgorithm& algorithm, std::span<const BYTE> key) {
    CheckStatus(BCryptGenerateSymmetricKey(algorithm.Get(), &handle_, nullptr,
                                           0, const_cast<PUCHAR>(key.data()),
                                           gsl::narrow<ULONG>(key.size()), 0),
                "BCryptGenerateSymmetricKey");
  }

  BcryptKey(const BcryptKey&) = delete;
  BcryptKey& operator=(const BcryptKey&) = delete;

  ~BcryptKey() { BCryptDestroyKey(handle_); }

  // Encrypts "data" in place. "iv" is consumed by the call: pass a copy.
  void Encrypt(std::span<BYTE> data, std::span<BYTE> iv) const {
    std::vector<BYTE> out(data.size());
    ULONG written = 0;
    CheckStatus(BCryptEncrypt(handle_, data.data(),
                              gsl::narrow<ULONG>(data.size()), nullptr,
                              iv.empty() ? nullptr : iv.data(),
                              gsl::narrow<ULONG>(iv.size()), out.data(),
                              gsl::narrow<ULONG>(out.size()), &written, 0),
                "BCryptEncrypt");
    std::ranges::copy(out, data.begin());
  }

 private:
  BCRYPT_KEY_HANDLE handle_ = nullptr;
};

// AES or 3DES in CBC mode, one fresh IV per sector, as EFS does.
std::vector<BYTE> EncryptCbc(LPCWSTR algorithm_id, size_t block_size,
                             std::span<const BYTE> key,
                             std::span<const BYTE> plaintext,
                             ULONGLONG stream_offset) {
  std::vector<BYTE> data = PadToSectors(plaintext);

  const BcryptAlgorithm algorithm(algorithm_id, BCRYPT_CHAIN_MODE_CBC);
  const BcryptKey cipher(algorithm, key);
  for (size_t done = 0; done < data.size(); done += sector_value) {
    std::vector<BYTE> initialization_vector =
        SectorIv(stream_offset + done, block_size);
    // done < data.size() by the loop condition, which is sector-aligned.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    cipher.Encrypt(std::span<BYTE>(&data[done], sector_value),
                   initialization_vector);
  }
  return data;
}

std::vector<BYTE> EncryptAes(std::span<const BYTE> key,
                             std::span<const BYTE> plaintext,
                             ULONGLONG stream_offset) {
  return EncryptCbc(BCRYPT_AES_ALGORITHM, aes_block_size, key, plaintext,
                    stream_offset);
}

std::vector<BYTE> Encrypt3Des(std::span<const BYTE> key,
                              std::span<const BYTE> plaintext,
                              ULONGLONG stream_offset) {
  return EncryptCbc(BCRYPT_3DES_ALGORITHM, des_block_size, key, plaintext,
                    stream_offset);
}

// MD5 of "first" followed by "second".
std::array<BYTE, md5_digest_size> Md5Of(std::span<const BYTE> first,
                                        std::span<const BYTE> second) {
  std::vector<BYTE> joined(first.begin(), first.end());
  joined.insert(joined.end(), second.begin(), second.end());
  std::array<BYTE, md5_digest_size> digest{};
  CheckStatus(BCryptHash(BCRYPT_MD5_ALG_HANDLE, nullptr, 0, joined.data(),
                         gsl::narrow<ULONG>(joined.size()), digest.data(),
                         gsl::narrow<ULONG>(digest.size())),
              "BCryptHash");
  return digest;
}

// Single-block DES in ECB mode: the core of the hand-written DESX.
class DesBlockCipher {
 public:
  explicit DesBlockCipher(std::span<const BYTE> key)
      : algorithm_(BCRYPT_DES_ALGORITHM, BCRYPT_CHAIN_MODE_ECB),
        key_(algorithm_, key) {}

  void ProcessBlock(BYTE* block) const {
    key_.Encrypt(std::span<BYTE>(block, des_block_size), {});
  }

 private:
  BcryptAlgorithm algorithm_;
  BcryptKey key_;
};

#endif

// Encrypts as EFS does with DESX, written out by hand: a 128-bit FEK is
// expanded with MD5 into a DES key and two whitening keys, and each block is
// out_whitening ^ DES(block ^ prev ^ in_whitening), chained per sector.
std::vector<BYTE> EncryptDesx(std::span<const BYTE> key,
                              std::span<const BYTE> plaintext,
                              ULONGLONG stream_offset) {
  // The salts include their terminating NUL: 12 bytes each.
  constexpr std::array<char, desx_salt_size> salt1{"Dan Simon  "};
  constexpr std::array<char, desx_salt_size> salt2{"Scott Field"};

  const auto digest = [&key](const std::array<char, desx_salt_size>& salt) {
    return Md5Of(
        key, std::span<const BYTE>(reinterpret_cast<const BYTE*>(salt.data()),
                                   salt.size()));
  };

  const std::array<BYTE, md5_digest_size> md1 = digest(salt1);
  std::array<DWORD, 4> words1{};
  std::memcpy(words1.data(), md1.data(), md1.size());
  // words1 holds 4 words.
  // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
  const DWORD low = words1[0] ^ words1[1];
  // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
  const DWORD high = words1[2] ^ words1[3];
  const std::array<DWORD, 2> des_key_words{low, high};
  std::array<BYTE, des_block_size> des_key{};
  std::memcpy(des_key.data(), des_key_words.data(), des_key.size());

  const std::array<BYTE, md5_digest_size> md2 = digest(salt2);
  ULONGLONG out_whitening = 0;
  ULONGLONG in_whitening = 0;
  std::memcpy(&out_whitening, md2.data(), sizeof(out_whitening));
  // md2 is a 16-byte MD5 digest.
  // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
  std::memcpy(&in_whitening, &md2[sizeof(out_whitening)], sizeof(in_whitening));

  const DesBlockCipher des(des_key);

  std::vector<BYTE> data = PadToSectors(plaintext);
  for (size_t done = 0; done < data.size(); done += sector_value) {
    const auto initialization_vector =
        SectorIv(stream_offset + done, des_block_size);
    ULONGLONG prev = 0;
    std::memcpy(&prev, initialization_vector.data(), sizeof(prev));
    for (size_t block = 0; block < sector_value; block += des_block_size) {
      // data.size() is a multiple of sector, done < data.size() and block <
      // sector.
      // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
      BYTE* bytes = &data[done + block];
      ULONGLONG value = 0;
      std::memcpy(&value, bytes, sizeof(value));
      value ^= prev ^ in_whitening;
      std::memcpy(bytes, &value, sizeof(value));
      des.ProcessBlock(bytes);
      std::memcpy(&value, bytes, sizeof(value));
      value ^= out_whitening;
      std::memcpy(bytes, &value, sizeof(value));
      prev = value;
    }
  }
  return data;
}

void Put32(std::vector<BYTE>& out, size_t offset, DWORD value) {
  std::memcpy(&out.at(offset), &value, sizeof(value));
}

// Appends one entry, in the layout of a real one, to "out".
void AppendEntry(std::vector<BYTE>& out, const TestEfsEntry& user) {
  // The credential: a 28-byte SID, then the certificate hash record. Five
  // DWORDs, then the 20-byte thumbprint.
  constexpr size_t sid_offset = 0x1C;
  constexpr size_t hash_offset = 0x38;
  constexpr size_t hash_header = 0x14;
  constexpr size_t credential_size =
      hash_offset + hash_header + thumbprint_size_value;
  constexpr size_t entry_header = 0x14;

  const size_t base = out.size();
  const size_t fek_offset = entry_header + credential_size;
  out.resize(base + fek_offset + user.wrapped_fek.size(), 0);

  Put32(out, base + 0x00, gsl::narrow<DWORD>(out.size() - base));
  Put32(out, base + 0x04, entry_header);
  Put32(out, base + entry_fek_length_field,
        gsl::narrow<DWORD>(user.wrapped_fek.size()));
  Put32(out, base + entry_fek_offset_field, gsl::narrow<DWORD>(fek_offset));

  const size_t cred = base + entry_header;
  Put32(out, cred + 0x00, static_cast<DWORD>(credential_size));
  Put32(out, cred + 0x04, sid_offset);
  Put32(out, cred + credential_sid_type_field, sid_type_user);
  Put32(out, cred + credential_hash_field, hash_offset);

  const size_t hash = cred + hash_offset;
  Put32(out, hash + 0x00, hash_header);
  Put32(out, hash + 0x04, gsl::narrow<DWORD>(thumbprint_size_value));
  std::memcpy(&out.at(hash + hash_header), user.thumbprint.data(),
              thumbprint_size_value);
  std::memcpy(&out.at(base + fek_offset), user.wrapped_fek.data(),
              user.wrapped_fek.size());
}

// Appends a field (count, then entries) and returns its offset.
size_t AppendField(std::vector<BYTE>& out,
                   std::span<const TestEfsEntry> users) {
  const size_t offset = out.size();
  out.resize(offset + sizeof(DWORD), 0);
  Put32(out, offset, gsl::narrow<DWORD>(users.size()));
  for (const TestEfsEntry& user : users) {
    AppendEntry(out, user);
  }
  return offset;
}

}  // namespace

std::vector<BYTE> TestKey(Algorithm algorithm) {
  size_t length = 0;
  switch (algorithm) {
    case Algorithm::Aes128:
      length = aes128_key_size;
      break;
    case Algorithm::Aes192:
      length = aes192_key_size;
      break;
    case Algorithm::_3Des:
      length = _3_des_key_size;
      break;
    case Algorithm::Desx:
      length = desx_key_size;
      break;
    case Algorithm::Aes256:
      length = aes256_key_size;
      break;
  }

  std::vector<BYTE> key(length);
  for (size_t i = 0; i < length; ++i) {
    // i < length = key.size() by the loop condition.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    key[i] = gsl::narrow<BYTE>((i * key_byte_step) + key_byte_seed);
  }
  return key;
}

std::vector<BYTE> MakeFekBlob(Algorithm algorithm, std::span<const BYTE> key) {
  std::vector<BYTE> blob(fek_header_size + key.size(), 0);
  Put32(blob, fek_key_length_field, gsl::narrow<DWORD>(key.size()));
  Put32(blob, fek_entropy_length_field, gsl::narrow<DWORD>(key.size()));
  Put32(blob, fek_algorithm_field, static_cast<DWORD>(algorithm));
  std::ranges::copy(key, blob.begin() + fek_header_size);
  return blob;
}

std::vector<BYTE> EfsEncrypt(Algorithm algorithm, std::span<const BYTE> key,
                             std::span<const BYTE> plaintext,
                             ULONGLONG stream_offset) {
  switch (algorithm) {
    case Algorithm::Aes128:
    case Algorithm::Aes192:
    case Algorithm::Aes256:
      return EncryptAes(key, plaintext, stream_offset);
    case Algorithm::_3Des:
      return Encrypt3Des(key, plaintext, stream_offset);
    case Algorithm::Desx:
      return EncryptDesx(key, plaintext, stream_offset);
  }
  return {};
}

std::vector<BYTE> PlaintextPattern(size_t size) {
  std::vector<BYTE> bytes(size);
  // A fixed seed keeps the pattern reproducible.
  // NOLINTNEXTLINE(bugprone-random-generator-seed,cert-msc32-c,cert-msc51-cpp)
  std::mt19937_64 engine(pattern_seed);
  for (BYTE& byte : bytes) {
    byte = static_cast<BYTE>(engine() >> top_byte_shift);
  }
  return bytes;
}

std::array<BYTE, thumbprint_size_value> TestThumbprint(BYTE seed) {
  std::array<BYTE, thumbprint_size_value> thumbprint{};
  for (size_t i = 0; i < thumbprint.size(); ++i) {
    // i < thumbprint.size() by the loop condition.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    thumbprint[i] = static_cast<BYTE>(seed + (i * thumbprint_byte_step));
  }
  return thumbprint;
}

std::vector<BYTE> MakeEfsStream(std::span<const TestEfsEntry> users,
                                std::span<const TestEfsEntry> recovery) {
  // The header ends where the first field starts, as in a real stream.
  constexpr size_t header_size = 0x54;
  std::vector<BYTE> out(header_size, 0);
  Put32(out, stream_version_field, 2);

  if (!users.empty()) {
    Put32(out, ddf_offset_field, gsl::narrow<DWORD>(AppendField(out, users)));
  }
  if (!recovery.empty()) {
    Put32(out, drf_offset_field,
          gsl::narrow<DWORD>(AppendField(out, recovery)));
  }
  Put32(out, 0x00, gsl::narrow<DWORD>(out.size()));
  return out;
}

void TestKeyProvider::Add(
    const std::array<BYTE, thumbprint_size_value>& thumbprint,
    std::span<const BYTE> wrapped_fek, std::vector<BYTE> blob) {
  known_.push_back(
      {thumbprint, {wrapped_fek.begin(), wrapped_fek.end()}, std::move(blob)});
}

std::optional<std::vector<BYTE>>
    TestKeyProvider::UnwrapFek(std::span<const BYTE> thumbprint,
                               std::span<const BYTE> wrapped_fek) const {
  for (const Known& known : known_) {
    if (std::ranges::equal(thumbprint, known.thumbprint) &&
        std::ranges::equal(wrapped_fek, known.wrapped_fek)) {
      return known.blob;
    }
  }
  return std::nullopt;
}

}  // namespace NtfsBrowserTests
